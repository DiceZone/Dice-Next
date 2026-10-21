#include "core/mod/lua_plugin_manager.h"
#include "common/logger.h"
#include "common/utils.h"
#include "common/subprocess.h"
#include "common/bounded_regex.h"
#include "common/legacy_conf_value.h"
#include "common/legacy_mod_limit.h"
#include "common/legacy_deck_generator.h"
#include "common/sample_template.h"
#include "common/message_break.h"
#include "common/legacy_model.h"
#include "service/image_resource_service.h"
#include "../../../third_party/tomlplusplus/toml.hpp"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <sqlite3.h>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <sstream>
#include <random>
#include <ctime>
#include <algorithm>
#include <array>
#include <regex>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace fs = std::filesystem;
using nlohmann::json;

namespace dice {

// 路径 → UTF-8 窄串：Windows 上 path::string() 走 ANSI 代码页，文件名含 GBK 无映射
// 字符（emoji 等）会抛 system_error（Server 2012/2016 启动崩溃根因）。u8string 永不抛。
// ── Lua 安全工具前置声明（实现见文件后部）──
static std::string luaAuditTs();
static void luaAudit(const std::string&, const std::string&, const std::string&);
static std::vector<std::string> scanLuaRisks(const std::filesystem::path&);
static std::vector<std::string> scanLuaRisksText(const std::string&);
static std::string luaAuditMod(LuaPluginManager*);

static inline std::string dnx_u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
static inline fs::path dnx_path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

// 宽路径读全文（fs::path 构造 ifstream 走宽字符 API，任何文件名都能打开——
// 窄 .string() 对 emoji 等抛异常，u8 串又打不开 GBK 中文名，两头都不行）。
static inline bool dnx_readFile(const std::filesystem::path& p, std::string& out) {
    std::error_code ec;
    if (fs::file_size(p, ec) > 4 * 1024 * 1024 || ec) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

static bool dnx_isLinked(const fs::path& path) {
    std::error_code ec;
    if (fs::is_symlink(path, ec)) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
static bool dnx_safeUnder(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    const auto parent = fs::absolute(root, ec).lexically_normal();
    if (ec || parent.empty()) return false;
    const auto child = fs::absolute(path, ec).lexically_normal();
    if (ec || child == parent) return false;
    auto pi = parent.begin(), ci = child.begin();
    for (; pi != parent.end(); ++pi, ++ci) {
        if (ci == child.end()) return false;
#ifdef _WIN32
        auto a = pi->native(), b = ci->native();
        std::transform(a.begin(), a.end(), a.begin(), ::towlower);
        std::transform(b.begin(), b.end(), b.begin(), ::towlower);
        if (a != b) return false;
#else
        if (*pi != *ci) return false;
#endif
    }
    auto current = parent;
    if (dnx_isLinked(current)) return false;
    for (; ci != child.end(); ++ci) { current /= *ci; if (dnx_isLinked(current)) return false; }
    return true;
}
static std::vector<fs::path> dnx_ownedFiles(const fs::path& root) {
    std::vector<fs::path> files; std::error_code ec;
    if (!fs::is_directory(root, ec) || dnx_isLinked(root)) return files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && files.size() < 10000; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto name = dnx_u8str(it->path().filename());
        if (dnx_isLinked(it->path()) || (name.size() >= 9 && name.ends_with(".disabled")) || it.depth() >= 16) {
            it.disable_recursion_pending(); continue;
        }
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

static size_t dnx_utf8SequenceLength(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    int len = 0;
    uint32_t code = 0;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; code = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; code = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; code = c & 0x07; }
    else return 0;
    if (i + static_cast<size_t>(len) > s.size()) return 0;
    for (int k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
        if ((cc & 0xC0) != 0x80) return 0;
        code = (code << 6) | (cc & 0x3F);
    }
    const uint32_t minimum = len == 2 ? 0x80u : len == 3 ? 0x800u : 0x10000u;
    if (code < minimum || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return 0;
    return static_cast<size_t>(len);
}

static bool dnx_validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const size_t len = dnx_utf8SequenceLength(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

#ifdef _WIN32
static bool dnx_appendCp936(const char* bytes, int byteCount, std::string& out) {
    const int wideLen = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes, byteCount, nullptr, 0);
    if (wideLen <= 0) return false;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes, byteCount,
                            wide.data(), wideLen) != wideLen) return false;
    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen,
                                            nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return false;
    const size_t offset = out.size();
    out.resize(offset + static_cast<size_t>(utf8Len));
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, out.data() + offset,
                            utf8Len, nullptr, nullptr) != utf8Len) {
        out.resize(offset);
        return false;
    }
    return true;
}

static std::string dnx_cp936SourceToUtf8(const std::string& s) {
    std::string out;
    if (s.empty() || !dnx_appendCp936(s.data(), static_cast<int>(s.size()), out)) return {};
    return dnx_validUtf8(out) ? out : std::string();
}
#endif

// Runtime strings can combine UTF-8 literals with bytes read from an old CP936
// data file. Preserve already-valid UTF-8 spans and convert only invalid legacy
// byte pairs; this avoids fixing one half of a mixed reply while corrupting the other.
static std::string dnx_normalizeLuaText(const std::string& s) {
    if (dnx_validUtf8(s)) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t utf8Len = dnx_utf8SequenceLength(s, i);
        if (utf8Len != 0) {
            out.append(s, i, utf8Len);
            i += utf8Len;
            continue;
        }
#ifdef _WIN32
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        if (lead >= 0x81 && lead <= 0xFE && i + 1 < s.size()) {
            const unsigned char trail = static_cast<unsigned char>(s[i + 1]);
            if (trail >= 0x40 && trail <= 0xFE && trail != 0x7F &&
                dnx_appendCp936(s.data() + i, 2, out)) {
                i += 2;
                continue;
            }
        }
#endif
        out.append("\xEF\xBF\xBD");
        ++i;
    }
    return out;
}
// luaL_loadfile 的宽路径安全替代：读文本 + loadbuffer（chunkname 用 UTF-8 文件名）。
// 复刻 lua loadfile 的头部处理：剥 UTF-8 BOM（EF BB BF）与 shebang 行（loadbuffer 不做，
// 不剥则带 BOM 的社区插件全部报 unexpected symbol near '<\239>'）。
static inline int dnx_loadfile(lua_State* L, const std::filesystem::path& p) {
    std::string src;
    if (!dnx_readFile(p, src)) {
        lua_pushfstring(L, "cannot open %s", dnx_u8str(p.filename()).c_str());
        return LUA_ERRFILE;
    }
#ifdef _WIN32
    if (!dnx_validUtf8(src)) {
        const std::string converted = dnx_cp936SourceToUtf8(src);
        src = converted.empty() ? dnx_normalizeLuaText(src) : converted;
    }
#endif
    size_t off = 0;
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB
        && (unsigned char)src[2] == 0xBF) off = 3;                        // UTF-8 BOM
    if (off < src.size() && src[off] == '#') {                            // shebang 行
        while (off < src.size() && src[off] != '\n') ++off;
    }
    // chunkname 用完整路径（对齐 luaL_dofile 的 source="@fullpath"——有插件靠
    // debug.getinfo(1).source 定位自己的资源目录，只给文件名会破坏它们）。
    std::string chunk = "@" + dnx_u8str(p);
    return luaL_loadbuffer(L, src.data() + off, src.size() - off, chunk.c_str());
}

static thread_local unsigned dnx_luaProtected = 0;
static thread_local bool dnx_allowSleep = false;
static void dnx_executionHook(lua_State* L, lua_Debug*) {
    if (!ScriptExecution::expired()) return;
    // Yield from a host-owned root coroutine instead of throwing a catchable
    // Lua error: while true do pcall(...) end must not defeat the timeout.
    if (lua_isyieldable(L)) { lua_yield(L, 0); return; }
    luaL_error(L, "插件执行超时");
}
static int dnx_resume(lua_State* coroutine, lua_State* from, int arguments, int* results, bool allowSleep = false) {
    lua_sethook(coroutine, dnx_executionHook, LUA_MASKCOUNT, 10000);
    ++dnx_luaProtected;
    const bool previousSleep = dnx_allowSleep;
    dnx_allowSleep = allowSleep;
    const int status = lua_resume(coroutine, from, arguments, results);
    dnx_allowSleep = previousSleep;
    --dnx_luaProtected;
    if (ScriptExecution::expired()) {
        lua_settop(coroutine, 0);
        lua_pushliteral(coroutine, "插件执行超时，已中断本次操作（重载后可重新启用）");
        *results = 1;
        return LUA_ERRRUN; // never mistake a watchdog yield for sleepTime
    }
    return status;
}
static int dnx_pcall(lua_State* L, int arguments, int results) {
    const int base = lua_gettop(L) - arguments - 1;
    lua_State* coroutine = lua_newthread(L);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_xmove(L, coroutine, arguments + 1);
    int count = 0;
    int status = dnx_resume(coroutine, L, arguments, &count);
    if (status == LUA_YIELD) {
        lua_settop(coroutine, 0);
        lua_pushliteral(coroutine, "此调用不支持协程挂起，请在 msg_order 中使用 sleepTime");
        status = LUA_ERRRUN;
    }
    if (status == LUA_OK && (count > 1024 || !lua_checkstack(L, count))) {
        lua_settop(coroutine, 0);
        lua_pushliteral(coroutine, "Lua 返回值过多");
        status = LUA_ERRRUN;
    }
    if (status != LUA_OK) {
        const char* message = lua_tostring(coroutine, -1);
        luaL_traceback(L, coroutine, message ? message : "non-string Lua error", 0);
    } else {
        lua_xmove(coroutine, L, count);
        if (results != LUA_MULTRET) lua_settop(L, base + results);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return status;
}
static int dnx_dostring(lua_State* L, const char* source) {
    const int status = luaL_loadstring(L, source);
    return status == LUA_OK ? dnx_pcall(L, 0, LUA_MULTRET) : status;
}
static int dnx_sethook(lua_State* L) {
    return luaL_error(L, "debug.sethook 不可用：宿主执行时限保护不能被插件覆盖");
}
static int dnx_checkExecution(lua_State* L) {
    if (ScriptExecution::expired()) return luaL_error(L, "插件执行超时");
    return 0;
}
static int dnx_forbidExit(lua_State* L) {
    return luaL_error(L, "插件不能调用 os.exit 关闭整个骰子，请使用管理面板或托盘退出");
}
static int dnx_finalizer(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    // Lua disables hooks on the GC caller; a fresh host-owned coroutine still
    // has hooks and can yield out of an uncatchable watchdog timeout.
    if (dnx_pcall(L, 1, 0) != LUA_OK) return lua_error(L);
    return 0;
}
static int dnx_setmetatable(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_istable(L, 2)) {
        lua_pushliteral(L, "__gc"); lua_rawget(L, 2);
        if (lua_isfunction(L, -1) && !lua_iscfunction(L, -1)) {
            lua_pushcclosure(L, dnx_finalizer, 1);
            lua_pushliteral(L, "__gc"); lua_insert(L, -2); lua_rawset(L, 2);
        } else lua_pop(L, 1);
    }
    lua_settop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 2, 1); // preserved builtin also checks protected metatables
    return 1;
}
struct DnxPipe { luaL_Stream stream; int exitCode; };
static int dnx_processClose(lua_State* L) {
    auto* pipe = static_cast<DnxPipe*>(luaL_checkudata(L, 1, LUA_FILEHANDLE));
    const int status = std::fclose(pipe->stream.f);
    pipe->stream.f = nullptr;
    if (status != 0) return luaL_fileresult(L, 0, nullptr);
    if (pipe->exitCode == 0) lua_pushboolean(L, 1); else lua_pushnil(L);
    lua_pushliteral(L, "exit"); lua_pushinteger(L, pipe->exitCode);
    return 3;
}
static int dnx_popen(lua_State* L) {
    const char* command = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");
    if (std::string_view(mode) != "r") {
        lua_pushnil(L); lua_pushliteral(L, "受控 io.popen 仅支持读取模式 r"); return 2;
    }
    // Allocate the handle before spawning, so a Lua allocation failure cannot
    // leak a native process or FILE. The result owns only bounded captured data.
    auto* pipe = static_cast<DnxPipe*>(lua_newuserdatauv(L, sizeof(DnxPipe), 0));
    auto* stream = &pipe->stream;
    stream->f = nullptr; stream->closef = nullptr;
    luaL_setmetatable(L, LUA_FILEHANDLE);
    bool failed = false;
    {
        const auto result = proc::legacyShellCancellable(command, [] { return ScriptExecution::expired(); });
        pipe->exitCode = result.exitCode;
        failed = result.cancelled || result.truncated || result.exitCode < 0;
        if (!failed) {
            stream->f = std::tmpfile();
            failed = !stream->f;
            if (!failed) {
                failed = std::fwrite(result.output.data(), 1, result.output.size(), stream->f) != result.output.size();
                std::rewind(stream->f);
            }
        }
    }
    if (failed) {
        if (stream->f) { std::fclose(stream->f); stream->f = nullptr; }
        lua_pushnil(L); lua_pushliteral(L, "外部命令超时、输出过大或执行失败"); return 2;
    }
    stream->closef = dnx_processClose;
    return 1;
}
static int dnx_execute(lua_State* L) {
    if (lua_isnoneornil(L, 1)) { lua_pushboolean(L, 1); return 1; }
    int exitCode = -1;
    {
        const auto result = proc::legacyShellCancellable(luaL_checkstring(L, 1), [] { return ScriptExecution::expired(); });
        exitCode = result.cancelled ? 124 : result.exitCode;
    }
    if (exitCode == 0) lua_pushboolean(L, 1); else lua_pushnil(L);
    lua_pushliteral(L, "exit"); lua_pushinteger(L, exitCode);
    return 3;
}
static std::string dnx_coroutineError(lua_State* L) {
    // lua_resume does not accept an error handler. Its failed stack still keeps
    // the suspended call frames, so collect the traceback before releasing it.
    const char* message = lua_tostring(L, -1);
    luaL_traceback(L, L, message ? message : "non-string Lua error", 0);
    const char* trace = lua_tostring(L, -1);
    std::string result = trace ? trace : "Lua coroutine failed";
    lua_pop(L, 1);
    return result;
}
static inline int dnx_dofile(lua_State* L, const std::filesystem::path& p) {
    if (const int status = dnx_loadfile(L, p); status != LUA_OK) return status;
    return dnx_pcall(L, 0, LUA_MULTRET);
}

// ─── helpers ─────────────────────────────────────────────────
static LuaPluginManager* mgrOf(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "DiceMgr");
    auto* m = static_cast<LuaPluginManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return m;
}
static std::string argStr(lua_State* L, int i) {
    if (lua_isnil(L, i) || lua_isnone(L, i)) return "";
    size_t n = 0; const char* s = lua_tolstring(L, i, &n);
    return s ? std::string(s, n) : std::string();
}

// 原版 Dice! 对 data/plugin 下的单文件 Lua 每次调用都会新建 LuaState、重新执行
// 文件顶层，再调用 msg_order/task_call 指向的函数。用带 _G 回退的独立 _ENV
// 复刻这一生命周期：HTTP 等动态顶层数据会刷新，同时不会把插件全局变量泄漏给
// 其他插件。成功时仅把 env 留在栈顶；失败时仅把错误对象留在栈顶。
static int dnx_loadLegacyEnvironment(lua_State* L, const fs::path& path) {
    lua_newtable(L);
    const int env = lua_absindex(L, -1);

    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");
    lua_newtable(L);
    lua_setfield(L, env, "msg_order");
    lua_newtable(L);
    lua_setfield(L, env, "task_call");

    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);

    const int status = dnx_loadfile(L, path);
    if (status != LUA_OK) {
        lua_remove(L, env);
        return status;
    }
    lua_pushvalue(L, env);
    if (lua_setupvalue(L, -2, 1) == nullptr) {
        lua_pop(L, 1);  // loaded chunk
        lua_remove(L, env);
        lua_pushliteral(L, "legacy plugin chunk has no _ENV upvalue");
        return LUA_ERRRUN;
    }
    const int callStatus = dnx_pcall(L, 0, 0);
    if (callStatus != LUA_OK) lua_remove(L, env);
    return callStatus;
}

// Directory script/text entrypoints also ran in a fresh LuaState in Dice!.
// Keep their temporary globals per invocation (and per suspended coroutine),
// while loadLua deliberately inherits this same environment for helper scripts.
// Do not rebind registered function closures: Next preserves their captured
// locals. This is lifecycle compatibility, not a security sandbox for Lua.
static void dnx_isolateScriptChunk(lua_State* L) {
    const int chunk = lua_absindex(L, -1);
    lua_newtable(L);
    const int env = lua_absindex(L, -1);
    lua_pushvalue(L, env); lua_setfield(L, env, "_G");
    for (const char* key : {"msg", "event"}) {
        lua_getglobal(L, key); lua_setfield(L, env, key);
    }
    lua_newtable(L);
    lua_pushglobaltable(L); lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);
    if (!lua_setupvalue(L, chunk, 1)) lua_pop(L, 1);
}

// Old single-file plugins often ship a generated cache containing absolute paths
// from the author's Dice! installation. Dice! resolved those paths relative to
// the active plugin directory after the bundle was moved. Preserve that useful
// behaviour, but only for a traversal-free tail below plugin/ (or a genuinely
// relative path), so this fallback cannot widen filesystem access. ResourceSearchEngine
// additionally stores Chinese filenames in CP936 inside its index/cache. The server
// runs the Windows CRT in UTF-8 mode, therefore normalize that tail before retrying.
static int l_resolvePluginFile(lua_State* L) {
    auto* m = mgrOf(L);
    const std::string raw = argStr(L, 1);
    if (!m || m->loadingModDir_.empty() || raw.empty()) {
        lua_pushnil(L);
        return 1;
    }

    std::string slash = raw;
    std::replace(slash.begin(), slash.end(), '\\', '/');
    std::string tail;
    const size_t pluginMarker = slash.rfind("/plugin/");
    if (pluginMarker != std::string::npos) {
        // Keep the original bytes for code-page conversion. A CP936 trail byte
        // may itself be 0x5c, so converting every backslash before decoding can
        // silently split a Chinese filename.
        tail = raw.substr(pluginMarker + std::string("/plugin/").size());
    } else {
        const bool rooted = !slash.empty() && slash.front() == '/';
        const bool drive = slash.size() >= 2 && slash[1] == ':';
        if (!rooted && !drive) tail = raw;
    }
    if (tail.empty()) {
        lua_pushnil(L);
        return 1;
    }
    std::string checkedTail = tail;
    std::replace(checkedTail.begin(), checkedTail.end(), '\\', '/');
    size_t start = 0;
    while (start <= checkedTail.size()) {
        const size_t end = checkedTail.find('/', start);
        const std::string part = checkedTail.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (part == ".." || part.find(':') != std::string::npos) {
            lua_pushnil(L);
            return 1;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
#ifdef _WIN32
    if (!dnx_validUtf8(tail)) {
        const std::string cp936Tail = dnx_cp936SourceToUtf8(tail);
        const std::string mixedTail = dnx_normalizeLuaText(tail);
        const auto existsBelowPlugin = [&](const std::string& candidateTail) {
            std::string candidate = m->loadingModDir_;
            if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
                candidate.push_back(fs::path::preferred_separator);
            candidate += candidateTail;
            const fs::path candidatePath(
                std::u8string(candidate.begin(), candidate.end()));
            std::error_code ec;
            return fs::is_regular_file(candidatePath, ec);
        };

        // A legacy index is normally wholly CP936 after its ASCII directory
        // prefix. Some community plugins instead concatenate UTF-8 and CP936
        // fragments. Prefer whichever decoding maps to an existing file; for a
        // new write target, the full CP936 interpretation is the legacy default.
        if (!cp936Tail.empty() && existsBelowPlugin(cp936Tail)) tail = cp936Tail;
        else if (existsBelowPlugin(mixedTail)) tail = mixedTail;
        else tail = cp936Tail.empty() ? mixedTail : cp936Tail;
    }
    std::replace(tail.begin(), tail.end(), '/', '\\');
#else
    tail = std::move(checkedTail);
#endif
    std::string mapped = m->loadingModDir_;
    if (!mapped.empty() && mapped.back() != '/' && mapped.back() != '\\')
        mapped.push_back(fs::path::preferred_separator);
    mapped += tail;
    mapped = image_resources::resolvePath(mapped);
    lua_pushlstring(L, mapped.data(), mapped.size());
    return 1;
}
// Only an already-managed image alias is redirected. Arbitrary plugin file IO
// and new paths keep their existing semantics and filesystem permissions.
static int l_resolveImageResource(lua_State* L) {
    try {
        const auto raw=argStr(L,1);
        if(auto resources=image_resources::current()) {
            if(auto path=resources->resolve(raw)) {const auto value=image_resources::utf8(*path);lua_pushlstring(L,value.data(),value.size());return 1;}
            if(resources->known(raw)) {const auto unavailable=image_resources::utf8(resources->root()/".unavailable"/"recycled-resource");lua_pushlstring(L,unavailable.data(),unavailable.size());return 1;}
        }
    } catch(...) {}
    lua_pushnil(L);return 1;
}
// 取配置键，并解析 &field（经 speech 模板 → 实际字段名）。
static std::string resolvedKey(LuaPluginManager* m, lua_State* L, int i) {
    std::string key = argStr(L, i);
    if (m && !key.empty() && key[0] == '&') key = m->resolveAmp(key.substr(1));
    return key;
}
// 把字符串值压栈：能转成数字就压数字（好感等算术），否则压字符串。
static void jsonToLua(lua_State* L, const json& j, const std::string& scope = {},
                      const std::string& key = {}, const std::string& path = {}, const json& card = json());
static json luaToJson(lua_State* L, int idx, int depth = 0);
static int saveLegacySet(lua_State* L) {
    auto* manager = mgrOf(L); if (!manager) return 0;
    const auto scope = argStr(L, lua_upvalueindex(1)), key = argStr(L, lua_upvalueindex(2));
    const auto path = argStr(L, lua_upvalueindex(3));
    const auto card = json::parse(argStr(L, lua_upvalueindex(4)), nullptr, false);
    json root;
    try {
        if (card.is_object()) {
            if (manager->hasPlayerCardBridge()) {
                if (!manager->playerCardRead(card["uid"], card["scope"], card["byName"], card["field"], root)) return 0;
            } else {
                const auto data = json::parse(manager->cardLoad(card["uid"], card["scope"]), nullptr, false);
                if (!data.is_object() || !data.contains(card["field"].get<std::string>())) return 0;
                root = data[card["field"].get<std::string>()];
            }
        } else if (!legacy_conf::decode(manager->confGet(scope, key), root)) return 0;
        const json::json_pointer pointer(path);
        if (!root.contains(pointer) || !legacy_conf::isSet(root.at(pointer))) return 0;
        auto values = luaToJson(L, 1);
        if (values.empty()) values = json::array();
        if (!values.is_array()) return 0;
        root[pointer] = legacy_conf::setValue(values);
        if (card.is_object()) {
            if (manager->hasPlayerCardBridge())
                manager->playerCardWrite(card["uid"], card["scope"], card["byName"], card["field"], root);
            else {
                auto data = json::parse(manager->cardLoad(card["uid"], card["scope"]), nullptr, false);
                if (!data.is_object()) return 0;
                data[card["field"].get<std::string>()] = root;
                manager->cardSave(card["uid"], card["scope"], data.dump());
            }
        } else manager->confSet(scope, key, legacy_conf::encode(root));
    } catch (...) { return 0; }
    return 0;
}
static std::string legacyJsonPath(const std::string& path, const std::string& key) {
    std::string escaped;
    for (const char ch : key) {
        if (ch == '~') escaped += "~0"; else if (ch == '/') escaped += "~1"; else escaped += ch;
    }
    return path + "/" + escaped;
}
static void pushConfValue(lua_State* L, const std::string& v, const std::string& scope = {}, const std::string& key = {}) {
    json typed;
    if (legacy_conf::decode(v, typed)) { jsonToLua(L, typed, scope, key); return; }
    if (v == "true") { lua_pushboolean(L, 1); return; }
    if (v == "false") { lua_pushboolean(L, 0); return; }
    if (!v.empty()) {
        char* end = nullptr;
        long long ll = std::strtoll(v.c_str(), &end, 10);
        if (end && *end == '\0') { lua_pushinteger(L, (lua_Integer)ll); return; }   // 整数（不带 .0）
        end = nullptr;
        double d = std::strtod(v.c_str(), &end);
        if (end && *end == '\0') { lua_pushnumber(L, d); return; }                  // 浮点
    }
    lua_pushlstring(L, v.data(), v.size());
}
static std::string todayStr() {
    return dice::utils::formatTimeInTimezone(std::time(nullptr), "%Y%m%d");
}

// ── Lua ↔ JSON（人物卡可存嵌套结构：数组/对象）──────────────────
static void jsonToLua(lua_State* L, const json& j, const std::string& scope,
                      const std::string& key, const std::string& path, const json& card) {
    if (legacy_conf::isFunction(j)) { lua_pushnil(L); return; } // never execute legacy bytecode
    if (legacy_conf::isSet(j)) {
        lua_getglobal(L, "Set");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return; }
        lua_getfield(L, -1, "new");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 2); lua_pushnil(L); return; }
        jsonToLua(L, j["values"]);
        if ((!scope.empty() && !key.empty()) || card.is_object()) {
            lua_pushlstring(L, scope.data(), scope.size()); lua_pushlstring(L, key.data(), key.size());
            lua_pushlstring(L, path.data(), path.size());
            const auto binding = card.dump(); lua_pushlstring(L, binding.data(), binding.size());
            lua_pushcclosure(L, saveLegacySet, 4);
        } else lua_pushnil(L);
        if (dnx_pcall(L, 2, 1) == LUA_OK) lua_remove(L, -2);
        else { lua_pop(L, 2); lua_pushnil(L); }
        return;
    }
    switch (j.type()) {
        case json::value_t::null: lua_pushnil(L); break;
        case json::value_t::boolean: lua_pushboolean(L, j.get<bool>()); break;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned: lua_pushinteger(L, (lua_Integer)j.get<long long>()); break;
        case json::value_t::number_float: lua_pushnumber(L, j.get<double>()); break;
        case json::value_t::string: { auto s = j.get<std::string>(); lua_pushlstring(L, s.data(), s.size()); break; }
        case json::value_t::array: {
            lua_newtable(L); int i = 1;
            for (auto& e : j) {
                jsonToLua(L, e, scope, key, legacyJsonPath(path, std::to_string(i - 1)), card);
                lua_rawseti(L, -2, i++);
            }
            break;
        }
        case json::value_t::object: {
            lua_newtable(L);
            for (auto it = j.begin(); it != j.end(); ++it) {
                jsonToLua(L, it.value(), scope, key, legacyJsonPath(path, it.key()), card);
                lua_setfield(L, -2, it.key().c_str());
            }
            break;
        }
        default: lua_pushnil(L); break;
    }
}
static json luaToJson(lua_State* L, int idx, int depth) {
    if (depth > 32) return nullptr;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL: case LUA_TNONE: return nullptr;
        case LUA_TBOOLEAN: return (bool)lua_toboolean(L, idx);
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) return (long long)lua_tointeger(L, idx);
            return lua_tonumber(L, idx);
        case LUA_TSTRING: { size_t n = 0; const char* s = lua_tolstring(L, idx, &n); return std::string(s, n); }
        case LUA_TTABLE: {
            bool isSet = false;
            if (lua_getmetatable(L, idx)) {
                lua_getfield(L, -1, "__dnx_legacy_set"); isSet = lua_toboolean(L, -1) != 0; lua_pop(L, 2);
            }
            if (isSet) {
                json values = json::array();
                lua_pushliteral(L, "__d"); lua_rawget(L, idx);
                if (lua_istable(L, -1)) {
                    lua_pushnil(L);
                    while (lua_next(L, -2)) {
                        values.push_back(luaToJson(L, -2, depth + 1)); lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
                return legacy_conf::setValue(values);
            }
            // 判定数组（连续 1..n 整数键）还是对象。
            lua_Integer len = (lua_Integer)lua_rawlen(L, idx);
            bool isArray = len > 0;
            if (isArray) {   // 确认无非序列键
                lua_pushnil(L);
                while (lua_next(L, idx)) {
                    bool seqKey = lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 && lua_tointeger(L, -2) <= len;
                    lua_pop(L, 1);
                    if (!seqKey) { isArray = false; lua_pop(L, 1); break; }
                }
            }
            if (isArray) {
                json arr = json::array();
                for (lua_Integer i = 1; i <= len; ++i) { lua_rawgeti(L, idx, i); arr.push_back(luaToJson(L, -1, depth + 1)); lua_pop(L, 1); }
                return arr;
            }
            json obj = json::object();
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                lua_pushvalue(L, -2);                       // 复制 key
                std::string k; { size_t n = 0; const char* s = lua_tolstring(L, -1, &n); if (s) k.assign(s, n); }
                lua_pop(L, 1);
                if (!k.empty()) obj[k] = luaToJson(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return obj;
        }
        default: return nullptr;
    }
}

// lua_tolstring 不接受 boolean；旧实现会把 true/false 误写成空串（等价于删除）。
static std::string confValueString(lua_State* L, int idx) {
    if (lua_isboolean(L, idx)) return lua_toboolean(L, idx) ? "true" : "false";
    if (lua_istable(L, idx)) return legacy_conf::encode(luaToJson(L, idx));
    if (lua_type(L, idx) == LUA_TSTRING) return legacy_conf::encode(argStr(L, idx));
    return argStr(L, idx);
}

// ── 人物卡 C 函数（getPlayerCard 对象代理 + getPlayerCardAttr/setPlayerCardAttr）──
// 代理 __index：读字段 → jsonToLua。upvalue: 1=uid, 2=群作用域或卡名, 3=是否按卡名。
static bool playerCardByNameArg(lua_State* L, int arg, int overrideArg = 0) {
    if (overrideArg > 0 && lua_gettop(L) >= overrideArg && lua_isboolean(L, overrideArg))
        return lua_toboolean(L, overrideArg) != 0;
    return lua_type(L, arg) == LUA_TSTRING;
}
static int card_index(lua_State* L) {
    auto* m = mgrOf(L);
    std::string uid = argStr(L, lua_upvalueindex(1)), scope = argStr(L, lua_upvalueindex(2));
    const bool byName = lua_toboolean(L, lua_upvalueindex(3)) != 0;
    std::string key = argStr(L, 2);
    if (!m) { lua_pushnil(L); return 1; }
    if (m->hasPlayerCardBridge()) {
        json value;
        if (m->playerCardRead(uid, scope, byName, key, value))
            jsonToLua(L, value, {}, {}, {}, json{{"uid", uid}, {"scope", scope}, {"byName", byName}, {"field", key}});
        else lua_pushnil(L);
        return 1;
    }
    json j = json::parse(m->cardLoad(uid, scope), nullptr, false);
    if (j.is_object() && j.contains(key))
        jsonToLua(L, j[key], {}, {}, {}, json{{"uid", uid}, {"scope", scope}, {"byName", byName}, {"field", key}});
    else lua_pushnil(L);
    return 1;
}
// 代理 __newindex：写字段 → luaToJson 后持久化。
static int card_newindex(lua_State* L) {
    auto* m = mgrOf(L);
    std::string uid = argStr(L, lua_upvalueindex(1)), scope = argStr(L, lua_upvalueindex(2));
    const bool byName = lua_toboolean(L, lua_upvalueindex(3)) != 0;
    std::string key = argStr(L, 2);
    if (!m || key.empty()) return 0;
    if (m->hasPlayerCardBridge()) {
        m->playerCardWrite(uid, scope, byName, key, luaToJson(L, 3));
        return 0;
    }
    json j = json::parse(m->cardLoad(uid, scope), nullptr, false);
    if (!j.is_object()) j = json::object();
    if (lua_isnil(L, 3)) j.erase(key); else j[key] = luaToJson(L, 3);
    m->cardSave(uid, scope, j.dump());
    return 0;
}
static int l_getPlayerCard(lua_State* L) {
    std::string uid = argStr(L, 1), scope = argStr(L, 2); if (scope.empty()) scope = "0";
    const bool byName = playerCardByNameArg(L, 2);
    // 旧版返回 Actor，不是只可下标访问的裸表；这样 get/set/rollDice/lock/locked 等
    // 方法也与 getPlayerCardAttr/setPlayerCardAttr 操作同一张真实人物卡。
    lua_getglobal(L, "Actor");
    if (lua_isfunction(L, -1)) {
        lua_pushlstring(L, uid.data(), uid.size());
        lua_pushlstring(L, scope.data(), scope.size());
        lua_pushboolean(L, byName ? 1 : 0);
        if (dnx_pcall(L, 3, 1) == LUA_OK) return 1;
        DICE_LOG_ERROR("[lua] getPlayerCard Actor construction failed: {}", argStr(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    lua_pop(L, 1);
    lua_newtable(L);                                    // 代理表（空，读写全走元方法）
    lua_newtable(L);                                    // 元表
    lua_pushstring(L, uid.c_str()); lua_pushstring(L, scope.c_str());
    lua_pushboolean(L, byName ? 1 : 0);
    lua_pushcclosure(L, card_index, 3); lua_setfield(L, -2, "__index");
    lua_pushstring(L, uid.c_str()); lua_pushstring(L, scope.c_str());
    lua_pushboolean(L, byName ? 1 : 0);
    lua_pushcclosure(L, card_newindex, 3); lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    return 1;
}
static int l_getPlayerCardAttr(lua_State* L) {
    auto* m = mgrOf(L); if (!m) { lua_pushnil(L); return 1; }
    std::string uid = argStr(L, 1), scope = argStr(L, 2), attr = argStr(L, 3);
    if (scope.empty()) scope = "0";
    const bool byName = playerCardByNameArg(L, 2, 5);
    if (m->hasPlayerCardBridge()) {
        json value;
        if (m->playerCardRead(uid, scope, byName, attr, value)) {
            jsonToLua(L, value, {}, {}, {}, json{{"uid", uid}, {"scope", scope}, {"byName", byName}, {"field", attr}}); return 1;
        }
        if (m->playerCardRead(uid, scope, byName, "&" + attr, value)) {
            jsonToLua(L, value, {}, {}, {}, json{{"uid", uid}, {"scope", scope}, {"byName", byName}, {"field", "&" + attr}}); return 1;
        }
        if (lua_gettop(L) >= 4 && !lua_isnoneornil(L, 4)) { lua_pushvalue(L, 4); return 1; }
        lua_pushnil(L); return 1;
    }
    json j = json::parse(m->cardLoad(uid, scope), nullptr, false);
    if (j.is_object() && j.contains(attr)) {
        jsonToLua(L, j[attr], {}, {}, {}, json{{"uid", uid}, {"scope", scope}, {"byName", byName}, {"field", attr}}); return 1;
    }
    if (j.is_object() && j.contains("&" + attr)) {
        jsonToLua(L, j["&" + attr], {}, {}, {}, json{{"uid", uid}, {"scope", scope}, {"byName", byName}, {"field", "&" + attr}}); return 1;
    }
    if (lua_gettop(L) >= 4 && !lua_isnoneornil(L, 4)) { lua_pushvalue(L, 4); return 1; }   // 默认值
    lua_pushnil(L); return 1;
}
static int l_setPlayerCardAttr(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string uid = argStr(L, 1), scope = argStr(L, 2), attr = argStr(L, 3);
    if (scope.empty()) scope = "0";
    const bool byName = playerCardByNameArg(L, 2, 5);
    if (attr.empty()) return 0;
    if (m->hasPlayerCardBridge()) {
        m->playerCardWrite(uid, scope, byName, attr, luaToJson(L, 4));
        return 0;
    }
    json j = json::parse(m->cardLoad(uid, scope), nullptr, false);
    if (!j.is_object()) j = json::object();
    if (lua_isnoneornil(L, 4)) j.erase(attr); else j[attr] = luaToJson(L, 4);
    m->cardSave(uid, scope, j.dump());
    return 0;
}

// 好感度公开接口。platform 省略时继承当前 msg_order / echo 的消息平台。
static bool luaFavorTarget(lua_State* L, LuaPluginManager* m, int platformArg,
                           std::string& platform, std::string& uid) {
    uid = argStr(L, 1);
    platform = (lua_gettop(L) >= platformArg && !lua_isnoneornil(L, platformArg))
        ? argStr(L, platformArg) : std::string();
    platform = m ? m->favorPlatform(platform) : std::string();
    return m && m->hasFavorBridge() && !platform.empty() && !uid.empty();
}
static int l_getFavor(lua_State* L) {
    auto* m = mgrOf(L); std::string platform, uid;
    if (!luaFavorTarget(L, m, 2, platform, uid)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, m->favorGet(platform, uid)); return 1;
}
static int l_setFavor(lua_State* L) {
    if (lua_gettop(L) < 2 || !lua_isnumber(L, 2)) { lua_pushnil(L); return 1; }
    auto* m = mgrOf(L); std::string platform, uid;
    if (!luaFavorTarget(L, m, 3, platform, uid)) { lua_pushnil(L); return 1; }
    const int value = static_cast<int>(lua_tointeger(L, 2));
    lua_pushinteger(L, m->favorSet(platform, uid, value)); return 1;
}
static int l_addFavor(lua_State* L) {
    if (lua_gettop(L) < 2 || !lua_isnumber(L, 2)) { lua_pushnil(L); return 1; }
    auto* m = mgrOf(L); std::string platform, uid;
    if (!luaFavorTarget(L, m, 3, platform, uid)) { lua_pushnil(L); return 1; }
    const int delta = static_cast<int>(lua_tointeger(L, 2));
    lua_pushinteger(L, m->favorAdd(platform, uid, delta)); return 1;
}
static int l_growFavor(lua_State* L) {
    auto* m = mgrOf(L); std::string platform, uid;
    if (!luaFavorTarget(L, m, 2, platform, uid)) {
        lua_pushboolean(L, 0); lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 3;
    }
    const auto [delta, value] = m->favorGrow(platform, uid);
    lua_pushboolean(L, delta >= 0 ? 1 : 0);
    lua_pushinteger(L, delta >= 0 ? delta : 0);
    lua_pushinteger(L, value);
    return 3;
}

// 原版 CharaCard::lock/unlock 移植：锁写在「真人物卡」（CharacterCardStore，.st 用的那套）
// 上，经 main.cpp 注入的 cardLock 桥接生效。lockPlayerCard(uid, gid, key) /
// unlockPlayerCard(uid, gid, key)，key = "w"(锁写：.st set/del/clr 拒) / "r"(锁读：.st show 拒)。
static int cardLockToggle(lua_State* L, bool on) {
    auto* m = mgrOf(L);
    std::string uid = argStr(L, 1), scope = argStr(L, 2), key = argStr(L, 3);
    if (!m || uid.empty() || key.empty()) { lua_pushboolean(L, 0); return 1; }
    const bool byName = playerCardByNameArg(L, 2, 4);
    if (m->hasPlayerCardBridge()) {
        lua_pushboolean(L, m->playerCardLock(uid, scope, byName, key, on) ? 1 : 0);
        return 1;
    }
    lua_pushboolean(L, m->cardLock(uid, scope, key, on) ? 1 : 0);
    return 1;
}
static int l_lockPlayerCard(lua_State* L)   { return cardLockToggle(L, true); }
static int l_unlockPlayerCard(lua_State* L) { return cardLockToggle(L, false); }
static int l_isPlayerCardLocked(lua_State* L) {
    auto* m = mgrOf(L);
    std::string uid = argStr(L, 1), scope = argStr(L, 2), key = argStr(L, 3);
    const bool byName = playerCardByNameArg(L, 2, 4);
    lua_pushboolean(L, m && m->hasPlayerCardBridge() && m->playerCardLocked(uid, scope, byName, key));
    return 1;
}

// ─── core C functions (复刻原版 DiceLua 全局，阶段一子集) ─────────
static int l_log(lua_State* L) {
    std::string s; int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) s += '\t';
        size_t length = 0;
        const char* value = luaL_tolstring(L, i, &length);
        if (value) s.append(value, length);
        lua_pop(L, 1);
    }
    const auto level = lua_isinteger(L, lua_upvalueindex(1))
        ? static_cast<LogLevel>(lua_tointeger(L, lua_upvalueindex(1))) : LogLevel::Info;
    logPluginText(level, dnx_normalizeLuaText(s));
    return 0;
}
static int l_ranint(lua_State* L) {
    long a = (long)luaL_optinteger(L, 1, 1), b = (long)luaL_optinteger(L, 2, 100);
    if (a > b) std::swap(a, b);
    static thread_local std::mt19937_64 rng{ std::random_device{}() };
    lua_pushinteger(L, (lua_Integer)(std::uniform_int_distribution<long>(a, b)(rng)));
    return 1;
}
static int l_getUserConf(lua_State* L) {
    auto* m = mgrOf(L); if (!m) { lua_pushnil(L); return 1; }
    std::string key = resolvedKey(m, L, 2);
    if (lua_isnil(L, 1)) {   // getUserConf(nil, field) → {uid → val}（rank_user）
        if (key.empty()) { lua_pushnil(L); return 1; }
        lua_newtable(L);
        for (auto& [id, val] : m->confAllUsers(key)) { pushConfValue(L, val, "u:" + id, key); lua_setfield(L, -2, id.c_str()); }
        return 1;
    }
    std::string uid = argStr(L, 1);
    if (key.empty()) { lua_pushnil(L); return 1; }   // uid 可空（如 getDiceQQ() 未配=机器人自身CD），仍走查询+默认
    std::string v = m->confGet("u:" + uid, key);
    if (v.empty() && !m->/*has*/confHas("u:" + uid, key)) {   // 未存 → 返回默认(arg3)或 nil
        if (lua_gettop(L) >= 3 && !lua_isnoneornil(L, 3)) { lua_pushvalue(L, 3); return 1; }
        lua_pushnil(L); return 1;
    }
    pushConfValue(L, v, "u:" + uid, key); return 1;
}
static int l_setUserConf(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string uid = argStr(L, 1), key = resolvedKey(m, L, 2);
    if (key.empty()) return 0;   // uid 可空（机器人自身存储）
    if (lua_isnoneornil(L, 3)) m->confSet("u:" + uid, key, std::string());   // nil → 删除
    else m->confSet("u:" + uid, key, confValueString(L, 3));
    return 0;
}
static int l_getGroupConf(lua_State* L) {
    auto* m = mgrOf(L); if (!m) { lua_pushnil(L); return 1; }
    std::string gid = argStr(L, 1), key = resolvedKey(m, L, 2);
    std::string v = m->confGet("g:" + gid, key);
    if (v.empty() && !m->confHas("g:" + gid, key)) {
        if (lua_gettop(L) >= 3 && !lua_isnoneornil(L, 3)) { lua_pushvalue(L, 3); return 1; }
        lua_pushnil(L); return 1;
    }
    pushConfValue(L, v, "g:" + gid, key); return 1;
}
static int l_setGroupConf(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string gid = argStr(L, 1), key = resolvedKey(m, L, 2);
    if (gid.empty() || key.empty()) return 0;
    if (lua_isnoneornil(L, 3)) m->confSet("g:" + gid, key, std::string());
    else m->confSet("g:" + gid, key, confValueString(L, 3));
    return 0;
}
static int l_getUserToday(lua_State* L) {
    auto* m = mgrOf(L); if (!m) { lua_pushnil(L); return 1; }
    std::string uid = argStr(L, 1), key = argStr(L, 2);
    std::string scope = "today:" + todayStr() + ":" + uid;
    std::string v = m->confGet(scope, key);
    if (v.empty() && !m->confHas(scope, key)) {
        if (lua_gettop(L) >= 3 && !lua_isnoneornil(L, 3)) { lua_pushvalue(L, 3); return 1; }
        lua_pushinteger(L, 0); return 1;   // 原版未命中返回 0（DiceLua.cpp 948，计数场景免 nil 算术崩）
    }
    pushConfValue(L, v, scope, key); return 1;
}
static int l_setUserToday(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string uid = argStr(L, 1), key = argStr(L, 2);
    std::string scope = "today:" + todayStr() + ":" + uid;
    if (lua_isnoneornil(L, 3)) m->confSet(scope, key, std::string());
    else m->confSet(scope, key, confValueString(L, 3));
    return 0;
}
static int l_getDiceDir(lua_State* L) {
    // 返回 mod/plugin 的父目录（如 data），让插件的 getDiceDir().."/plugin/x" 解析正确。
    auto* m = mgrOf(L); std::error_code ec;
    fs::path base = (m && !m->modDir().empty())
        ? fs::absolute(dnx_path(m->modDir()).parent_path(), ec) : fs::current_path(ec);
    std::string s = base.string();
    for (auto& c : s) if (c == '\\') c = '/';   // 统一正斜杠（插件常拼接路径）
    lua_pushstring(L, s.c_str()); return 1;
}
// getDiceQQ(): resolve the triggering account, including delayed continuations.
static int l_getDiceQQ(lua_State* L) {
    auto* m = mgrOf(L);
    std::string id = m ? m->botId() : std::string();
    lua_pushstring(L, id.c_str()); return 1;
}
// getSelfData([key])：机器人自身数据。简化为空表（多数插件读字段，缺则 nil，不崩）。
// ── SelfData：自定义数据文件（原版 getSelfData(name) → 读写自动落盘 userdata）。
// C 侧仅提供 JSON 载入/保存；活对象由 bootstrap 的 SelfData proxy 实现。
static std::filesystem::path selfDataPath(const std::string& name) {
    std::string base = name;
    if (auto p = base.find_last_of("/\\"); p != std::string::npos) base = base.substr(p + 1);
    std::string suffix = base.size() >= 5 ? base.substr(base.size() - 5) : std::string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (suffix == ".json") base.replace(base.size() - 5, 5, ".json");
    else base += ".json";
    return std::filesystem::path("data/self_data") / std::filesystem::path(std::u8string(base.begin(), base.end()));
}
static int l_sdLoad(lua_State* L) {           // __dnx_sd_load(name) → json 串
    std::string name = argStr(L, 1);
    if (name.empty()) { lua_pushstring(L, "{}"); return 1; }
    std::ifstream f(selfDataPath(name), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.empty()) s = "{}";
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
static int l_sdSave(lua_State* L) {           // __dnx_sd_save(name, json)
    std::string name = argStr(L, 1), body = argStr(L, 2);
    if (name.empty()) return 0;
    std::error_code ec; std::filesystem::create_directories("data/self_data", ec);
    std::ofstream f(selfDataPath(name), std::ios::binary);
    f.write(body.data(), (std::streamsize)body.size());
    return 0;
}
// getSelfData 本体由 bootstrap 重定义为 SelfData(name) proxy；此处留兜底（bootstrap 失败时空表）。
static int l_getSelfData(lua_State* L) { lua_newtable(L); return 1; }

// ── __dnx_roll(expr, defaultFace) → {expr,sum,expansion}|{expr,error}（原版 Actor_rollDice 的引擎侧）──
static int l_rollExpr(lua_State* L) {
    auto* m = mgrOf(L);
    std::string expr = argStr(L, 1);
    int face = (int)luaL_optinteger(L, 2, 100);
    lua_newtable(L);
    if (!m || !m->roller_) { lua_pushinteger(L, -1); lua_setfield(L, -2, "error"); return 1; }
    auto r = m->roller_(expr, face > 0 ? face : 100);
    lua_pushlstring(L, r.expr.data(), r.expr.size()); lua_setfield(L, -2, "expr");
    if (r.ok) {
        lua_pushinteger(L, (lua_Integer)r.sum); lua_setfield(L, -2, "sum");
        lua_pushlstring(L, r.expansion.data(), r.expansion.size()); lua_setfield(L, -2, "expansion");
    } else {
        lua_pushlstring(L, r.err.data(), r.err.size()); lua_setfield(L, -2, "error");
    }
    return 1;
}

// ── askExtra(data)：平台扩展查询（原版 DD::getExtra）。data=table→JSON 或字符串。──
static int l_askExtra(lua_State* L) {
    auto* m = mgrOf(L);
    std::string data;
    if (lua_istable(L, 1)) { data = luaToJson(L, 1).dump(); }
    else data = argStr(L, 1);
    if (data.empty() || !m || !m->askExtra_) return 0;
    std::string ret = m->askExtra_(data);
    if (ret.empty()) return 0;
    json j = json::parse(ret, nullptr, false);
    if (j.is_discarded()) { lua_pushlstring(L, ret.data(), ret.size()); return 1; }
    jsonToLua(L, j);
    return 1;
}

// ── __dnx_fmt(text, uid, gid, nick)：msg:format 的模板桥（原版 fmt->format(msg, obj)）──
static safe_template::Read dnx_messageReader(lua_State* L, int index) {
    if (!lua_istable(L, index)) return {};
    const int context = lua_absindex(L, index);
    return [L, context](const std::string& key) -> std::optional<std::string> {
        const int top = lua_gettop(L);
        lua_getglobal(L, "__dnx_context_get");
        lua_pushvalue(L, context);
        lua_pushlstring(L, key.data(), key.size());
        std::optional<std::string> result;
        if (dnx_pcall(L, 2, 1) == LUA_OK) {
            if (lua_isboolean(L, -1)) result = lua_toboolean(L, -1) ? "true" : "false";
            else if (lua_isstring(L, -1)) result = dnx_normalizeLuaText(argStr(L, -1));
        }
        lua_settop(L, top);
        return result;
    };
}

static safe_template::Length dnx_messageLength(lua_State* L, int index) {
    if (!lua_istable(L, index)) return {};
    const int context = lua_absindex(L, index);
    return [L, context](const std::string& key) -> std::optional<size_t> {
        const int top = lua_gettop(L);
        lua_getglobal(L, "__dnx_context_get"); lua_pushvalue(L, context);
        lua_pushlstring(L, key.data(), key.size());
        std::optional<size_t> length;
        if (dnx_pcall(L, 2, 1) == LUA_OK && !lua_isnil(L, -1)) {
            length = 0;
            if (lua_type(L, -1) == LUA_TSTRING) length = lua_rawlen(L, -1);
            else if (lua_istable(L, -1)) {
                // Never call __len, __pairs, tostring or a plugin function just
                // to inspect a collection. Sets expose only their cardinality.
                if (lua_getmetatable(L, -1)) {
                    lua_pushliteral(L, "__dnx_legacy_set"); lua_rawget(L, -2);
                    const bool set = lua_toboolean(L, -1); lua_pop(L, 2);
                    if (set) { lua_pushliteral(L, "__d"); lua_rawget(L, -2); }
                }
                if (lua_istable(L, -1)) {
                    const int table = lua_absindex(L, -1);
                    lua_pushnil(L);
                    while (lua_next(L, table)) {
                        lua_pop(L, 1);
                        if (++*length > 100000) { length = 0; break; }
                    }
                }
            }
        }
        lua_settop(L, top); return length;
    };
}

static int l_formatTpl(lua_State* L) {
    auto* m = mgrOf(L);
    std::string text = argStr(L, 1);
    if (!m) { lua_pushlstring(L, text.data(), text.size()); return 1; }
    std::map<std::string, std::string> vars;
    vars["uid"] = argStr(L, 2); vars["gid"] = argStr(L, 3); vars["nick"] = argStr(L, 4);
    vars["fromQQ"] = vars["uid"]; vars["fromGroup"] = vars["gid"];
    safe_template::Delay delay;
    std::string out = m->formatTemplate(text, vars, 0, dnx_messageReader(L, 5), dnx_messageLength(L, 5), &delay);
    lua_pushlstring(L, out.data(), out.size());
    lua_pushinteger(L,delay.milliseconds);
    return 2;
}

// ── __dnx_conf(scope, key [, val])：通用配置读写（GameTable/团表 等 proxy 后端）──
static int l_confRaw(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string scope = argStr(L, 1), key = argStr(L, 2);
    if (scope.empty() || key.empty()) return 0;
    if (lua_gettop(L) >= 3) {   // 写（nil=删）
        std::string val = lua_isnoneornil(L, 3) ? std::string() : confValueString(L, 3);
        m->confSet(scope, key, val);
        return 0;
    }
    std::string v = m->confGet(scope, key);
    if (v.empty() && !m->confHas(scope, key)) return 0;
    pushConfValue(L, v, scope, key);
    return 1;
}
static int l_mkDirs(lua_State* L) {
    std::error_code ec; fs::create_directories(argStr(L, 1), ec);
    lua_pushboolean(L, !ec); return 1;
}
static int l_sleepTime(lua_State* L) {
    const double milliseconds = luaL_optnumber(L, 1, 0.0);
    if (!(milliseconds > 0.0)) return 0;
    // dispatch 会把含 sleepTime 的旧式 msg_order 函数放进协程。其它调用栈
    // 不可 yield 时维持旧有“立即返回”行为，绝不阻塞适配器事件线程。
    if (!dnx_allowSleep || !lua_isyieldable(L)) return 0;
    lua_pushnumber(L, milliseconds / 1000.0);
    return lua_yield(L, 1);
}
static int l_drawDeck(lua_State* L) {
    auto* m = mgrOf(L);
    // 原版 drawDeck(fromGID, fromUID, deckName) 三参（DiceLua.cpp 1027）；牌名是最后一个参数。
    // Also accept drawDeck(name). Three-argument calls preserve their chat scope.
    int n = lua_gettop(L);
    std::string name = (n >= 3) ? argStr(L, 3) : argStr(L, 1);
    std::string group = n >= 3 ? argStr(L, 1) : std::string();
    std::string user = n >= 3 ? argStr(L, 2) : std::string();
    if (group == "0") group.clear();
    if (user == "0") user.clear();
    std::string r;
    try { if (m) r = m->drawDeck(name, group, user); }
    catch (const std::exception& error) {
        DICE_LOG_ERROR("Lua 抽牌宿主调用失败：{}", error.what());
        lua_pushnil(L); lua_pushliteral(L, "drawDeck host callback failed"); return 2;
    } catch (...) {
        DICE_LOG_ERROR("Lua 抽牌宿主调用失败：未知异常。");
        lua_pushnil(L); lua_pushliteral(L, "drawDeck host callback failed"); return 2;
    }
    lua_pushlstring(L, r.data(), r.size()); return 1;
}

// 当前加载中的 mod 名（供审计）。
static std::string luaAuditMod(LuaPluginManager* m) {
    return m ? dnx_u8str(std::filesystem::path(m->loadingModDir_).filename()) : std::string("?");
}
// __dnx_audit(action, detail)：bootstrap 包装 io/os/package 后回调审计。
static int l_audit(lua_State* L) {
    try {
        luaAudit(luaAuditMod(mgrOf(L)), argStr(L, 1), argStr(L, 2));
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("[lua] audit callback failed: {}", e.what());
    } catch (...) {
        DICE_LOG_ERROR("[lua] audit callback failed with an unknown error");
    }
    return 0;
}
// sendMsg(text, groupId, userId)：插件主动发消息（经注入的 sender 路由适配器）。
static int l_sendMsg(lua_State* L) {
    OutboundScope newOutput(nullptr);
    auto* m = mgrOf(L);
    std::string text = dnx_normalizeLuaText(argStr(L, 1));
    std::string gid = dnx_normalizeLuaText(argStr(L, 2));
    std::string uid = dnx_normalizeLuaText(argStr(L, 3));
    if (m && m->hasSender() && !text.empty()) {
        luaAudit(luaAuditMod(m), "sendMsg", "gid=" + gid + " uid=" + uid + " len=" + std::to_string(text.size()));
        try { m->send(text, gid, uid); }
        catch (const std::exception& e) { DICE_LOG_ERROR("[lua] sendMsg host callback failed: {}", e.what()); }
        catch (...) { DICE_LOG_ERROR("[lua] sendMsg host callback failed with unknown error"); }
    }
    return 0;
}
// eventMsg(text|table, gid, uid)：把 text 当作消息跑完整回复管线（原版 virtualCall）。
// 表形式 {fromMsg=,gid=,uid=}；gid=="0"/空 视为私聊。无返回值。
static int l_eventMsg(lua_State* L) {
    auto* m = mgrOf(L);
    if (!m || !m->eventMsg_) return 0;
    std::string text, gid, uid;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "fromMsg"); text = argStr(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "gid");     gid  = argStr(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "uid");     uid  = argStr(L, -1); lua_pop(L, 1);
    } else {
        text = argStr(L, 1); gid = argStr(L, 2); uid = argStr(L, 3);
    }
    text = dnx_normalizeLuaText(text);
    gid = dnx_normalizeLuaText(gid);
    uid = dnx_normalizeLuaText(uid);
    if (gid == "0") gid.clear();   // 原版 0=非群（私聊/系统）
    if (text.empty()) return 0;
    try { m->eventMsg_(text, gid, uid); }
    catch (const std::exception& e) { DICE_LOG_ERROR("[lua] eventMsg host callback failed: {}", e.what()); }
    catch (...) { DICE_LOG_ERROR("[lua] eventMsg host callback failed with unknown error"); }
    return 0;
}

// ── json 模块（require("json") + 全局 json）：encode/decode，backed nlohmann ──
static int l_jsonEncode(lua_State* L) {
    try { std::string s = luaToJson(L, 1).dump(-1, ' ', false, json::error_handler_t::replace); lua_pushlstring(L, s.data(), s.size()); }
    catch (...) { lua_pushnil(L); }
    return 1;
}
static int l_jsonDecode(lua_State* L) {
    std::string s = argStr(L, 1);
    try { jsonToLua(L, json::parse(s)); } catch (...) { lua_pushnil(L); }
    return 1;
}

// ── yaml 模块（require("yaml") + 全局 yaml）：parse/dump，backed yaml-cpp ──
//    复刻原版 lua_useful_extensions 等用到的 yaml.parse(text)→table。
static void yamlToLua(lua_State* L, const YAML::Node& n) {
    if (!n || n.IsNull()) { lua_pushnil(L); return; }
    if (n.IsSequence()) {
        lua_newtable(L); int i = 1;
        for (auto&& e : n) { yamlToLua(L, e); lua_rawseti(L, -2, i++); }
        return;
    }
    if (n.IsMap()) {
        lua_newtable(L);
        for (auto it = n.begin(); it != n.end(); ++it) {
            std::string k = it->first.as<std::string>("");
            yamlToLua(L, it->second);
            lua_setfield(L, -2, k.c_str());
        }
        return;
    }
    // 标量：依次尝试 bool / 整数 / 浮点 / 字符串。
    std::string s = n.as<std::string>("");
    if (s == "true" || s == "True")  { lua_pushboolean(L, 1); return; }
    if (s == "false" || s == "False"){ lua_pushboolean(L, 0); return; }
    if (s == "~" || s == "null")     { lua_pushnil(L); return; }
    try { size_t pos; long long iv = std::stoll(s, &pos); if (pos == s.size()) { lua_pushinteger(L, (lua_Integer)iv); return; } } catch (...) {}
    try { size_t pos; double dv = std::stod(s, &pos); if (pos == s.size()) { lua_pushnumber(L, dv); return; } } catch (...) {}
    lua_pushlstring(L, s.data(), s.size());
}
static YAML::Node luaToYaml(lua_State* L, int idx, int depth = 0) {
    if (depth > 32) return YAML::Node(YAML::NodeType::Null);
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL: return YAML::Node(YAML::NodeType::Null);
        case LUA_TBOOLEAN: return YAML::Node(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) return YAML::Node((long long)lua_tointeger(L, idx));
            return YAML::Node(lua_tonumber(L, idx));
        case LUA_TTABLE: {
            lua_len(L, idx); lua_Integer len = lua_tointeger(L, -1); lua_pop(L, 1);
            bool isArr = len > 0;
            if (isArr) { lua_pushnil(L); while (lua_next(L, idx)) { if (lua_type(L, -2) != LUA_TNUMBER) isArr = false; lua_pop(L, 1); if (!isArr) break; } if (!isArr) {} }
            if (isArr) {
                YAML::Node arr(YAML::NodeType::Sequence);
                for (lua_Integer i = 1; i <= len; ++i) { lua_rawgeti(L, idx, i); arr.push_back(luaToYaml(L, -1, depth + 1)); lua_pop(L, 1); }
                return arr;
            }
            YAML::Node obj(YAML::NodeType::Map);
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                std::string k;
                if (lua_type(L, -2) == LUA_TSTRING) k = lua_tostring(L, -2);
                else { lua_pushvalue(L, -2); k = lua_tostring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1); }
                if (!k.empty()) obj[k] = luaToYaml(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return obj;
        }
        default: { const char* s = lua_tostring(L, idx); return YAML::Node(std::string(s ? s : "")); }
    }
}
static int l_yamlParse(lua_State* L) {
    std::string s = argStr(L, 1);
    try { yamlToLua(L, YAML::Load(s)); } catch (...) { lua_pushnil(L); }
    return 1;
}
static int l_yamlDump(lua_State* L) {
    try { YAML::Emitter e; e << luaToYaml(L, 1); lua_pushstring(L, e.c_str()); }
    catch (...) { lua_pushnil(L); }
    return 1;
}

// ── http 库（复刻原版 http.get/post/urlEncode/urlDecode）──────────
// 返回 (bool 成功, string body)。走注入的受控 fetch（外置API开关 + SSRF）。
static int l_httpGet(lua_State* L) {
    auto* m = mgrOf(L); std::string url = argStr(L, 1);
    int status = 0; std::string body;
    if (m && m->httpFetch_) { luaAudit(luaAuditMod(m), "httpGet", url); body = m->httpFetch_("GET", url, "", "", status); }
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, body.data(), body.size());
    return 2;
}
static int l_httpPost(lua_State* L) {
    auto* m = mgrOf(L); std::string url = argStr(L, 1);
    std::string content = lua_istable(L, 2) ? luaToJson(L, 2).dump() : argStr(L, 2);   // 表→json
    std::string headers = "Content-Type: application/json";
    if (lua_istable(L, 3)) {   // {key=value} → "key: value\n..."
        headers.clear();
        lua_pushnil(L);
        while (lua_next(L, 3)) {
            lua_pushvalue(L, -2);
            std::string k = argStr(L, -1); lua_pop(L, 1);
            std::string v = argStr(L, -1);
            if (!k.empty()) { if (!headers.empty()) headers += "\n"; headers += k + ": " + v; }
            lua_pop(L, 1);
        }
    } else if (lua_isstring(L, 3)) headers = argStr(L, 3);
    int status = 0; std::string body;
    if (m && m->httpFetch_) { luaAudit(luaAuditMod(m), "httpPost", url); body = m->httpFetch_("POST", url, headers, content, status); }
    lua_pushboolean(L, status >= 200 && status < 300);
    lua_pushlstring(L, body.data(), body.size());
    return 2;
}
static int l_urlEncode(lua_State* L) {
    std::string s = argStr(L, 1), o;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { snprintf(buf, sizeof buf, "%%%02X", c); o += buf; }
    }
    lua_pushlstring(L, o.data(), o.size()); return 1;
}
static int l_urlDecode(lua_State* L) {
    std::string s = argStr(L, 1), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    lua_pushlstring(L, o.data(), o.size()); return 1;
}

// Push the Lua caller's _ENV. Legacy loadLua executes a sibling inside the same
// fresh LuaState as its entry plugin, so global assignments made by that sibling
// must belong to the current per-invocation environment rather than the shared
// host runtime. Returns false without changing the stack when no Lua _ENV exists.
static bool dnx_pushCallerEnvironment(lua_State* L) {
    lua_Debug frame{};
    if (!lua_getstack(L, 1, &frame) || !lua_getinfo(L, "f", &frame)) return false;
    const int caller = lua_absindex(L, -1);
    for (int index = 1;; ++index) {
        const char* name = lua_getupvalue(L, caller, index);
        if (!name) break;
        if (std::string_view(name) == "_ENV") {
            lua_remove(L, caller);  // leave only the environment value
            return true;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // caller function
    return false;
}

static int l_loadLua(lua_State* L) {
    auto* m = mgrOf(L); std::string name = argStr(L, 1);
    if (!m || name.empty()) { lua_pushnil(L); return 1; }
    const auto p = m->resolveScript(name, m->loadingModDir_);
    if (p.empty()) {
        DICE_LOG_ERROR("loadLua 无法读取脚本「{}」：未找到已启用的脚本或路径不安全。", name);
        lua_pushnil(L); return 1;
    }
    const int stackBefore = lua_gettop(L);
    if (dnx_loadfile(L, p) != LUA_OK) {
        DICE_LOG_ERROR("[lua] loadLua '{}' error: {}", name, argStr(L, -1));
        lua_settop(L, stackBefore); lua_pushnil(L); return 1;
    }
    const int chunk = lua_absindex(L, -1);
    if (dnx_pushCallerEnvironment(L)) lua_setupvalue(L, chunk, 1);
    if (dnx_pcall(L, 0, LUA_MULTRET) != LUA_OK) {
        DICE_LOG_ERROR("[lua] loadLua '{}' error: {}", name, argStr(L, -1));
        lua_settop(L, stackBefore); lua_pushnil(L); return 1;
    }
    if (lua_gettop(L) == stackBefore) lua_pushnil(L);
    return 1;   // 与旧实现一致：只把脚本最后一个返回值交给调用者
}

// ─── Lua 安全：审计日志 + 静态风险预检 ─────────────────────────
static std::string luaAuditTs() {
    std::time_t t = std::time(nullptr); std::tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[24]; std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &g); return b;
}
// 插件行为审计：data/audit/lua_audit.jsonl（检测而非预防）。
static void luaAudit(const std::string& mod, const std::string& action, const std::string& detail) {
    std::error_code ec;
    std::filesystem::create_directories("data/audit", ec);
    std::ofstream f("data/audit/lua_audit.jsonl", std::ios::app | std::ios::binary);
    if (!f) return;
    // Audit is a diagnostic boundary and must never be able to terminate the
    // host. Legacy plugins commonly pass mixed UTF-8/CP936 filenames here.
    nlohmann::json j{
        {"ts", luaAuditTs()},
        {"mod", dnx_normalizeLuaText(mod)},
        {"action", dnx_normalizeLuaText(action)},
        {"detail", dnx_normalizeLuaText(detail)},
    };
    f << j.dump() << '\n';
}

// 单文件 .lua 源码风险扫描（同 scanLuaRisks 的模式）。
static std::vector<std::string> scanLuaRisksText(const std::string& s) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& tag) {
        if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
    };
    const auto has = [&s](const std::string& p) { return s.find(p) != std::string::npos; };
    if (has("os.execute")) add("shell:os.execute");
    if (has("io.popen"))  add("shell:io.popen");
    if (has("package.loadlib")) add("native:package.loadlib");
    if (has("io.open"))   add("file:io.open");
    if (has("os.remove")) add("file:os.remove");
    if (has("os.rename")) add("file:os.rename");
    if (has("debug."))    add("debug");
    if (has("loadstring") || s.find("load(") != std::string::npos) add("dynamic:load");
    return out;
}
// 扫描目录下 .lua 源码命中的高危模式（仅告警，不硬拦——避免误杀正常 Dice! mod）。
static std::vector<std::string> scanLuaRisks(const std::filesystem::path& root) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& tag) {
        if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
    };
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return out;
    for (auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".lua") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const auto has = [&s](const std::string& p) { return s.find(p) != std::string::npos; };
        if (has("os.execute")) add("shell:os.execute");
        if (has("io.popen"))  add("shell:io.popen");
        if (has("package.loadlib")) add("native:package.loadlib");
        if (has("io.open"))   add("file:io.open");
        if (has("os.remove")) add("file:os.remove");
        if (has("os.rename")) add("file:os.rename");
        if (has("debug."))    add("debug");
        if (has("loadstring") || s.find("load(") != std::string::npos) add("dynamic:load");
    }
    return out;
}
// ─── LuaPluginManager ────────────────────────────────────────
LuaPluginManager::~LuaPluginManager() {
    { std::lock_guard<std::recursive_mutex> lock(callbackState_->mutex); callbackState_->owner = nullptr; }
    freeRuntime();
    std::lock_guard<std::mutex> lk(confMutex_);
    if (confDb_) { sqlite3_close(confDb_); confDb_ = nullptr; }
}

void LuaPluginManager::registerGlobals() {
    static const luaL_Reg fns[] = {
        {"log", l_log}, {"ranint", l_ranint},
        {"getUserConf", l_getUserConf}, {"setUserConf", l_setUserConf},
        {"getGroupConf", l_getGroupConf}, {"setGroupConf", l_setGroupConf},
        {"getUserToday", l_getUserToday}, {"setUserToday", l_setUserToday},
        {"getDiceDir", l_getDiceDir}, {"getDiceQQ", l_getDiceQQ}, {"getSelfData", l_getSelfData}, {"mkDirs", l_mkDirs},
        {"sleepTime", l_sleepTime}, {"drawDeck", l_drawDeck}, {"loadLua", l_loadLua}, {"sendMsg", l_sendMsg},
        {"eventMsg", l_eventMsg},
        {"getPlayerCard", l_getPlayerCard}, {"getPlayerCardAttr", l_getPlayerCardAttr},
        {"setPlayerCardAttr", l_setPlayerCardAttr},
        {"getFavor", l_getFavor}, {"setFavor", l_setFavor},
        {"addFavor", l_addFavor}, {"growFavor", l_growFavor},
        {"lockPlayerCard", l_lockPlayerCard}, {"unlockPlayerCard", l_unlockPlayerCard},
        {"isPlayerCardLocked", l_isPlayerCardLocked},
        {"askExtra", l_askExtra},                                  // 平台扩展查询（原版 DD::getExtra）
        {"__dnx_sd_load", l_sdLoad}, {"__dnx_sd_save", l_sdSave},  // SelfData 后端（bootstrap 用）
        {"__dnx_roll", l_rollExpr},                                // pc:rollDice 引擎桥
        {"__dnx_fmt", l_formatTpl},                                // msg:format 模板桥
        {"__dnx_conf", l_confRaw},                                 // 通用配置读写（GameTable 等）
        {"__dnx_audit", l_audit},                                  // Lua 安全审计回调
        {"__dnx_resolve_plugin_file", l_resolvePluginFile},         // 旧插件缓存路径重映射
        {"__dnx_resolve_image_resource", l_resolveImageResource},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* f = fns; f->name; ++f) lua_register(state_, f->name, f->func);
    lua_register(state_, "print", l_log);
    for (const auto& [name, level] : std::vector<std::pair<const char*, LogLevel>>{
        {"logTrace", LogLevel::Trace}, {"logDebug", LogLevel::Debug}, {"logInfo", LogLevel::Info},
        {"logSuccess", LogLevel::Success}, {"logWarn", LogLevel::Warn}, {"logError", LogLevel::Error}}) {
        lua_pushinteger(state_, static_cast<int>(level));
        lua_pushcclosure(state_, l_log, 1); lua_setglobal(state_, name);
    }
}

bool LuaPluginManager::s_cpathStrict = false;   // cpath 收窄开关（兼容优先默认关）

bool LuaPluginManager::init() {
    std::lock_guard<ScriptMutex> lk(mutex_);
    if (state_) return true;
    state_ = lua_newstate([](void* opaque, void* pointer, size_t oldSize, size_t newSize) -> void* {
        auto& manager = *static_cast<LuaPluginManager*>(opaque);
        if (!pointer) oldSize = 0; // Lua passes a type tag on new allocations
        if (!newSize) {
            manager.allocatedBytes_ -= oldSize;
            std::free(pointer);
            return nullptr;
        }
        // Only fail allocation inside protected lua_resume. Returning nullptr
        // from an unprotected host stack setup would invoke Lua's abort/panic.
        if (dnx_luaProtected && newSize > oldSize &&
            newSize - oldSize > manager.memoryLimit_ - std::min(manager.memoryLimit_, manager.allocatedBytes_))
            return nullptr;
        void* next = std::realloc(pointer, newSize);
        if (next) manager.allocatedBytes_ = manager.allocatedBytes_ - oldSize + newSize;
        return next;
    }, this);
    if (!state_) { DICE_LOG_ERROR("[lua] luaL_newstate failed"); return false; }
    ++runtimeGeneration_;
    luaL_openlibs(state_);
    lua_getglobal(state_, "setmetatable");
    lua_pushcclosure(state_, dnx_setmetatable, 1); lua_setglobal(state_, "setmetatable");
    lua_pushcfunction(state_, dnx_checkExecution); lua_setglobal(state_, "__dnx_check_execution");
    lua_getglobal(state_, "os"); lua_pushcfunction(state_, dnx_execute); lua_setfield(state_, -2, "execute"); lua_pop(state_, 1);
    lua_getglobal(state_, "os"); lua_pushcfunction(state_, dnx_forbidExit); lua_setfield(state_, -2, "exit"); lua_pop(state_, 1);
    lua_getglobal(state_, "io"); lua_pushcfunction(state_, dnx_popen); lua_setfield(state_, -2, "popen"); lua_pop(state_, 1);
    lua_sethook(state_, dnx_executionHook, LUA_MASKCOUNT, 10000);
    lua_getglobal(state_, "debug");
    lua_pushcfunction(state_, dnx_sethook); lua_setfield(state_, -2, "sethook");
    lua_pop(state_, 1);
    // Check outside the original protected call too. In non-yieldable __gc or
    // metamethod frames, a repeated pcall must not swallow watchdog errors.
    dnx_dostring(state_, R"lua(do
        local check = __dnx_check_execution
        local call, xcall, resume = pcall, xpcall, coroutine.resume
        pcall = function(...) check(); return call(...) end
        xpcall = function(...) check(); return xcall(...) end
        coroutine.resume = function(...) check(); return resume(...) end
    end)lua");
    lua_pushlightuserdata(state_, this);
    lua_setfield(state_, LUA_REGISTRYINDEX, "DiceMgr");
    registerGlobals();
    // json 模块：全局 json + package.loaded["json"]（供 require("json")）。
    lua_newtable(state_);
    lua_pushcfunction(state_, l_jsonEncode); lua_setfield(state_, -2, "encode");
    lua_pushcfunction(state_, l_jsonDecode); lua_setfield(state_, -2, "decode");
    lua_pushvalue(state_, -1); lua_setglobal(state_, "json");
    lua_getglobal(state_, "package"); lua_getfield(state_, -1, "loaded");
    lua_pushvalue(state_, -3); lua_setfield(state_, -2, "json");
    lua_pop(state_, 3);   // loaded, package, json table
    // yaml 模块：全局 yaml + package.loaded["yaml"]（供 require("yaml")）。parse/dump（+别名 decode/encode）。
    lua_newtable(state_);
    lua_pushcfunction(state_, l_yamlParse); lua_setfield(state_, -2, "parse");
    lua_pushcfunction(state_, l_yamlParse); lua_setfield(state_, -2, "decode");
    lua_pushcfunction(state_, l_yamlParse); lua_setfield(state_, -2, "load");
    lua_pushcfunction(state_, l_yamlDump);  lua_setfield(state_, -2, "dump");
    lua_pushcfunction(state_, l_yamlDump);  lua_setfield(state_, -2, "encode");
    lua_pushvalue(state_, -1); lua_setglobal(state_, "yaml");
    lua_getglobal(state_, "package"); lua_getfield(state_, -1, "loaded");
    lua_pushvalue(state_, -3); lua_setfield(state_, -2, "yaml");
    lua_pop(state_, 3);   // loaded, package, yaml table
    // http 库：get/post/urlEncode/urlDecode（走受控 fetch）。
    lua_newtable(state_);
    lua_pushcfunction(state_, l_httpGet);    lua_setfield(state_, -2, "get");
    lua_pushcfunction(state_, l_httpPost);   lua_setfield(state_, -2, "post");
    lua_pushcfunction(state_, l_urlEncode);  lua_setfield(state_, -2, "urlEncode");
    lua_pushcfunction(state_, l_urlDecode);  lua_setfield(state_, -2, "urlDecode");
    lua_setglobal(state_, "http");
    openConfStore();
    // ── bootstrap：用 Lua 实现原版的对象层（Set/SelfData/Actor/GameTable/User/Chat proxy
    // + msg Context 化）。C 侧仅曝光薄后端（__dnx_* 函数）。与 DiceLua.cpp 语义对齐。
    static const char* kBootstrap = R"lua(
do
  -- ===== Set 库（原版 luaopen_Set：new/add/in/remove/totable/#/tostring）=====
  local SetMethods = {}
  local function set_flush(s)
    local save = rawget(s, '__save')
    if save then save(SetMethods.totable(s)) end
  end
  SetMethods.add     = function(s, v)
    local d = rawget(s,'__d'); local had = d[v] ~= nil; d[v] = true
    if not had then set_flush(s) end
    return not had
  end
  SetMethods.remove  = function(s, v)
    local d = rawget(s,'__d'); local had = d[v] ~= nil; d[v] = nil
    if had then set_flush(s) end
    return had
  end
  SetMethods['in']   = function(s, v) return rawget(s,'__d')[v] ~= nil end
  SetMethods.totable = function(s) local t = {}; for k in pairs(rawget(s,'__d')) do t[#t+1] = k end; return t end
  local SetM = {
    __dnx_legacy_set = true,
    __index = function(s, k)
      if SetMethods[k] then return SetMethods[k] end
      if rawget(s,'__d')[k] ~= nil then return true end
      return nil
    end,
    __len = function(s) local n = 0; for _ in pairs(rawget(s,'__d')) do n = n + 1 end; return n end,
    __tostring = function(s)
      local t = {}; for k in pairs(rawget(s,'__d')) do t[#t+1] = tostring(k) end
      return '{' .. table.concat(t, ',') .. '}'
    end,
  }
  Set = { new = function(values, save)
    local d = {}
    if type(values) == 'table' then
      for k, v in pairs(values) do
        if type(k) == 'number' then d[v] = true elseif v then d[k] = true end
      end
    end
    local o = {}; rawset(o, '__d', d); rawset(o, '__save', save)
    return setmetatable(o, SetM)
  end }

  -- ===== SelfData（原版 getSelfData：读写自动落盘 data/self_data/<name>.json）=====
  local sd_cache = {}
  local function sd_flush(o) __dnx_sd_save(rawget(o,'__name'), json.encode(rawget(o,'__t'))) end
  local SDMethods = {
    get = function(o, key, def)
      if key == nil then return rawget(o,'__t') end
      local v = rawget(o,'__t')[key]
      if v == nil then return def end
      return v
    end,
    set = function(o, key, val)
      if type(key) == 'table' then rawset(o, '__t', key)
      else rawget(o,'__t')[key] = val end
      sd_flush(o)
    end,
  }
  local SDM = {
    __index = function(o, k)
      if SDMethods[k] then return SDMethods[k] end
      return rawget(o,'__t')[k]
    end,
    __newindex = function(o, k, v) rawget(o,'__t')[k] = v; sd_flush(o) end,
    __totable = function(o) return rawget(o,'__t') end,
  }
  getSelfData = function(name)
    if not name or name == '' then return nil end
    if sd_cache[name] then return sd_cache[name] end
    local ok, t = pcall(json.decode, __dnx_sd_load(name))
    if not ok or type(t) ~= 'table' then t = {} end
    local o = {}; rawset(o,'__t',t); rawset(o,'__name',name)
    setmetatable(o, SDM); sd_cache[name] = o
    return o
  end
  __dnx_sd_reset = function() sd_cache = {} end

  -- ===== Actor（原版 metatable Actor：get/set/rollDice/locked/lock/unlock + 属性读写）=====
  local ActorMethods = {
    get = function(a, k) return getPlayerCardAttr(rawget(a,'__u'), rawget(a,'__g'), k, nil, rawget(a,'__n')) end,
    set = function(a, k, v)
      local u, g = rawget(a,'__u'), rawget(a,'__g')
      if type(k) == 'table' then
        local n = 0
        for kk, vv in pairs(k) do setPlayerCardAttr(u, g, kk, vv, rawget(a,'__n')); n = n + 1 end
        return n
      end
      setPlayerCardAttr(u, g, k, v, rawget(a,'__n')); return 1
    end,
    rollDice = function(a, exp)
      local u, g = rawget(a,'__u'), rawget(a,'__g')
      if not exp or exp == '' then exp = getPlayerCardAttr(u, g, '__DefaultDiceExp', nil, rawget(a,'__n')) or '1D' end
      local face = tonumber(getPlayerCardAttr(u, g, '__DefaultDice', nil, rawget(a,'__n'))) or 100
      return __dnx_roll(exp, face)
    end,
    lock   = function(a, k) return lockPlayerCard(rawget(a,'__u'), rawget(a,'__g'), k, rawget(a,'__n')) end,
    unlock = function(a, k) return unlockPlayerCard(rawget(a,'__u'), rawget(a,'__g'), k, rawget(a,'__n')) end,
    locked = function(a, k) return isPlayerCardLocked(rawget(a,'__u'), rawget(a,'__g'), k, rawget(a,'__n')) end,
  }
  local ActorM = {
    __index = function(a, k)
      if ActorMethods[k] then return ActorMethods[k] end
      return getPlayerCardAttr(rawget(a,'__u'), rawget(a,'__g'), k, nil, rawget(a,'__n'))
    end,
    __newindex = function(a, k, v) setPlayerCardAttr(rawget(a,'__u'), rawget(a,'__g'), k, v, rawget(a,'__n')) end,
  }
  Actor = function(uid, gid, byName)
    local o = {}; rawset(o,'__u', tostring(uid)); rawset(o,'__g', (gid == nil or gid == '') and '0' or tostring(gid)); rawset(o,'__n', byName == true)
    return setmetatable(o, ActorM)
  end

  -- ===== GameTable（原版：set/message + 团表变量读写；多群共享团时解析 __session）=====
  local function game_scope(gid)
    local group_scope = 'game:'..tostring(gid)
    local session = __dnx_conf(group_scope, '__session')
    if session ~= nil and session ~= '' then return 'game:session:'..tostring(session) end
    return group_scope
  end
  local GameMethods = {
    set = function(gt, k, v)
      if type(k) == 'table' then
        for kk, vv in pairs(k) do
          if type(kk) == 'number' then __dnx_conf(game_scope(rawget(gt,'__g')), tostring(vv), nil)
          else __dnx_conf(game_scope(rawget(gt,'__g')), tostring(kk), vv) end
        end
      else __dnx_conf(game_scope(rawget(gt,'__g')), tostring(k), v) end
    end,
    message = function(gt, text) if text and text ~= '' then sendMsg(text, rawget(gt,'__g'), '') end end,
  }
  local GameM = {
    __index = function(gt, k)
      if GameMethods[k] then return GameMethods[k] end
      local scope = game_scope(rawget(gt,'__g'))
      -- 原版 GameTable 暴露 game.pls/game.gms 为 Set；Next 持久层以
      -- __pls/__gms 的 JSON 数组保存，必须在这里还原对象语义。
      if k == 'pls' or k == 'gms' then
        local storage = '__'..k
        local raw = __dnx_conf(scope, storage)
        local ok, values = pcall(json.decode, raw or '[]')
        if not ok or type(values) ~= 'table' then values = {} end
        return Set.new(values, function(nextValues)
          __dnx_conf(scope, storage, json.encode(nextValues))
        end)
      end
      return __dnx_conf(scope, tostring(k))
    end,
    __newindex = function(gt, k, v) __dnx_conf(game_scope(rawget(gt,'__g')), tostring(k), v) end,
  }
  GameTable = function(gid)
    if gid == nil or gid == '' then return nil end
    -- 对齐原版语义——开团（.game new 写 __name）才存在，无团返回 nil。
    local nm = __dnx_conf(game_scope(gid), '__name')
    if nm == nil or nm == '' then return nil end
    local o = {}; rawset(o,'__g', tostring(gid))
    return setmetatable(o, GameM)
  end

  -- ===== User / Chat proxy（msg.user / msg.grp|group：读写用户/群 conf）=====
  local UserM = {
    __index = function(u, k)
      if k == 'trust' then return tonumber(rawget(u,'__trust')) or 0 end
      if k == 'nick' or k == 'name' or k == 'nn' then return rawget(u,'__nick') end
      return getUserConf(rawget(u,'__id'), k)
    end,
    __newindex = function(u, k, v) setUserConf(rawget(u,'__id'), k, v) end,
    __tostring = function(u) return rawget(u,'__id') end,
  }
  local GrpM = {
    __index = function(g, k) return getGroupConf(rawget(g,'__id'), k) end,
    __newindex = function(g, k, v) setGroupConf(rawget(g,'__id'), k, v) end,
    __tostring = function(g) return rawget(g,'__id') end,
  }

  -- ===== msg Context 化（原版 msg userdata：方法 echo/format/get/inc + 动态字段）=====
  -- Only message-owned fields and the existing public proxies are traversed.
  -- No _G, metatable internals, host settings or implicit SelfData lookup.
  __dnx_context_get = function(t, key)
    if type(t) ~= 'table' or type(key) ~= 'string' or #key == 0 or #key > 512 then return nil end
    local parts = {}
    for part in key:gmatch('[^.]+') do
      if part:sub(1,2) == '__' or part:find('[%c]') then return nil end
      parts[#parts+1] = part
    end
    if #parts == 0 or #parts > 16 or table.concat(parts,'.') ~= key then return nil end
    local exact = rawget(t,key)
    if exact ~= nil and type(exact) ~= 'function' then return exact end
    local value = t
    for _, part in ipairs(parts) do
      if type(value) ~= 'table' then return nil end
      value = value[part]
      if type(value) == 'function' then return nil end
    end
    return value
  end
  __dnx_wrap_msg = function(m, trust)
    local uidS = tostring(rawget(m,'uid') or '')
    local gidS = tostring(rawget(m,'gid') or '')
    -- 原版 uid/gid 是整数
    if tonumber(uidS) then rawset(m, 'uid', tonumber(uidS)) end
    if gidS == '' then rawset(m, 'gid', nil)
    elseif tonumber(gidS) then rawset(m, 'gid', tonumber(gidS)) end
    local MsgMethods = {
      echo = function(t, text, noFmt)
        if text == nil or text == '' then return end
        text = tostring(text)
        if not noFmt then
          local delay
          text,delay = __dnx_fmt(text, uidS, gidS, rawget(t,'nick') or '', t)
          if delay and delay > 0 then sleepTime(delay) end
        end
        sendMsg(text, gidS, uidS)
      end,
      format = function(t, text)
        local formatted,delay = __dnx_fmt(tostring(text or ''), uidS, gidS, rawget(t,'nick') or '', t)
        if delay and delay > 0 then sleepTime(delay) end
        return formatted
      end,
      get = function(t, key, def)
        if key == nil then return t end
        local v = __dnx_context_get(t, key)
        if v == nil then return def end
        return v
      end,
      inc = function(t, key, n)
        local v = tonumber(rawget(t, key)) or 0
        rawset(t, key, v + (tonumber(n) or 1))
        return rawget(t, key)
      end,
    }
    local lazy = {}   -- 惰性字段缓存
    local M = {
      __index = function(t, k)
        if MsgMethods[k] then return MsgMethods[k] end
        if lazy[k] ~= nil then return lazy[k] end
        local v
        if k == 'at' or k == '@' then v = (uidS ~= '') and ('[CQ:at,qq='..uidS..']') or ''
        elseif k == 'char' then v = Actor(uidS, gidS, false)
        elseif k == 'pc' then v = rawget(t,'nick')
        elseif k == 'user' then
          local o = {}; rawset(o,'__id',uidS); rawset(o,'__nick',rawget(t,'nick') or ''); rawset(o,'__trust',trust or 0)
          v = setmetatable(o, UserM)
        elseif k == 'grp' or k == 'group' then
          if gidS ~= '' then local o = {}; rawset(o,'__id',gidS); v = setmetatable(o, GrpM) end
        elseif k == 'game' then v = GameTable(gidS)
        elseif k == 'gender' then v = getUserConf(uidS, 'gender')   -- 原版 getUserItem(uid,'gender')
        elseif k == 'grpAuth' then v = trust or 0                    -- 原版 idx_gAuth（近似为本消息 trust 等级）
        end
        if v ~= nil then lazy[k] = v end
        return v
      end,
    }
    return setmetatable(m, M)
  end
end
)lua";
    if (dnx_dostring(state_, kBootstrap) != LUA_OK) {
        DICE_LOG_WARN("[lua] bootstrap error: {}", argStr(state_, -1));
    }
    // require aliases return the same public objects, not independent copies.
    dnx_dostring(state_, "package.loaded.Set=Set; package.loaded.Actor=Actor; package.loaded.GameTable=GameTable; package.loaded.SelfData=getSelfData");
    // ── 审计包装：io.open/io.popen/os.execute/package.loadlib 等记录到审计日志 ──
    static const char* kAuditWrap = R"lua(
if __dnx_audit then
  do
    local function detail(...)
      local values = {}
      for i = 1, select('#', ...) do values[i] = tostring(select(i, ...)) end
      return table.concat(values, ' ')
    end
    local function wrap(t, k, act)
      local orig = t[k]
      if type(orig) == 'function' then
        t[k] = function(...)
          __dnx_audit(act, detail(...))
          return orig(...)
        end
      end
    end
    do
      local orig = io.open
      io.open = function(path, ...)
        __dnx_audit('io.open', detail(path, ...))
        local image = __dnx_resolve_image_resource and __dnx_resolve_image_resource(path)
        if image ~= nil and image ~= path then
          __dnx_audit('io.open.image', detail(path, image))
          return orig(image, ...)
        end
        local file, err, code = orig(path, ...)
        if file ~= nil then return file, err, code end
        local mapped = __dnx_resolve_plugin_file and __dnx_resolve_plugin_file(path)
        if mapped ~= nil and mapped ~= path then
          __dnx_audit('io.open.remap', detail(path, mapped))
          return orig(mapped, ...)
        end
        return file, err, code
      end
    end
    wrap(io, 'popen', 'io.popen')
    wrap(os, 'execute', 'os.execute')
    wrap(os, 'remove', 'os.remove')
    wrap(os, 'rename', 'os.rename')
    wrap(package, 'loadlib', 'package.loadlib')
  end
end
)lua";
    if (dnx_dostring(state_, kAuditWrap) != LUA_OK) {
        DICE_LOG_WARN("[lua] audit wrap error: {}", argStr(state_, -1));
    }

    DICE_LOG_DEBUG("Lua 运行时已初始化，旧版接口与日志桥接已就绪。");
    return true;
}

void LuaPluginManager::freeRuntime() {
    std::lock_guard<ScriptMutex> lk(mutex_);
    ++runtimeGeneration_;                    // 让已经排队的续跑回调自动失效
    pendingCoroutines_.clear();
    std::atomic_store(&directoryRules_, std::make_shared<const std::vector<DirectoryRule>>());
    taskCalls_.clear();
    if (state_) { lua_close(state_); state_ = nullptr; allocatedBytes_ = 0; }
}

void LuaPluginManager::openConfStore() {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (confDb_) return;
    std::error_code ec; fs::create_directories("data", ec);
    if (sqlite3_open("data/lua_mod.db", &confDb_) != SQLITE_OK) {
        DICE_LOG_ERROR("[lua] open conf db failed: {}", confDb_ ? sqlite3_errmsg(confDb_) : "null");
        if (confDb_) { sqlite3_close(confDb_); confDb_ = nullptr; }
        return;
    }
    char* err = nullptr;
    sqlite3_exec(confDb_,
        "CREATE TABLE IF NOT EXISTS lua_conf(scope TEXT, k TEXT, v TEXT, PRIMARY KEY(scope,k)) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS lua_card(uid TEXT, scope TEXT, data TEXT, PRIMARY KEY(uid,scope)) WITHOUT ROWID;",
        nullptr, nullptr, &err);
    if (err) { DICE_LOG_ERROR("[lua] schema: {}", err); sqlite3_free(err); }
}

std::vector<std::pair<std::string, std::string>> LuaPluginManager::confAllUsers(const std::string& key) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    std::vector<std::pair<std::string, std::string>> out;
    if (!confDb_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(confDb_, "SELECT scope,v FROM lua_conf WHERE k=? AND scope LIKE 'u:%';", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* sc = sqlite3_column_text(st, 0);
            const unsigned char* v = sqlite3_column_text(st, 1);
            if (sc) { std::string scope = reinterpret_cast<const char*>(sc);
                out.emplace_back(scope.substr(2), v ? reinterpret_cast<const char*>(v) : ""); }
        }
    }
    sqlite3_finalize(st);
    return out;
}

// 枚举某作用域（如 "u:<uid>"）全部键值，玩家管理详情页用。
std::vector<std::pair<std::string, std::string>> LuaPluginManager::confAllOf(const std::string& scope) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    std::vector<std::pair<std::string, std::string>> out;
    if (!confDb_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(confDb_, "SELECT k,v FROM lua_conf WHERE scope=? ORDER BY k;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* k = sqlite3_column_text(st, 0);
            const unsigned char* v = sqlite3_column_text(st, 1);
            if (k) out.emplace_back(reinterpret_cast<const char*>(k),
                                    v ? reinterpret_cast<const char*>(v) : "");
        }
    }
    sqlite3_finalize(st);
    return out;
}

// 枚举某用户全部 Lua 卡片数据（scope→data JSON）。
std::vector<std::pair<std::string, std::string>> LuaPluginManager::cardAllOf(const std::string& uid) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    std::vector<std::pair<std::string, std::string>> out;
    if (!confDb_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(confDb_, "SELECT scope,data FROM lua_card WHERE uid=? ORDER BY scope;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* s = sqlite3_column_text(st, 0);
            const unsigned char* d = sqlite3_column_text(st, 1);
            if (s) out.emplace_back(reinterpret_cast<const char*>(s),
                                    d ? reinterpret_cast<const char*>(d) : "{}");
        }
    }
    sqlite3_finalize(st);
    return out;
}

// 删除某用户某作用域的 Lua 卡片数据。
void LuaPluginManager::cardDel(const std::string& uid, const std::string& scope) {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(confDb_, "DELETE FROM lua_card WHERE uid=? AND scope=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

bool LuaPluginManager::playerCardRead(const std::string& uid, const std::string& selector,
                                      bool byName, const std::string& key, nlohmann::json& out) const {
    return playerCardRead_ && playerCardRead_(uid, selector, byName, key, out);
}
bool LuaPluginManager::playerCardWrite(const std::string& uid, const std::string& selector,
                                       bool byName, const std::string& key, const nlohmann::json& value) const {
    return playerCardWrite_ && playerCardWrite_(uid, selector, byName, key, value);
}
bool LuaPluginManager::playerCardLock(const std::string& uid, const std::string& selector,
                                      bool byName, const std::string& key, bool on) const {
    if (playerCardLock_) return playerCardLock_(uid, selector, byName, key, on);
    return cardLock(uid, selector, key, on);
}
bool LuaPluginManager::playerCardLocked(const std::string& uid, const std::string& selector,
                                        bool byName, const std::string& key) const {
    return playerCardLocked_ && playerCardLocked_(uid, selector, byName, key);
}

std::string LuaPluginManager::cardLoad(const std::string& uid, const std::string& scope) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return "{}";
    sqlite3_stmt* st = nullptr; std::string out = "{}";
    if (sqlite3_prepare_v2(confDb_, "SELECT data FROM lua_card WHERE uid=? AND scope=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, scope.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) { const unsigned char* d = sqlite3_column_text(st, 0); if (d) out = reinterpret_cast<const char*>(d); }
    }
    sqlite3_finalize(st);
    return out;
}

void LuaPluginManager::cardSave(const std::string& uid, const std::string& scope, const std::string& jsonObj) {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(confDb_,
            "INSERT INTO lua_card(uid,scope,data) VALUES(?,?,?) ON CONFLICT(uid,scope) DO UPDATE SET data=excluded.data;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, jsonObj.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

nlohmann::json LuaPluginManager::confIndexedSnapshot(const std::string& indexScope,
    const std::string& indexKey, const std::string& scopePrefix, const std::vector<std::string>& keys) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    auto out = nlohmann::json::object();
    if (!confDb_ || keys.empty()) return out;
    std::string sql = R"sql(
        SELECT entry.key, c.k, c.v FROM lua_conf AS idx,
        json_each(CASE WHEN json_valid(idx.v) THEN
            CASE WHEN json_type(idx.v)='object' THEN idx.v ELSE '{}' END ELSE '{}' END) AS entry
        JOIN lua_conf AS c ON c.scope=(?3 || entry.value)
        WHERE idx.scope=?1 AND idx.k=?2 AND entry.type='text' AND c.k IN (
    )sql";
    for (std::size_t i = 0; i < keys.size(); ++i) sql += i ? ",?" : "?";
    sql += ")";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(confDb_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(confDb_));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    sqlite3_bind_text(raw, 1, indexScope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, indexKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, scopePrefix.c_str(), -1, SQLITE_TRANSIENT);
    for (std::size_t i = 0; i < keys.size(); ++i)
        sqlite3_bind_text(raw, static_cast<int>(i + 4), keys[i].c_str(), -1, SQLITE_TRANSIENT);
    int rc;
    while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
        auto text = [&](int column) {
            const auto* value = sqlite3_column_text(raw, column);
            return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
        };
        out[text(0)][text(1)] = text(2);
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(confDb_));
    return out;
}

std::string LuaPluginManager::confGet(const std::string& scope, const std::string& key) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return "";
    sqlite3_stmt* st = nullptr; std::string out;
    if (sqlite3_prepare_v2(confDb_, "SELECT v FROM lua_conf WHERE scope=? AND k=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* v = sqlite3_column_text(st, 0);
            if (v) out = reinterpret_cast<const char*>(v);
        }
    }
    sqlite3_finalize(st);
    return out;
}

// 是否存在该键（区分「存了空串」与「未存」，让默认值语义正确）。
bool LuaPluginManager::confHas(const std::string& scope, const std::string& key) const {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return false;
    sqlite3_stmt* st = nullptr; bool has = false;
    if (sqlite3_prepare_v2(confDb_, "SELECT 1 FROM lua_conf WHERE scope=? AND k=?;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        has = (sqlite3_step(st) == SQLITE_ROW);
    }
    sqlite3_finalize(st);
    return has;
}

void LuaPluginManager::confSet(const std::string& scope, const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return;
    sqlite3_stmt* st = nullptr;
    if (val.empty()) {   // 删除
        if (sqlite3_prepare_v2(confDb_, "DELETE FROM lua_conf WHERE scope=? AND k=?;", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
        }
    } else if (sqlite3_prepare_v2(confDb_,
            "INSERT INTO lua_conf(scope,k,v) VALUES(?,?,?) ON CONFLICT(scope,k) DO UPDATE SET v=excluded.v;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, val.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

bool LuaPluginManager::confSetBatch(
        const std::vector<std::tuple<std::string, std::string, std::string>>& values) {
    std::lock_guard<std::mutex> lk(confMutex_);
    if (!confDb_) return false;
    if (values.empty()) return true;

    char* error = nullptr;
    if (sqlite3_exec(confDb_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error) != SQLITE_OK) {
        DICE_LOG_ERROR("[lua] begin batch config transaction failed: {}", error ? error : sqlite3_errmsg(confDb_));
        if (error) sqlite3_free(error);
        return false;
    }

    sqlite3_stmt* upsert = nullptr;
    sqlite3_stmt* remove = nullptr;
    bool ok = sqlite3_prepare_v2(confDb_,
        "INSERT INTO lua_conf(scope,k,v) VALUES(?,?,?) ON CONFLICT(scope,k) DO UPDATE SET v=excluded.v;",
        -1, &upsert, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(confDb_, "DELETE FROM lua_conf WHERE scope=? AND k=?;",
        -1, &remove, nullptr) == SQLITE_OK;

    for (const auto& [scope, key, value] : values) {
        if (!ok) break;
        sqlite3_stmt* statement = value.empty() ? remove : upsert;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        sqlite3_bind_text(statement, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        if (!value.empty()) sqlite3_bind_text(statement, 3, value.c_str(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(statement) == SQLITE_DONE;
    }
    sqlite3_finalize(upsert);
    sqlite3_finalize(remove);

    if (ok) {
        error = nullptr;
        ok = sqlite3_exec(confDb_, "COMMIT;", nullptr, nullptr, &error) == SQLITE_OK;
        if (!ok) {
            DICE_LOG_ERROR("[lua] commit batch config transaction failed: {}", error ? error : sqlite3_errmsg(confDb_));
            if (error) sqlite3_free(error);
        }
    }
    if (!ok) {
        error = nullptr;
        sqlite3_exec(confDb_, "ROLLBACK;", nullptr, nullptr, &error);
        if (error) sqlite3_free(error);
    }
    return ok;
}

bool LuaPluginManager::eval(const std::string& code, std::string* err) {
    std::lock_guard<ScriptMutex> lk(mutex_);
    if (!state_ && !init()) { if (err) *err = "no state"; return false; }
    ProcessingScope logScope(ProcessingSource{"Lua 脚本", "临时执行", "<eval>", "求值"});
    const int base = lua_gettop(state_);
    if (dnx_dostring(state_, code.c_str()) != LUA_OK) {
        if (err) *err = argStr(state_, -1);
        lua_settop(state_, base);
        return false;
    }
    lua_settop(state_, base);
    return true;
}

int LuaPluginManager::loadDir(const std::string& dir) {
    std::lock_guard<ScriptMutex> lk(mutex_);
    if (!state_ && !init()) return 0;
    return loadDirLocked(dir);
}

// 容错：去掉对象/数组末尾的多余逗号（真实 mod 描述档常见，原版 Dice! 容忍）。
// 跳过字符串内的逗号（含转义），仅当 , 后下一个非空白为 } 或 ] 时删之。
static std::string stripJsonTrailingCommas(const std::string& s) {
    std::string out; out.reserve(s.size());
    bool inStr = false, esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            out.push_back(c);
            if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; out.push_back(c); continue; }
        if (c == ',') {
            size_t k = i + 1;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r' || s[k] == '\n')) ++k;
            if (k < s.size() && (s[k] == '}' || s[k] == ']')) continue;   // 丢弃尾逗号
        }
        out.push_back(c);
    }
    return out;
}

// 解析 mod 描述档（<name>.json 或 <name>/descriptor.json）。字段兼容 mod/title、ver/version。
// speechOut：原版 loadDesc 还认 json 内嵌 speech{}（文本或均匀随机文本数组）。
static void parseModDescriptor(const fs::path& descPath, LuaPluginManager::LuaMod& m,
                               std::map<std::string, std::vector<std::string>>* speechOut = nullptr) {
    try {
        std::ifstream f(descPath, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // Preserve declaration order for case-equivalent keys without using
        // ordered_json's linear-search object container on large query Mods.
        std::string section;
        std::vector<std::string> helpKeys, speechKeys;
        auto j = json::parse(stripJsonTrailingCommas(raw), [&](int depth, json::parse_event_t event, json& value) {
            if (event == json::parse_event_t::key) {
                if (depth == 1) section = value.get<std::string>();
                else if (depth == 2 && section == "helpdoc") helpKeys.push_back(value.get<std::string>());
                else if (depth == 2 && section == "speech") speechKeys.push_back(value.get<std::string>());
            }
            return true;
        }, true, true);
        m.title = j.value("title", j.value("mod", m.name));
        m.author = j.value("author", "");
        m.version = j.value("ver", j.value("version", ""));
        m.brief = j.value("brief", "");
        if (j.contains("permissions") && j["permissions"].is_array()) {
            for (auto& p : j["permissions"]) if (p.is_string()) m.permissions.push_back(p.get<std::string>());
            m.permissionDeclared = true;
        }
        if (j.contains("helpdoc") && j["helpdoc"].is_object())
            for (const auto& key : helpKeys)
                if (j["helpdoc"][key].is_string()) m.helpdoc[key] = j["helpdoc"][key].get<std::string>();
        if (speechOut && j.contains("speech") && j["speech"].is_object())
            for (const auto& key : speechKeys) {
                const auto& value = j["speech"][key];
                if (value.is_string()) (*speechOut)[legacy_mod::lower(key)] = {value.get<std::string>()};
                else if (value.is_array() && std::all_of(value.begin(), value.end(),
                    [](const auto& item) { return item.is_string(); }))
                    (*speechOut)[legacy_mod::lower(key)] = value.get<std::vector<std::string>>();
            }
    } catch (const std::exception& ex) {
        DICE_LOG_ERROR("[lua] mod descriptor '{}' parse error: {}", dnx_u8str(descPath.filename()), ex.what());
    }
}

int LuaPluginManager::loadDirLocked(const std::string& dir) {
    lastLoadErrors_ = 0;
    dir_ = dir;
    mods_.clear();
    suspendedMods_.clear();
    speech_.clear();
    replyRules_.clear();   // echo refs 随旧 state 关闭已失效
    std::atomic_store(&directoryRules_, std::make_shared<const std::vector<DirectoryRule>>());
    cycleEvents_.clear();  // 已排队回调靠 runtimeGeneration_ 自动失效
    ruleBooks_.clear(); scriptRegistry_.clear(); legacyHelp_.clear();
    activeLegacyLocks_.clear(); queuedDispatches_.clear(); lockDrainScheduled_ = false;
    taskCalls_.clear();
    pendingCoroutines_.clear();
    std::error_code ec;

    // 旧版 conf/ModList.json 的顺序由导入器保存到 data/mod_order.json。
    // 顺序会影响同名 speech/help/reply 的覆盖关系，因此不能依赖目录枚举顺序。
    std::map<std::string, size_t> legacyOrder;
    try {
        const fs::path orderFile = dnx_path(dir).parent_path() / "mod_order.json";
        std::ifstream orderIn(orderFile, std::ios::binary);
        if (orderIn) {
            json orderJson; orderIn >> orderJson;
            if (orderJson.is_array()) for (const auto& item : orderJson)
                if (item.is_string() && !legacyOrder.count(item.get<std::string>()))
                    legacyOrder[item.get<std::string>()] = legacyOrder.size();
        }
    } catch (const std::exception& ex) {
        DICE_LOG_WARN("[lua] ignored invalid legacy mod order: {}", ex.what());
    }

    // require("X") 能找到 mod/plugin 目录下的 X.lua（跨插件依赖，如 lua_useful_extensions）。
    if (state_) {
        fs::path base = dnx_path(dir).parent_path();   // data
        auto fwd = [](fs::path p) { std::string s = dnx_u8str(p); for (auto& c : s) if (c == '\\') c = '/'; return s; };
        std::string paths = fwd(dnx_path(dir) / "?.lua") + ";" + fwd(dnx_path(dir) / "?" / "init.lua") + ";"
                          + fwd(base / "plugin" / "?.lua") + ";" + fwd(base / "plugin" / "?" / "init.lua") + ";";
        for (auto& ed : extraDirs_)   // 规则包附加 lua 目录也可 require
            paths += fwd(dnx_path(ed) / "?.lua") + ";" + fwd(dnx_path(ed) / "?" / "init.lua") + ";";
        lua_getglobal(state_, "package");
        lua_getfield(state_, -1, "path");
        std::string cur = (lua_type(state_, -1) == LUA_TSTRING) ? argStr(state_, -1) : "";
        lua_pop(state_, 1);
        std::string np = paths + cur;
        lua_pushlstring(state_, np.data(), np.size());
        lua_setfield(state_, -2, "path");
        lua_pop(state_, 1);

        // cpath：默认与 Dice! 一致（不覆盖，兼容依赖原生库的 mod）；
        // dice/lua_cpath_strict=true 时收窄到插件/内置目录，阻断 loadlib 加载系统 .dll。
        if (s_cpathStrict) {
            std::string cpaths = fwd(dnx_path(dir) / "?.dll") + ";" + fwd(base / "plugin" / "?.dll") + ";";
            fs::path luaDir = fs::current_path() / "lua";
            if (fs::is_directory(luaDir, ec)) cpaths += fwd(luaDir / "?.dll") + ";";
            lua_getglobal(state_, "package");
            lua_pushlstring(state_, cpaths.data(), cpaths.size());
            lua_setfield(state_, -2, "cpath");
            lua_pop(state_, 1);
        }
    }

    // Discover first, then load deterministically. Reserved resource directories are
    // owned by their mod, not standalone plugins; symlinks/junctions are never followed.
    std::map<std::string, std::map<std::string, std::vector<std::string>>> descriptorSpeech;
    std::vector<std::pair<fs::path, bool>> singleFiles;
    const std::set<std::string> resources = {"reply", "script", "speech", "event", "model", "rulebook",
                                              ".git", ".svn", "node_modules"};
    auto isDir = [](const fs::path& p) { std::error_code e; return !dnx_isLinked(p) && fs::is_directory(p, e); };
    auto isFile = [](const fs::path& p) { std::error_code e; return !dnx_isLinked(p) && fs::is_regular_file(p, e); };
    auto safeList = [&](const fs::path& d) {
        std::vector<fs::path> paths; std::error_code e;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, e), end;
             it != end && paths.size() < 10000; it.increment(e)) {
            if (e) { e.clear(); continue; }
            if (!dnx_isLinked(it->path())) paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    };
    auto descriptor = [&](const fs::path& file) {
        std::string raw;
        if (!dnx_readFile(file, raw)) return false;
        const auto data = json::parse(stripJsonTrailingCommas(raw), nullptr, false, true);
        if (!data.is_object()) return false;
        for (const char* key : {"mod", "title", "ver", "version", "author", "helpdoc", "speech", "permissions"})
            if (data.contains(key)) return true;
        return false;
    };
    std::set<std::string> discovered;
    auto finishMod = [&](LuaMod& mod, const fs::path& path, const fs::path& desc, bool hasDirectory) {
        if (!discovered.insert(mod.name).second) {
            DICE_LOG_WARN("模组名称「{}」重复，已跳过后发现的目录。", mod.name); return;
        }
        if (!desc.empty()) mod.managedPaths.push_back(dnx_u8str(desc));
        if (hasDirectory) {
            mod.dir = dnx_u8str(path); mod.managedPaths.push_back(mod.dir);
            for (const auto& file : dnx_ownedFiles(path / "reply"))
                if (file.extension() == ".lua" || file.extension() == ".toml") ++mod.replies;
            for (const auto& file : dnx_ownedFiles(path / "script"))
                if (file.extension() == ".lua") ++mod.scripts;
            for (const auto& file : dnx_ownedFiles(path / "model"))
                if (file.extension() == ".xml") { mod.ruleCompat = true; break; }
        }
        mods_.push_back(std::move(mod));
    };
    std::function<void(const fs::path&, const std::string&, bool, int, bool)> scan;
    scan = [&](const fs::path& directory, const std::string& prefix, bool parentDisabled, int depth, bool ownerRoot) {
        if (depth > 16 || mods_.size() >= 2000 || !isDir(directory)) return;
        const auto entries = safeList(directory);
        std::set<std::string> claimed;
        for (const auto& file : entries) {
            if (!isFile(file)) continue;
            auto filename = dnx_u8str(file.filename());
            if (filename == "descriptor.json") continue;
            const bool disabled = filename.ends_with(".json.disabled");
            if (!disabled && !filename.ends_with(".json")) continue;
            auto name = filename.substr(0, filename.size() - (disabled ? 14 : 5));
            if (name.empty() || !descriptor(file)) continue;
            auto path = directory / dnx_path(name);
            if (!isDir(path) && isDir(directory / dnx_path(name + ".disabled"))) path = directory / dnx_path(name + ".disabled");
            const bool directoryDisabled = dnx_u8str(path.filename()).ends_with(".disabled");
            LuaMod mod; mod.name = prefix + name; mod.title = name;
            mod.ancestorDisabled = parentDisabled; mod.enabled = !parentDisabled && !disabled && !directoryDisabled;
            parseModDescriptor(file, mod, &descriptorSpeech[mod.name]);
            claimed.insert(name);
            finishMod(mod, path, file, isDir(path));
        }
        for (const auto& path : entries) {
            if (!isDir(path)) continue;
            auto filename = dnx_u8str(path.filename());
            const bool disabled = filename.ends_with(".disabled");
            const auto name = disabled ? filename.substr(0, filename.size() - 9) : filename;
            if (resources.count(legacy_mod::lower(name))) continue;
            const auto inner = path / "descriptor.json";
            bool looksMod = isFile(inner);
            for (const auto& sub : resources) if (isDir(path / sub) && sub.front() != '.' && sub != "node_modules") looksMod = true;
            if (looksMod && !claimed.count(name)) {
                LuaMod mod; mod.name = prefix + name; mod.title = name;
                mod.ancestorDisabled = parentDisabled; mod.enabled = !parentDisabled && !disabled;
                if (isFile(inner)) parseModDescriptor(inner, mod, &descriptorSpeech[mod.name]);
                // Inner descriptor is renamed with its directory, not separately.
                finishMod(mod, path, {}, true);
            }
            bool inactive = parentDisabled || disabled;
            for (const auto& mod : mods_) if (mod.name == prefix + name && !mod.enabled) inactive = true;
            scan(path, prefix + name + ".", inactive, depth + 1, looksMod);
        }
        // A Lua file inside a mod is a resource, not an executable entry point.
        if (!ownerRoot && depth == 0) for (const auto& file : entries) {
            const auto filename = dnx_u8str(file.filename());
            const bool disabled = filename.ends_with(".lua.disabled");
            if (isFile(file) && (file.extension() == ".lua" || disabled))
                singleFiles.emplace_back(file, !disabled && !parentDisabled);
        }
    };
    scan(dnx_path(dir), "", false, 0, false);
    for (const auto& extra : extraDirs_) scan(dnx_path(extra), "", false, 0, false);
    std::stable_sort(mods_.begin(), mods_.end(), [&](const auto& a, const auto& b) {
        const auto ai = legacyOrder.find(a.name), bi = legacyOrder.find(b.name);
        const size_t ar = ai == legacyOrder.end() ? static_cast<size_t>(-1) : ai->second;
        const size_t br = bi == legacyOrder.end() ? static_cast<size_t>(-1) : bi->second;
        return ar != br ? ar < br : a.name < b.name;
    });
    for (const auto& mod : mods_) if (mod.enabled)
        for (const auto& [topic, text] : mod.helpdoc) legacyHelp_[topic] = {topic, text, mod.name};
    // Old loadLua names are a global last-loaded-wins namespace. Registration
    // never executes a resource; disabled parents and resources remain inactive.
    for (const auto& mod : mods_) if (mod.enabled && !mod.dir.empty()) {
        const auto scriptRoot = dnx_path(mod.dir) / "script";
        for (const auto& file : dnx_ownedFiles(scriptRoot)) if (file.extension() == ".lua") {
            auto name = dnx_u8str(file.lexically_relative(scriptRoot));
            name.resize(name.size() - 4);
            for (auto& c : name) if (c == '/' || c == '\\') c = '.';
            scriptRegistry_[name] = {file, mod.name};
            // Old cut_stem(parent) registers foo/init.lua as "foo". Keep Next's
            // explicit foo.init spelling too; both retain the same safe path.
            if (legacy_mod::lower(dnx_u8str(file.stem())) == "init" && name.size() > 5 && legacy_mod::lower(name).ends_with(".init"))
                scriptRegistry_[name.substr(0, name.size() - 5)] = {file, mod.name};
        }
    }
    for (auto& mod : mods_) if (mod.enabled) {
        for (const auto& [key, value] : descriptorSpeech[mod.name]) speech_[key] = value;
        if (!mod.dir.empty()) loadModReplies(mod);
    }
    const auto pluginDir = dnx_path(dir).parent_path() / "plugin";
    if (isDir(pluginDir)) for (const auto& file : safeList(pluginDir)) {
        const auto filename = dnx_u8str(file.filename());
        const bool disabled = filename.ends_with(".lua.disabled");
        if (isFile(file) && (file.extension() == ".lua" || disabled)) singleFiles.emplace_back(file, !disabled);
    }
    for (const auto& [file, enabled] : singleFiles)
        try { loadModFile(file, enabled); }
        catch (const std::exception& ex) { DICE_LOG_ERROR("Lua 插件「{}」加载失败：{}", dnx_u8str(file.filename()), ex.what()); }

    // 目录型旧 Mod 的 reply ID 是全局覆盖键；单文件插件仍保留各自独立的规则。
    std::set<std::string> seenReplyIds;
    for (size_t i = replyRules_.size(); i > 0; --i) {
        auto& rule = replyRules_[i - 1];
        if (!rule.legacySourceFile.empty() || seenReplyIds.insert(rule.name).second) continue;
        if (rule.echoRef > 0)
            luaL_unref(state_, LUA_REGISTRYINDEX, rule.echoRef);
        replyRules_.erase(replyRules_.begin() + (i - 1));
    }

    // 原版消息链先调用 listen_game，再调用 listen_order。团规则不能被通用兜底抢先。
    std::stable_partition(replyRules_.begin(), replyRules_.end(),
        [](const ReplyRule& rule) { return rule.requiresGame || !rule.requiredRule.empty(); });
    auto directory = std::make_shared<std::vector<DirectoryRule>>();
    for (const auto& rule : replyRules_) directory->push_back({rule.modName, rule.type, rule.requiredRule,
        rule.requiresGame, rule.matchPatterns, rule.prefixPatterns, rule.searchPatterns, rule.limits});
    std::atomic_store(&directoryRules_, DirectorySnapshot(std::move(directory)));

    // 与旧版 build() 一致：所有活动 mod 合并完再注册事件，避免同名事件被后载入
    // 模组覆盖时留下两套定时回调。首次注册会立即执行一次，然后按 cycle 重排。
    for (const auto& [id, unused] : cycleEvents_) {
        (void)unused;
        scheduleCycleEvent(id, true);
    }
    lastClockTime_ = nowSeconds();
    scheduleClockTick();

    DICE_LOG_INFO("插件扫描结束：目录「{}」及插件目录共发现 {} 个模组／插件，注册 {} 条回复规则。",
                  dir, (int)mods_.size(), (int)replyRules_.size());
    return (int)mods_.size();
}

namespace {

static bool dnx_dataOnly(lua_State* state, int index, int depth = 0, size_t* budget = nullptr, bool functions = false) {
    size_t remaining = 10000;
    if (!budget) budget = &remaining;
    if (!*budget) return false;
    --*budget;
    index = lua_absindex(state, index);
    const auto type = lua_type(state, index);
    if (type == LUA_TFUNCTION) return functions;
    if (type == LUA_TNUMBER) return std::isfinite(lua_tonumber(state, index));
    if (type != LUA_TTABLE) return type == LUA_TNIL || type == LUA_TSTRING || type == LUA_TBOOLEAN;
    if (depth >= 32) return false;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        const bool valid = (lua_type(state, -2) == LUA_TSTRING || lua_type(state, -2) == LUA_TNUMBER) && dnx_dataOnly(state, -1, depth + 1, budget, functions);
        lua_pop(state, 1);
        if (!valid) { lua_pop(state, 1); return false; }
    }
    return true;
}
static bool dnx_inlineDeckValid(const json& value) {
    return value.is_array() && !value.empty() && value.size() <= 10000
        && std::all_of(value.begin(), value.end(), [](const auto& item) { return item.is_string(); });
}
struct DnxLegacyTomlReply {
    std::string name, type, requiredRule, echoLua, echoJs, staticEcho, echoDeckJson;
    std::vector<std::string> match, prefix, search, regex;
    std::string unsupportedLimit;
    bool staticEchoDefined = false;
    std::shared_ptr<legacy_mod::Limits> limits;
};

// Use the SAME TOML 1.0 parser as Dice!, not a line-oriented approximation.
// Files are data only; neither parsing nor JSON conversion executes a script.
static std::vector<DnxLegacyTomlReply> dnx_parseLegacyReplyToml(const fs::path& path) {
    std::error_code ec;
    if (fs::file_size(path, ec) > 4 * 1024 * 1024 || ec)
        throw std::runtime_error("reply TOML exceeds the 4 MiB safety limit or cannot be read");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open TOML");
    const auto document = toml::parse(in, dnx_u8str(path));
    std::ostringstream serialized;
    serialized << toml::json_formatter{document};
    const auto root = json::parse(serialized.str());
    std::vector<DnxLegacyTomlReply> out;
    if (!root.contains("reply")) return out;
    if (!root["reply"].is_object()) throw std::runtime_error("reply must be a TOML table");
    for (const auto& [id, value] : root["reply"].items()) {
        if (!value.is_object()) throw std::runtime_error("reply entry must be a table");
        DnxLegacyTomlReply reply;
        reply.name = id;
        reply.type = value.value("type", std::string());
        reply.requiredRule = value.value("rule", std::string());
        const auto keyword = value.value("keyword", json::object());
        if (!keyword.is_object()) throw std::runtime_error("keyword must be a table");
        auto words = [](const json& input, std::vector<std::string>& result) {
            if (input.is_string()) result.push_back(input.get<std::string>());
            else if (input.is_array()) {
                for (const auto& item : input) {
                    if (!item.is_string()) throw std::runtime_error("keyword entries must be strings");
                    result.push_back(item.get<std::string>());
                }
            } else throw std::runtime_error("keyword must be a string or array");
        };
        for (const auto& [kind, input] : keyword.items()) {
            const auto key = legacy_mod::lower(kind);
            if (key == "match") words(input, reply.match);
            else if (key == "prefix") words(input, reply.prefix);
            else if (key == "search") words(input, reply.search);
            else if (key == "regex") words(input, reply.regex);
            else reply.unsupportedLimit = "keyword." + kind;
        }
        reply.limits = std::make_shared<legacy_mod::Limits>();
        if (!legacy_mod::parseLimits(value.value("limit", json()), *reply.limits, reply.unsupportedLimit))
            reply.limits.reset();
        const auto echo = value.value("echo", json());
        if (echo.is_string()) { reply.staticEcho = echo.get<std::string>(); reply.staticEchoDefined = true; }
        else if (dnx_inlineDeckValid(echo)) reply.echoDeckJson = echo.dump();
        else if (echo.is_object()) {
            if (echo.contains("js") || echo.contains("py")) reply.echoJs = echo.contains("js") ? "js" : "py";
            if (echo.contains("lua")) reply.echoLua = echo.at("lua").get<std::string>();
            if (!echo.contains("lua") && !echo.contains("js") && !echo.contains("py"))
                reply.unsupportedLimit = "echo action";
            if (echo.size() != 1 && !echo.contains("js") && !echo.contains("py")) reply.unsupportedLimit = "echo 混合动作";
        } else reply.unsupportedLimit = "echo action";
        out.push_back(std::move(reply));
    }
    return out;
}
} // namespace

// 载入一个旧 Dice! mod 的 speech + reply(.lua/.toml) + event(.lua)。
void LuaPluginManager::loadModReplies(LuaMod& mod) {
    ScriptFaultScope fault(suspendedMods_, mod.name);
    ProcessingScope logScope(ProcessingSource{"Lua 插件", mod.title.empty() ? mod.name : mod.title, mod.dir, "加载"}, "插件加载");
    auto compatibilityWarning = [&](std::string text) {
        if (mod.compatibilityWarnings.size() < 100) mod.compatibilityWarnings.push_back(std::move(text));
    };
    std::error_code ec;
    // 1) speech 词条：平铺标量或随机候选数组；与旧版 dict_ci 同样忽略 ASCII 大小写。
    fs::path sp = dnx_path(mod.dir) / "speech";
    if (fs::is_directory(sp, ec)) {
        for (const auto& file : dnx_ownedFiles(sp)) { fs::directory_entry f(file, ec);
            if (ec || !f.is_regular_file()) continue;
            auto ext = f.path().extension();
            if (ext != ".yaml" && ext != ".yml") continue;
            try {
                std::string ytext; if (!dnx_readFile(f.path(), ytext)) continue;
                YAML::Node doc = YAML::Load(ytext);
                if (doc.IsMap())
                    for (auto it = doc.begin(); it != doc.end(); ++it) {
                        const auto key = legacy_mod::lower(it->first.as<std::string>());
                        if (it->second.IsScalar()) speech_[key] = {it->second.as<std::string>()};
                        else if (it->second.IsSequence() && std::all_of(it->second.begin(), it->second.end(),
                            [](const YAML::Node& item) { return item.IsScalar(); }))
                            speech_[key] = it->second.as<std::vector<std::string>>();
                    }
            } catch (const std::exception& ex) {
                DICE_LOG_ERROR("[lua] speech '{}' parse error: {}", dnx_u8str(f.path().filename()), ex.what());
            }
        }
    }
    auto collectReplies = [&]() {
        const auto firstRule = replyRules_.size();
        lua_getglobal(state_, "msg_reply");
        if (!lua_istable(state_, -1)) { lua_pop(state_, 1); return; }
        lua_pushnil(state_);
        while (lua_next(state_, -2)) {                                     // key=-2, entry=-1
            lua_pushvalue(state_, -2);                                     // 复制 key（避免 lua_next 中转换原 key）
            ReplyRule rule;
            rule.name = argStr(state_, -1); rule.modName = mod.name; rule.modDir = mod.dir;
            lua_pop(state_, 1);
            if (lua_istable(state_, -1)) {
                lua_getfield(state_, -1, "keyword");
                bool validKeywords = dnx_dataOnly(state_, -1) && lua_istable(state_, -1);
                if (validKeywords) {
                    const auto fields = luaToJson(state_, -1);
                    const std::map<std::string, std::vector<std::string>*> targets = {
                        {"match", &rule.matchPatterns}, {"prefix", &rule.prefixPatterns},
                        {"search", &rule.searchPatterns}, {"regex", &rule.regexPatterns}
                    };
                    for (const auto& [kind, value] : fields.items()) {
                        const auto target = targets.find(legacy_mod::lower(kind));
                        if (target == targets.end()) { validKeywords = false; continue; }
                        if (value.is_string()) target->second->push_back(value.get<std::string>());
                        else if (value.is_array()) for (const auto& item : value) {
                            if (item.is_string()) target->second->push_back(item.get<std::string>());
                            else validKeywords = false;
                        } else validKeywords = false;
                    }
                }
                lua_pop(state_, 1);
                if (!validKeywords) { compatibilityWarning("回复 " + rule.name + " 已跳过：keyword 包含不支持的匹配条件。"); lua_pop(state_, 1); continue; }
                lua_getfield(state_, -1, "rule");
                if (!lua_isnil(state_, -1) && !lua_isstring(state_, -1)) {
                    compatibilityWarning("回复 " + rule.name + " 已跳过：rule 不是规则名称。"); lua_pop(state_, 2); continue;
                }
                if (lua_isstring(state_, -1)) rule.requiredRule = argStr(state_, -1);
                lua_pop(state_, 1);
                lua_getfield(state_, -1, "type");
                if (!lua_isnil(state_, -1) && !lua_isstring(state_, -1)) {
                    compatibilityWarning("回复 " + rule.name + " 已跳过：type 无效。"); lua_pop(state_, 2); continue;
                }
                if (lua_isstring(state_, -1)) {
                    std::string type = argStr(state_, -1);
                    for (auto& c : type) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
                    rule.type = type;
                    rule.requiresGame = type == "game";
                }
                lua_pop(state_, 1);
                // Shared fail-closed limits for Lua and TOML; never discard an unknown restriction.
                lua_getfield(state_, -1, "limit");
                const bool dataOnly = dnx_dataOnly(state_, -1);
                auto limits = std::make_shared<legacy_mod::Limits>();
                std::string limitError;
                auto limitJson = luaToJson(state_, -1);
                if (lua_istable(state_, -1) && lua_rawlen(state_, -1) == 0 && limitJson.is_array() && limitJson.empty())
                    limitJson = json::object();
                const bool validLimit = dataOnly && legacy_mod::parseLimits(limitJson, *limits, limitError);
                if (!dataOnly) limitError = "limit 含非数据值、循环引用或超过嵌套限制";
                lua_pop(state_, 1);
                if (validLimit) rule.limits = std::move(limits);
                else {
                    compatibilityWarning("回复 " + rule.name + " 已跳过：" + limitError + "。");
                    lua_pop(state_, 1); // entry; retain iteration table and key
                    continue;
                }
                // echo：函数、静态串，或 {lua="脚本名"}。
                lua_getfield(state_, -1, "echo");
                if (lua_isfunction(state_, -1)) rule.echoRef = luaL_ref(state_, LUA_REGISTRYINDEX);
                else if (lua_type(state_, -1) == LUA_TSTRING) { rule.staticEcho = argStr(state_, -1); rule.staticEchoDefined = true; lua_pop(state_, 1); }
                else if (lua_istable(state_, -1)) {
                    auto action = dnx_dataOnly(state_, -1, 0, nullptr, true) ? luaToJson(state_, -1) : json();
                    lua_getfield(state_, -1, "lua");
                    const bool function = action.is_object() && action.size() == 1 && lua_isfunction(state_, -1);
                    if (function) { rule.echoRef = luaL_ref(state_, LUA_REGISTRYINDEX); rule.echoTakesMessage = false; }
                    else lua_pop(state_, 1);
                    if (!function) {
                        if (dnx_inlineDeckValid(action)) rule.echoDeckJson = action.dump();
                        else if (action.is_object() && action.size() == 1 && action.contains("lua") && action["lua"].is_string())
                            rule.echoScript = action["lua"].get<std::string>();
                        else compatibilityWarning("回复 " + rule.name + " 已跳过：无效或不安全的 echo 动作（不执行旧 JS/Python 或混合动作）。");
                    }
                    lua_pop(state_, 1);
                } else lua_pop(state_, 1);
            }
            if (rule.type != "nor" && rule.type != "reply" && rule.type != "order" && rule.type != "both" && rule.type != "game") {
                compatibilityWarning("回复 " + rule.name + " 已跳过：不支持的 type " + rule.type + "。");
                if (rule.echoRef) luaL_unref(state_, LUA_REGISTRYINDEX, rule.echoRef);
                lua_pop(state_, 1); continue;
            }
            if ((rule.echoRef != 0 || !rule.echoScript.empty() || rule.staticEchoDefined || !rule.echoDeckJson.empty())
                && (!rule.matchPatterns.empty() || !rule.prefixPatterns.empty()
                    || !rule.searchPatterns.empty() || !rule.regexPatterns.empty()))
                replyRules_.push_back(std::move(rule));
            lua_pop(state_, 1);
        }
        lua_pop(state_, 1);
        std::stable_sort(replyRules_.begin() + firstRule, replyRules_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    };

    auto collectEvents = [&]() {
        lua_getglobal(state_, "event");
        if (!lua_istable(state_, -1)) { lua_pop(state_, 1); return; }
        lua_pushnil(state_);
        while (lua_next(state_, -2)) {
            lua_pushvalue(state_, -2);
            LegacyCycleEvent event;
            event.id = argStr(state_, -1); event.modName = mod.name; event.modDir = mod.dir;
            lua_pop(state_, 1);
            std::string error;
            if (!lua_istable(state_, -1) || !dnx_dataOnly(state_, -1, 0, nullptr, true)) error = "事件必须是有界数据表，不能循环引用";
            else {
                const auto definition = luaToJson(state_, -1);
                event.defaultsJson = definition.dump();
                const auto trigger = definition.value("trigger", json());
                if (!trigger.is_object() || trigger.empty()) error = "trigger 必须声明有效触发器";
                else for (const auto& [kind, value] : trigger.items()) {
                    if (kind == "cycle") {
                        double interval = 0;
                        if (value.is_number()) interval = value.get<double>();
                        else if (value.is_object()) {
                            for (const auto& [unit, number] : value.items()) {
                                const double scale = unit == "second" ? 1 : unit == "minute" ? 60 : unit == "hour" ? 3600 : unit == "day" ? 86400 : 0;
                                if (scale == 0 || !number.is_number() || number.get<double>() < 0) { error = "无效的 cycle 时间单位或数值"; break; }
                                interval += scale * number.get<double>();
                            }
                        }
                        if (!std::isfinite(interval) || interval < 1 || interval > 7 * 86400)
                            error = "cycle 必须在 1 秒至 7 天之间";
                        else event.intervalSeconds = interval;
                    } else if (kind == "clock") {
                        const auto clocks = value.is_array() ? value : json::array({value});
                        if (clocks.empty() || clocks.size() > 1440) { error = "clock 列表为空或过长"; continue; }
                        for (const auto& clock : clocks) {
                            if (!clock.is_object()) { error = "clock 必须是 hour/minute 对象"; break; }
                            long long hour = 0, minute = 0;
                            if (!legacy_mod::scalarNumber(clock.value("hour", json(0)), hour) || hour > 23
                                || !legacy_mod::scalarNumber(clock.value("minute", json(0)), minute) || minute > 59
                                || clock.size() > 2) { error = "clock 的小时或分钟无效"; break; }
                            for (const auto& [key, unused] : clock.items()) if (key != "hour" && key != "minute") error = "不支持的 clock 字段 " + key;
                            event.clocks.emplace_back(static_cast<int>(hour), static_cast<int>(minute));
                        }
                    } else if (kind == "hook") {
                        static const std::set<std::string> allowed = {"startup", "dayend", "daynew", "message", "friendrequest", "friendadd",
                            "grouprequest", "groupadd", "groupwelcome", "groupdecrease", "grouprecall", "groupupload", "poke", "logend"};
                        if (!value.is_string() || !allowed.count(legacy_mod::lower(value.get<std::string>())))
                            error = "不支持或不安全的 hook（不覆盖授权、敏感词或黑名单处理）";
                        else event.hooks.push_back(legacy_mod::lower(value.get<std::string>()));
                    } else error = "不支持的 trigger." + kind;
                }
                lua_getfield(state_, -1, "action");
                if (lua_istable(state_, -1)) {
                    lua_pushnil(state_); size_t fields = 0;
                    while (lua_next(state_, -2)) { ++fields; lua_pop(state_, 1); }
                    lua_getfield(state_, -1, "lua");
                    if (lua_isfunction(state_, -1)) event.actionRef = luaL_ref(state_, LUA_REGISTRYINDEX);
                    else { if (lua_type(state_, -1) == LUA_TSTRING) event.actionLua = argStr(state_, -1); lua_pop(state_, 1); }
                    if (fields != 1 || (event.actionRef == 0 && event.actionLua.empty()))
                        error = "action 仅接受单一 Lua 动作，不执行混合 JS/Python 动作";
                } else error = "缺少有效的 action";
                lua_pop(state_, 1);
            }
            if (error.empty() && !event.id.empty()) {
                if (const auto previous = cycleEvents_.find(event.id); previous != cycleEvents_.end() && previous->second.actionRef)
                    luaL_unref(state_, LUA_REGISTRYINDEX, previous->second.actionRef);
                cycleEvents_[event.id] = std::move(event); ++mod.events;
            } else {
                if (event.actionRef) luaL_unref(state_, LUA_REGISTRYINDEX, event.actionRef);
                compatibilityWarning("事件 " + event.id + " 已跳过：" + error + "。");
                DICE_LOG_WARN("模组「{}」事件「{}」已跳过：{}", mod.name, event.id, error);
            }
            lua_pop(state_, 1);
        }
        lua_pop(state_, 1);
    };

    auto loadLuaDefinition = [&](const fs::path& file) {
        lua_newtable(state_); lua_setglobal(state_, "msg_reply");
        lua_newtable(state_); lua_setglobal(state_, "event");
        if (dnx_dofile(state_, file) != LUA_OK) {
            ++lastLoadErrors_;
            DICE_LOG_ERROR("模组定义「{}」加载失败：{}", dnx_u8str(file.filename()), argStr(state_, -1));
            compatibilityWarning(dnx_u8str(file.filename()) + " 加载失败，请查看运行日志。");
            lua_pop(state_, 1); return;
        }
        collectReplies();
        collectEvents();
    };

    // 2) reply/*.lua|toml。TOML 是旧版规则模组的主要声明格式。
    fs::path rp = dnx_path(mod.dir) / "reply";
    loadingModDir_ = mod.dir;
    if (fs::is_directory(rp, ec)) for (const auto& file : dnx_ownedFiles(rp)) { fs::directory_entry f(file, ec);
        if (ec || !f.is_regular_file()) continue;
        if (f.path().extension() == ".lua") loadLuaDefinition(f.path());
        else if (f.path().extension() == ".toml") {
            try {
                for (const auto& legacy : dnx_parseLegacyReplyToml(f.path())) {
                    if (!legacy.unsupportedLimit.empty()) {
                        DICE_LOG_WARN("[lua] TOML reply '{}:{}' skipped: unsupported condition '{}'",
                                      mod.name, legacy.name, legacy.unsupportedLimit);
                        compatibilityWarning(dnx_u8str(f.path().filename()) + " / " + legacy.name + " 已跳过：不支持的条件 " + legacy.unsupportedLimit + "，不会忽略条件后执行。");
                        continue;
                    }
                    ReplyRule rule;
                    rule.name = legacy.name; rule.modName = mod.name; rule.modDir = mod.dir;
                    rule.matchPatterns = legacy.match; rule.prefixPatterns = legacy.prefix;
                    rule.searchPatterns = legacy.search; rule.regexPatterns = legacy.regex;
                    rule.requiredRule = legacy.requiredRule; rule.echoScript = legacy.echoLua; rule.staticEcho = legacy.staticEcho;
                    rule.staticEchoDefined = legacy.staticEchoDefined; rule.echoDeckJson = legacy.echoDeckJson; rule.limits = legacy.limits;
                    std::string legacyType = legacy.type;
                    for (auto& c : legacyType) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
                    if (!legacyType.empty()) rule.type = legacyType;
                    rule.requiresGame = legacyType == "game";
                    if (rule.type != "nor" && rule.type != "reply" && rule.type != "order" && rule.type != "both" && rule.type != "game") {
                        compatibilityWarning("回复 " + rule.name + " 已跳过：不支持的 type " + rule.type + "。"); continue;
                    }
                    if (!legacy.echoJs.empty()) {
                        DICE_LOG_WARN("[lua] TOML reply '{}:{}' uses legacy Dice! JS action '{}'; skipped (not a SealDice plugin)",
                                      mod.name, legacy.name, legacy.echoJs);
                        compatibilityWarning(dnx_u8str(f.path().filename()) + " / " + legacy.name + " 已跳过：旧 Dice! JS 不属于海豹 JS 插件体系，不提供兼容执行。");
                        continue;
                    }
                    if ((!rule.echoScript.empty() || rule.staticEchoDefined || !rule.echoDeckJson.empty())
                        && (!rule.matchPatterns.empty() || !rule.prefixPatterns.empty()
                            || !rule.searchPatterns.empty() || !rule.regexPatterns.empty()))
                        replyRules_.push_back(std::move(rule));
                }
            } catch (const std::exception& ex) {
                DICE_LOG_ERROR("[lua] reply TOML '{}' parse error: {}", dnx_u8str(f.path().filename()), ex.what());
                compatibilityWarning(dnx_u8str(f.path().filename()) + " 解析失败，请检查 TOML 语法并查看运行日志。");
            }
        }
    }

    // 3) event/*.lua：混合 cycle / clock / hook 的每个触发器都必须有效。
    fs::path ep = dnx_path(mod.dir) / "event";
    if (fs::is_directory(ep, ec)) for (const auto& file : dnx_ownedFiles(ep))
        if (file.extension() == ".lua") loadLuaDefinition(file);

    for (const auto& file : dnx_ownedFiles(dnx_path(mod.dir) / "rulebook")) {
        if (file.extension() != ".yaml" && file.extension() != ".yml") continue;
        try {
            std::string text; if (!dnx_readFile(file, text)) throw std::runtime_error("文件不可读或超过 4 MiB");
            const auto document = YAML::Load(text);
            const auto rule = document["rule"].as<std::string>();
            if (rule.empty() || rule.size() > 256) throw std::runtime_error("rule 名称无效");
            auto& book = ruleBooks_[legacy_mod::lower(rule)];
            if (const auto manual = document["manual"]; manual && manual.IsMap()) {
                if (manual.size() > 10000) throw std::runtime_error("manual 条目过多");
                for (const auto& item : manual)
                    book.manual[legacy_mod::lower(item.first.as<std::string>())] = item.second.as<std::string>();
            }
            if (const auto tapes = document["tape"]; tapes && tapes.IsMap()) {
                if (tapes.size() > 1000) throw std::runtime_error("tape 条目过多");
                for (const auto& item : tapes) {
                    const auto name = item.first.as<std::string>(); const auto action = item.second;
                    if (name.empty() || !action.IsMap() || action.size() != 1
                        || (!action["lua"] && !action["text"])) {
                        compatibilityWarning("规则 " + rule + " 的 tape " + name + " 已跳过：仅支持单一 Lua 或文本动作。");
                        continue;
                    }
                    LegacyCycleEvent tape;
                    tape.id = "tape:" + rule + ":" + name; tape.modName = mod.name; tape.modDir = mod.dir;
                    if (action["lua"]) tape.actionLua = action["lua"].as<std::string>();
                    else tape.actionText = action["text"].as<std::string>();
                    book.tapes[legacy_mod::lower(name)] = std::move(tape);
                }
            }
        } catch (const std::exception& ex) {
            compatibilityWarning(dnx_u8str(file.filename()) + " 规则书读取失败：" + ex.what());
            DICE_LOG_WARN("规则书「{}」读取失败：{}", dnx_u8str(file.filename()), ex.what());
        }
    }
    for (const auto& file : dnx_ownedFiles(dnx_path(mod.dir) / "model")) if (file.extension() == ".xml") {
        std::string text;
        if (!dnx_readFile(file, text)) { compatibilityWarning(dnx_u8str(file.filename()) + " 模型不可读或超过 4 MiB。"); continue; }
        for (const auto& warning : legacy_model::parse(text).warnings) compatibilityWarning(dnx_u8str(file.filename()) + "：" + warning);
    }
    loadingModDir_.clear();
}

void LuaPluginManager::scheduleCycleEvent(const std::string& id, bool runNow) {
    if (!scheduler_) return;
    const auto found = cycleEvents_.find(id);
    if (found == cycleEvents_.end() || found->second.intervalSeconds <= 0) return;
    const uint64_t generation = runtimeGeneration_;
    const auto event = found->second;
    postCallback(event.intervalSeconds, [id, generation](LuaPluginManager& owner) { owner.runCycleEvent(id, generation); });
    if (runNow) { json context = json::object(); runLegacyEvent(event, context, true); }
}

void LuaPluginManager::runCycleEvent(const std::string& id, uint64_t generation) {
    std::lock_guard<ScriptMutex> guard(mutex_);
    if (!state_ || generation != runtimeGeneration_) return;
    const auto event = cycleEvents_.find(id);
    if (event == cycleEvents_.end() || suspendedMods_.count(event->second.modName)) return;
    scheduleCycleEvent(id, true);
}

static std::string dnx_contextString(const json& context, const char* key) {
    const auto found = context.find(key);
    if (found == context.end()) return {};
    if (found->is_string()) return found->get<std::string>();
    if (found->is_number_integer() || found->is_number_unsigned()) return found->dump();
    return {};
}

bool LuaPluginManager::runLegacyEvent(const LegacyCycleEvent& action, json& context, bool mayYield) {
    if (!state_ || suspendedMods_.count(action.modName)) { context["@dice_event_failed"] = true; return false; }
    ScriptFaultScope fault(suspendedMods_, action.modName);
    auto values = json::parse(action.defaultsJson, nullptr, false);
    if (!values.is_object()) values = json::object();
    if (context.is_object()) for (const auto& [key, value] : context.items()) values[key] = value;
    const auto platform = dnx_contextString(values, "platform");
    const auto gid = dnx_contextString(values, "gid"), uid = dnx_contextString(values, "uid");
    const auto adapter = dnx_contextString(values, "adapterId");
    if (groupGate_ && !gid.empty() && !groupGate_(platform, gid, "lua:" + action.modName, adapter)) return false;
    const int trust = trustLookup_ && !uid.empty() ? trustLookup_(uid, gid, platform, adapter, gid.empty()) : 0;
    if (trust < 0) return false;
    InvocationScope invocation(*this, platform, adapter, gid, uid, action.modName);
    ProcessingScope processing(loggingSource(action.modName, action.id), "Lua 事件「" + action.id + "」");
    values["id"] = action.id; values["mod"] = action.modName;
    if (!values.contains("time")) values["time"] = nowSeconds();
    const int base = lua_gettop(state_);
    lua_getglobal(state_, "event"); const int oldEvent = luaL_ref(state_, LUA_REGISTRYINDEX);
    lua_getglobal(state_, "msg"); const int oldMessage = luaL_ref(state_, LUA_REGISTRYINDEX);
    jsonToLua(state_, values);
    lua_getglobal(state_, "__dnx_wrap_msg");
    if (lua_isfunction(state_, -1)) {
        lua_pushvalue(state_, -2); lua_pushinteger(state_, trust);
        if (dnx_pcall(state_, 2, 1) == LUA_OK) lua_remove(state_, -2);
        else lua_pop(state_, 1);
    } else lua_pop(state_, 1);
    lua_pushvalue(state_, -1); lua_setglobal(state_, "event");
    lua_pushvalue(state_, -1); lua_setglobal(state_, "msg");
    const int contextRef = luaL_ref(state_, LUA_REGISTRYINDEX);
    const auto previousDir = loadingModDir_, previousPlatform = activePlatform_, previousAdapter = activeAdapter_;
    loadingModDir_ = action.modDir; activePlatform_ = platform; activeAdapter_ = adapter;
    int status = LUA_OK;
    bool pending = false;
    if (!action.actionText.empty()) {
        std::map<std::string, std::string> vars;
        for (const auto& [key, value] : values.items()) if (value.is_string()) vars[key] = value.get<std::string>();
        context["msg_reply"] = formatTemplate(action.actionText, vars);
        context["blocked"] = true;
    } else {
        if (action.actionRef) lua_rawgeti(state_, LUA_REGISTRYINDEX, action.actionRef);
        else {
            const auto script = resolveScript(action.actionLua, action.modDir);
            if (!script.empty()) status = dnx_loadfile(state_, script);
            else status = luaL_loadbufferx(state_, action.actionLua.data(), action.actionLua.size(), action.id.c_str(), "t");
            if (status == LUA_OK) dnx_isolateScriptChunk(state_);
        }
        if (status == LUA_OK && mayYield && scheduler_ && pendingCoroutines_.size() < 256) {
            auto* coroutine = lua_newthread(state_);
            const int reference = luaL_ref(state_, LUA_REGISTRYINDEX);
            lua_xmove(state_, coroutine, 1);
            int count = 0; status = dnx_resume(coroutine, state_, 0, &count, true);
            if (status == LUA_YIELD) {
                PendingCoroutine task;
                task.generation = runtimeGeneration_; task.threadRef = reference; task.messageRef = contextRef; task.event = true;
                task.ruleName = action.id; task.modName = action.modName; task.modDir = action.modDir;
                task.platform = platform; task.adapterId = adapter; task.gid = gid; task.uid = uid; task.logContext = activeProcessingContext;
                task.virtualEvent = invocationOrigin_.virtualEvent;
                pendingCoroutines_[reference] = std::move(task); pending = true;
                scheduleCoroutineResume(reference, yieldedDelaySeconds(coroutine, count));
                if (!pendingCoroutines_.count(reference)) context["@dice_event_failed"] = true;
            } else {
                if (status != LUA_OK) { const auto error = dnx_coroutineError(coroutine); lua_pushlstring(state_, error.data(), error.size()); }
                luaL_unref(state_, LUA_REGISTRYINDEX, reference);
            }
        } else if (status == LUA_OK) status = dnx_pcall(state_, 0, 0);
        if (status != LUA_OK && status != LUA_YIELD)
            logPluginFailure("执行事件「" + action.id + "」", dnx_normalizeLuaText(argStr(state_, -1)));
        if (status != LUA_OK && status != LUA_YIELD) context["@dice_event_failed"] = true;
        if (!pending && status == LUA_OK) {
            lua_rawgeti(state_, LUA_REGISTRYINDEX, contextRef);
            const auto result = luaToJson(state_, -1);
            if (result.is_object()) context = result;
            lua_pop(state_, 1);
        }
    }
    loadingModDir_ = previousDir; activePlatform_ = previousPlatform; activeAdapter_ = previousAdapter;
    lua_rawgeti(state_, LUA_REGISTRYINDEX, oldEvent); lua_setglobal(state_, "event");
    lua_rawgeti(state_, LUA_REGISTRYINDEX, oldMessage); lua_setglobal(state_, "msg");
    luaL_unref(state_, LUA_REGISTRYINDEX, oldEvent); luaL_unref(state_, LUA_REGISTRYINDEX, oldMessage);
    if (!pending) luaL_unref(state_, LUA_REGISTRYINDEX, contextRef);
    lua_settop(state_, base);
    return status == LUA_OK && context.is_object() && context.value("blocked", json(false)).is_boolean() && context["blocked"] == true;
}

bool LuaPluginManager::hasHook(const std::string& hook) const {
    std::lock_guard<ScriptMutex> guard(mutex_);
    if (!state_) return false;
    const auto key = legacy_mod::lower(hook);
    for (const auto& [id, action] : cycleEvents_) if (!suspendedMods_.count(action.modName)
        && std::find(action.hooks.begin(), action.hooks.end(), key) != action.hooks.end()) return true;
    return false;
}

bool LuaPluginManager::dispatchHook(const std::string& hook, json& context, std::vector<ProcessingSource>* sources) {
    std::lock_guard<ScriptMutex> guard(mutex_);
    if (!state_ || !context.is_object()) return false;
    const auto key = legacy_mod::lower(hook);
    context["Event"] = hook; context["hook"] = hook;
    bool blocked = false, denied = false, failed = false;
    for (const auto& [id, action] : cycleEvents_)
        if (std::find(action.hooks.begin(), action.hooks.end(), key) != action.hooks.end()) {
            blocked = runLegacyEvent(action, context, key == "startup" || key == "dayend" || key == "daynew") || blocked;
            denied = denied || (context.contains("approval") && context["approval"].is_boolean() && context["approval"] == false);
            failed = failed || (context.contains("@dice_event_failed") && context["@dice_event_failed"] == true);
            if (sources) addProcessingSource(*sources, loggingSource(action.modName, action.id));
        }
    if (denied) context["approval"] = false;
    if (failed) context["@dice_event_failed"] = true;
    return blocked;
}

void LuaPluginManager::startEventHooks() {
    std::lock_guard<ScriptMutex> guard(mutex_);
    if (hostStarted_) return;
    hostStarted_ = true;
    const uint64_t generation = runtimeGeneration_;
    auto startup = [generation](LuaPluginManager& owner) {
        if (generation != owner.runtimeGeneration_) return;
        json context = json::object(); owner.dispatchHook("StartUp", context);
    };
    if (!postCallback(0, startup)) startup(*this);
}

void LuaPluginManager::scheduleClockTick() {
    if (!scheduler_) return;
    bool needed = false;
    for (const auto& [id, event] : cycleEvents_)
        if (!event.clocks.empty() || std::find(event.hooks.begin(), event.hooks.end(), "daynew") != event.hooks.end()
            || std::find(event.hooks.begin(), event.hooks.end(), "dayend") != event.hooks.end()) { needed = true; break; }
    if (!needed) return;
    const uint64_t generation = runtimeGeneration_;
    const auto now = nowSeconds();
    const double delay = static_cast<double>(60 - (now % 60 + 60) % 60);
    postCallback(delay, [generation](LuaPluginManager& owner) { owner.runClockTick(generation); });
}

void LuaPluginManager::runClockTick(uint64_t generation) {
    std::lock_guard<ScriptMutex> guard(mutex_);
    if (!state_ || generation != runtimeGeneration_) return;
    const auto now = nowSeconds();
    const auto current = utils::timezoneTm(now), previous = utils::timezoneTm(lastClockTime_);
    const bool newMinute = now / 60 != lastClockTime_ / 60;
    const bool newDay = current.tm_year != previous.tm_year || current.tm_yday != previous.tm_yday;
    lastClockTime_ = now; // save before user code, never replay missed days or duplicate callbacks
    scheduleClockTick();
    if (newDay && hostStarted_) {
        json ended = {{"year", previous.tm_year + 1900}, {"month", previous.tm_mon + 1}, {"day", previous.tm_mday}};
        dispatchHook("DayEnd", ended);
        json started = {{"year", current.tm_year + 1900}, {"month", current.tm_mon + 1}, {"day", current.tm_mday}};
        dispatchHook("DayNew", started);
    }
    if (newMinute) for (const auto& [id, action] : cycleEvents_)
        if (std::find(action.clocks.begin(), action.clocks.end(), std::make_pair(current.tm_hour, current.tm_min)) != action.clocks.end()) {
            json context = {{"year", current.tm_year + 1900}, {"month", current.tm_mon + 1}, {"day", current.tm_mday},
                            {"hour", current.tm_hour}, {"minute", current.tm_min}};
            runLegacyEvent(action, context, true);
        }
}

// 单文件 Lua 插件：dofile 后读 msg_order[关键字]=函数名 → 前缀匹配调用该全局函数(msg)。
void LuaPluginManager::loadModFile(const fs::path& path, bool enabled) {
    LuaMod m;
    std::string fn = dnx_u8str(path.filename());
    if (!enabled && fn.size() > 9 && fn.substr(fn.size() - 9) == ".disabled") fn = fn.substr(0, fn.size() - 9);
    m.name = (fn.rfind('.') != std::string::npos) ? fn.substr(0, fn.rfind('.')) : fn;   // 去扩展名（fn 是 UTF-8，不回喂窄 path）
    m.title = m.name;
    const auto originalName = m.name;
    for (size_t suffix = 1; std::any_of(mods_.begin(), mods_.end(), [&](const auto& mod) { return mod.name == m.name; }); ++suffix)
        m.name = originalName + "@plugin" + (suffix == 1 ? std::string() : std::to_string(suffix));
    if (m.name != originalName) m.compatibilityWarnings.push_back("存在同名扩展，使用独立管理标识「" + m.name + "」，启停和删除仅作用于本文件。");
    m.dir = dnx_u8str(path.parent_path());
    m.singleFile = true;
    m.enabled = enabled;
    m.managedPaths.push_back(dnx_u8str(path));
    if (!enabled) { mods_.push_back(std::move(m)); return; }
    ScriptFaultScope fault(suspendedMods_, m.name);
    ProcessingScope logScope(ProcessingSource{"Lua 插件", m.title, dnx_u8str(path), "加载"}, "插件加载");

    LogOperation operation(DICE_LOG_SOURCE, "lua", "load_plugin", "file=" + dnx_u8str(path));
    loadingModDir_ = m.dir;
    const int stackBase = lua_gettop(state_);
    if (dnx_loadLegacyEnvironment(state_, path) != LUA_OK) {
        ++lastLoadErrors_;
        logPluginFailure("加载", dnx_normalizeLuaText(argStr(state_, -1)));
        operation.fail("file=" + dnx_u8str(path) + " script evaluation failed", true);
        lua_settop(state_, stackBase); loadingModDir_.clear(); mods_.push_back(std::move(m)); return;
    }
    const int env = lua_absindex(state_, -1);
    lua_getfield(state_, env, "msg_order");
    // 收集 {关键字, 函数名}，按关键字长度降序（长前缀优先，如「选择」先于「选」）。
    std::vector<std::pair<std::string, std::string>> orders;
    if (lua_istable(state_, -1)) {
        lua_pushnil(state_);
        while (lua_next(state_, -2)) {
            lua_pushvalue(state_, -2);
            std::string key = argStr(state_, -1); lua_pop(state_, 1);
            if (lua_isstring(state_, -1) && !key.empty()) orders.emplace_back(key, argStr(state_, -1));
            lua_pop(state_, 1);
        }
    }
    lua_pop(state_, 1);   // msg_order

    // 原版单文件插件的定时入口：task_call[任务名]="全局函数名"。
    lua_getfield(state_, env, "task_call");
    if (lua_istable(state_, -1)) {
        lua_pushnil(state_);
        while (lua_next(state_, -2)) {
            lua_pushvalue(state_, -2);
            const std::string taskName = dnx_normalizeLuaText(argStr(state_, -1));
            lua_pop(state_, 1);
            const std::string functionName = argStr(state_, -1);
            if (!taskName.empty() && !functionName.empty()) {
                lua_getfield(state_, env, functionName.c_str());
                if (lua_isfunction(state_, -1)) {
                    LegacyTask task;
                    task.name = taskName; task.modName = m.name; task.modDir = m.dir;
                    task.sourceFile = dnx_u8str(path); task.functionName = functionName;
                    taskCalls_[taskName] = std::move(task);
                }
                lua_pop(state_, 1);
            }
            lua_pop(state_, 1);
        }
    }
    lua_pop(state_, 1);   // task_call

    std::sort(orders.begin(), orders.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (auto& [key, func] : orders) {
        lua_getfield(state_, env, func.c_str());
        if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); continue; }
        lua_pop(state_, 1);
        ReplyRule rule;
        rule.name = m.name + ":" + key; rule.modName = m.name; rule.modDir = m.dir; rule.type = "order";
        rule.prefixPatterns.push_back(key);
        rule.legacySourceFile = dnx_u8str(path);
        rule.legacyFunctionName = func;
        replyRules_.push_back(std::move(rule));
        ++m.replies;
    }
    lua_settop(state_, stackBase);  // isolated discovery environment
    loadingModDir_.clear();
    operation.detail("file=" + dnx_u8str(path) + " replies=" + std::to_string(m.replies));
    mods_.push_back(std::move(m));
}

int LuaPluginManager::reload() {
    LogOperation operation(DICE_LOG_SOURCE, "lua", "reload");
    std::lock_guard<ScriptMutex> lk(mutex_);
    operation.phase("runtime_lock_acquired");
    freeRuntime();
    if (!init()) { operation.fail("runtime initialization failed"); return 0; }
    // 规则包热卸载时目录可能刚被移除，或其中有损坏的脚本。不能让一次插件扫描
    // 异常逃到 WebUI 请求线程，否则运行时已被销毁而骰娘会停在半重载状态。
    try {
        const int count = loadDirLocked(dir_);
        operation.detail("loaded=" + std::to_string(count) + " replies=" + std::to_string(replyRules_.size()));
        if (lastLoadErrors_) operation.partial(std::to_string(lastLoadErrors_) + " 项 Lua 加载失败，其余内容已载入");
        return count;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("[lua] reload failed; runtime remains available with no newly loaded mods: {}", e.what());
        operation.fail("plugin scan failed");
    } catch (...) {
        DICE_LOG_ERROR("[lua] reload failed with an unknown error; runtime remains available with no newly loaded mods");
        operation.fail("plugin scan failed");
    }
    return 0;
}

std::vector<LuaPluginManager::LuaMod> LuaPluginManager::mods() const {
    std::lock_guard<ScriptMutex> lk(mutex_);
    auto result = mods_;
    for (auto& mod : result) if (suspendedMods_.count(mod.name))
        mod.compatibilityWarnings.push_back("已暂停：插件执行超时。请修复插件后重载。");
    return result;
}

std::vector<std::string> LuaPluginManager::taskNames() const {
    std::lock_guard<ScriptMutex> lk(mutex_);
    std::vector<std::string> out;
    out.reserve(taskCalls_.size());
    for (const auto& [name, task] : taskCalls_) out.push_back(name);
    return out;
}

bool LuaPluginManager::hasTask(const std::string& name) const {
    std::lock_guard<ScriptMutex> lk(mutex_);
    return state_ && taskCalls_.find(name) != taskCalls_.end();
}

bool LuaPluginManager::runTask(const std::string& name, std::string* error) {
    std::lock_guard<ScriptMutex> lk(mutex_);
    const auto it = taskCalls_.find(name);
    if (!state_ || it == taskCalls_.end()) {
        if (error) *error = "Lua task not found: " + name;
        return false;
    }
    if (suspendedMods_.count(it->second.modName)) {
        if (error) *error = "此 Lua 插件已因执行超时暂停，请修复后重载";
        return false;
    }
    ScriptFaultScope fault(suspendedMods_, it->second.modName);
    const auto taskSource = loggingSource(it->second.modName, it->second.functionName);
    ProcessingScope logScope(taskSource, "Lua 定时任务「" + name + "」");
    const int base = lua_gettop(state_);
    const std::string previousDir = loadingModDir_;
    loadingModDir_ = it->second.modDir;
    const fs::path sourcePath(std::u8string(
        it->second.sourceFile.begin(), it->second.sourceFile.end()));
    int status = dnx_loadLegacyEnvironment(state_, sourcePath);
    if (status == LUA_OK) {
        const int env = lua_absindex(state_, -1);
        lua_getfield(state_, env, it->second.functionName.c_str());
        if (lua_isfunction(state_, -1)) {
            lua_remove(state_, env);  // function closure keeps its isolated _ENV alive
            status = dnx_pcall(state_, 0, LUA_MULTRET);
        } else {
            lua_pop(state_, 1);
            lua_remove(state_, env);
            lua_pushfstring(state_, "task function '%s' not found",
                            it->second.functionName.c_str());
            status = LUA_ERRRUN;
        }
    }
    loadingModDir_ = previousDir;
    if (status != LUA_OK) {
        const std::string message = dnx_normalizeLuaText(argStr(state_, -1));
        if (error) *error = message;
        logPluginFailure("运行定时任务「" + name + "」", message);
        lua_settop(state_, base);
        return false;
    }
    lua_settop(state_, base);
    logProcessingResult(taskSource, "定时任务执行完成", activeProcessingContext.trigger);
    return true;
}

// 插件管理 API 也必须只输出合法 UTF-8。旧 GBK 文本优先转码，失败才替换。
static std::string scrubUtf8(const std::string& s) { return dnx_normalizeLuaText(s); }

std::vector<LuaPluginManager::ModCommand> LuaPluginManager::commandsOf(const std::string& modName) const {
    std::lock_guard<ScriptMutex> lk(mutex_);
    std::vector<ModCommand> out;
    std::set<std::string> seen;
    auto push = [&](const std::string& raw, const char* kind) {
        if (raw.empty()) return;
        std::string t = scrubUtf8(formatTemplate(raw, {}));   // 解析 {自称} 等模板供展示
        // 解析后为空、或仍含未定义模板（{xxx}）→ 非可用触发词，跳过。
        if (t.empty() || (t.find('{') != std::string::npos && t.find('}') != std::string::npos)) return;
        if (!seen.insert(kind + ("|" + t)).second) return;
        out.push_back({t, kind});
    };
    for (const auto& r : replyRules_) {
        if (r.modName != modName) continue;
        for (const auto& p : r.prefixPatterns) push(p, "cmd");     // 前缀触发（含/不含前缀符号原样）
        for (const auto& p : r.matchPatterns)  push(p, "key");     // 精确关键词
        for (const auto& p : r.searchPatterns) push(p, "search");  // 包含匹配
    }
    return out;
}

std::vector<LuaPluginManager::HelpItem> LuaPluginManager::helpEntries() const {
    std::lock_guard<ScriptMutex> lk(mutex_);
    std::vector<HelpItem> out;
    for (const auto& [key, item] : legacyHelp_) {
        auto entry = item;
        std::set<std::string> visited;
        for (size_t depth = 0; !entry.text.empty() && entry.text[0] == '&'; ++depth) {
            const auto target = entry.text.substr(1);
            if (depth >= 24 || !visited.insert(legacy_mod::lower(target)).second) { entry.text.clear(); break; }
            const auto next = legacyHelp_.find(target);
            if (next == legacyHelp_.end()) break; // Context/speech alias: render only when selected.
            entry.text = next->second.text;
        }
        out.push_back(std::move(entry));
    }
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> manuals;
    for (const auto& [rule, book] : ruleBooks_) for (const auto& [topic, text] : book.manual) {
        out.push_back({rule + ":" + topic, text, "rule:" + rule});
        manuals[topic].emplace_back(rule, text);
    }
    for (const auto& [topic, values] : manuals) {
        std::string text;
        for (const auto& [rule, content] : values) {
            if (!text.empty()) text += "\n\n";
            text += values.size() > 1 ? rule + ":" + topic + "\n" + content : content;
        }
        out.push_back({topic, text, "rulebook"});
    }
    return out;
}

std::optional<std::string> LuaPluginManager::ruleManual(const std::string& topic, const std::string& activeRule) const {
    std::lock_guard<ScriptMutex> guard(mutex_);
    auto term = legacy_mod::lower(topic), rule = legacy_mod::lower(activeRule);
    const auto colon = term.find(':');
    if (colon != std::string::npos) { rule = term.substr(0, colon); term.erase(0, colon + 1); }
    auto own = [&](const std::string& name) -> std::optional<std::string> {
        if (const auto book = ruleBooks_.find(name); book != ruleBooks_.end())
            if (const auto item = book->second.manual.find(term); item != book->second.manual.end()) return item->second;
        return std::nullopt;
    };
    if (!rule.empty()) {
        if (auto value = own(rule)) return value;
        for (const auto& [name, book] : ruleBooks_) if (name.rfind(rule + "-", 0) == 0)
            if (auto value = own(name)) return value;
        for (auto hyphen = rule.rfind('-'); hyphen != std::string::npos; hyphen = rule.rfind('-')) {
            rule.resize(hyphen); if (auto value = own(rule)) return value;
        }
        return std::nullopt;
    }
    std::string result; size_t count = 0;
    for (const auto& [name, book] : ruleBooks_) if (auto value = own(name)) {
        if (!result.empty()) result += "\n\n";
        result += name + ":" + term + "\n" + *value; ++count;
    }
    if (!count) return std::nullopt;
    if (count == 1) for (const auto& [name, book] : ruleBooks_) if (auto value = own(name)) return value;
    return result;
}

LuaPluginManager::DispatchResult LuaPluginManager::dispatchTape(const std::string& rule, const std::string& tape, json& context) {
    std::lock_guard<ScriptMutex> guard(mutex_);
    DispatchResult result;
    if (const auto book = ruleBooks_.find(legacy_mod::lower(rule)); book != ruleBooks_.end())
        if (const auto action = book->second.tapes.find(legacy_mod::lower(tape)); action != book->second.tapes.end()) {
            result.matched = runLegacyEvent(action->second, context, false);
            result.failed = context.value("@dice_event_failed", json(false)) == true;
            result.matched = result.matched || result.failed;
            if (result.matched && !result.failed) result.reply = dnx_contextString(context, "msg_reply");
            result.sources = {loggingSource(action->second.modName, action->second.id)};
        }
    return result;
}

bool LuaPluginManager::setModEnabled(const std::string& name, bool enabled) {
    std::lock_guard<ScriptMutex> guard(mutex_);
    const auto found = std::find_if(mods_.begin(), mods_.end(), [&](const auto& mod) { return mod.name == name; });
    if (found == mods_.end() || found->managedPaths.empty() || (enabled && found->ancestorDisabled)) return false;
    auto allowed = [&](const fs::path& path) {
        if (dnx_safeUnder(path, dnx_path(dir_)) || dnx_safeUnder(path, dnx_path(dir_).parent_path() / "plugin")) return true;
        for (const auto& extra : extraDirs_) if (dnx_safeUnder(path, dnx_path(extra))) return true;
        return false;
    };
    std::vector<std::pair<fs::path, fs::path>> operations;
    std::error_code ec;
    for (const auto& raw : found->managedPaths) {
        const auto source = dnx_path(raw);
        const bool off = raw.ends_with(".disabled");
        if (!allowed(source) || !fs::exists(source, ec)) return false;
        if (enabled == !off) continue;
        const auto target = dnx_path(enabled ? raw.substr(0, raw.size() - 9) : raw + ".disabled");
        if (!allowed(target) || fs::exists(target, ec)) return false;
        operations.emplace_back(source, target);
    }
    size_t done = 0;
    for (; done < operations.size(); ++done) {
        fs::rename(operations[done].first, operations[done].second, ec);
        if (ec) {
            for (size_t rollback = done; rollback > 0; --rollback) {
                std::error_code undo;
                fs::rename(operations[rollback - 1].second, operations[rollback - 1].first, undo);
                if (undo) DICE_LOG_ERROR("模组「{}」启停失败，回滚路径「{}」失败：{}", name, dnx_u8str(operations[rollback - 1].second), undo.message());
            }
            return false;
        }
    }
    return true;
}

bool LuaPluginManager::deleteMod(const std::string& name) {
    std::lock_guard<ScriptMutex> guard(mutex_);
    const auto found = std::find_if(mods_.begin(), mods_.end(), [&](const auto& mod) { return mod.name == name; });
    if (found == mods_.end() || found->managedPaths.empty()) return false;
    std::vector<fs::path> targets;
    for (const auto& raw : found->managedPaths) {
        const auto path = dnx_path(raw);
        bool allowed = dnx_safeUnder(path, dnx_path(dir_)) || dnx_safeUnder(path, dnx_path(dir_).parent_path() / "plugin");
        for (const auto& extra : extraDirs_) allowed = allowed || dnx_safeUnder(path, dnx_path(extra));
        if (!allowed) return false;
        targets.push_back(path);
    }
    bool removed = false;
    for (const auto& path : targets) {
        std::error_code ec;
        removed = fs::remove_all(path, ec) > 0 || removed;
        if (ec) { DICE_LOG_ERROR("模组「{}」删除路径「{}」失败：{}", name, dnx_u8str(path), ec.message()); return false; }
    }
    return removed;
}

std::filesystem::path LuaPluginManager::resolveScript(const std::string& name, const std::string& ownerDir) const {
    std::lock_guard<ScriptMutex> guard(mutex_);
    if (name.empty() || name.size() > 1024 || name.find("..") != std::string::npos || name.find(':') != std::string::npos) return {};
    auto relative = name;
    if (legacy_mod::lower(relative).ends_with(".lua")) relative.resize(relative.size() - 4);
    for (auto& c : relative) if (c == '/' || c == '\\') c = '.';
    if (const auto entry = scriptRegistry_.find(relative); entry != scriptRegistry_.end()) {
        const auto mod = std::find_if(mods_.begin(), mods_.end(), [&](const auto& item) { return item.name == entry->second.modName; });
        if (mod != mods_.end() && mod->enabled && !suspendedMods_.count(mod->name)
            && dnx_safeUnder(entry->second.path, dnx_path(mod->dir))) return entry->second.path;
    }
    for (auto& c : relative) if (c == '.') c = '/';
    const auto file = dnx_path(relative + ".lua");
    if (file.is_absolute() || file.has_root_name()) return {};
    const auto root = dnx_path(ownerDir);
    for (const auto& candidate : {root / "script" / file, root / file}) {
        std::error_code ec;
        if (dnx_safeUnder(candidate, root) && fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

bool LuaPluginManager::importUpload(const std::string& filename, const std::string& bytes,
                                    std::string* err, std::vector<std::string>* imported,
                                    bool dryRun, std::vector<std::string>* permissions,
                                    std::vector<std::string>* risks) {
    std::lock_guard<ScriptMutex> lk(mutex_);
    auto fail = [&](const std::string& e) { if (err) *err = e; return false; };
    if (bytes.empty()) return fail("empty/invalid file");
    auto u8 = [](const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); };
    std::string base = filename;
    if (auto p = base.find_last_of("/\\"); p != std::string::npos) base = base.substr(p + 1);   // 防穿越
    if (base.empty()) return fail("bad filename");
    auto lower = [](std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; };
    auto endsWith = [](const std::string& s, const std::string& sfx) {
        return s.size() > sfx.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
    };
    auto writeFile = [](const fs::path& p, const std::string& b) {
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        f.write(b.data(), (std::streamsize)b.size());
        return (bool)f;
    };
    const fs::path modRoot = dir_.empty() ? fs::path("data/mod") : dnx_path(dir_);
    const fs::path pluginRoot = modRoot.parent_path() / "plugin";
    std::error_code ec;
    fs::create_directories(modRoot, ec); ec.clear();
    const std::string lb = lower(base);

    // 单文件 .lua → data/plugin（msg_order 插件，原版目录语义）。
    if (endsWith(lb, ".lua")) {
        if (dryRun) {
            if (permissions && permissions->empty()) *permissions = {"all"};   // 旧插件默认请求全部
            if (risks) *risks = scanLuaRisksText(bytes);
            if (imported) imported->push_back(base.substr(0, base.size() - 4));
            return true;
        }
        fs::create_directories(pluginRoot, ec); ec.clear();
        if (!writeFile(pluginRoot / u8(base), bytes)) return fail("write failed: " + base);
        if (imported) imported->push_back(base.substr(0, base.size() - 4));
        return true;
    }
    // 单文件 .json → data/mod（描述档/查询类 mod，原版正式登记形态）。
    if (endsWith(lb, ".json")) {
        if (dryRun) {
            if (permissions) {
                try {
                    auto jj = nlohmann::json::parse(bytes);
                    if (jj.contains("permissions") && jj["permissions"].is_array())
                        for (auto& pp : jj["permissions"]) if (pp.is_string()) permissions->push_back(pp.get<std::string>());
                } catch (...) {}
                if (permissions->empty()) *permissions = {"all"};
            }
            if (imported) imported->push_back(base.substr(0, base.size() - 5));
            return true;
        }
        if (!writeFile(modRoot / u8(base), bytes)) return fail("write failed: " + base);
        if (imported) imported->push_back(base.substr(0, base.size() - 5));
        return true;
    }

    // zip：落盘 → 解压到临时目录 → 归位。Win10+ 自带 tar(bsdtar 可解 zip)；POSIX 用 unzip。
    const std::string tag = std::to_string((long long)std::time(nullptr));
    fs::path zipPath = modRoot / ("_imp_" + tag + ".zip");
    fs::path tmpDir  = modRoot / ("_imp_" + tag);
    if (!writeFile(zipPath, bytes)) return fail("write failed: " + zipPath.string());
    fs::create_directories(tmpDir, ec); ec.clear();
#if defined(_WIN32)
    dice::proc::runPaths(dice::proc::systemTool("tar.exe"), {"-xf", zipPath, "-C", tmpDir});
#else
    dice::proc::runPaths("unzip", {"-o", "-q", zipPath, "-d", tmpDir});
#endif
    if (dryRun) {
        // 只解析：权限声明 + 风险扫描，不落盘。
        if (permissions) {
            for (auto& e : fs::recursive_directory_iterator(tmpDir, ec)) {
                if (ec) break;
                if (!e.is_regular_file(ec)) continue;
                const std::string fn = dnx_u8str(e.path().filename());
                if (fn == "descriptor.json" || endsWith(lower(fn), ".json")) {
                    LuaMod tmp; tmp.name = "?";
                    parseModDescriptor(e.path(), tmp);
                    if (tmp.permissionDeclared) { *permissions = tmp.permissions; break; }
                }
            }
            if (permissions->empty()) *permissions = {"all"};
        }
        if (risks) *risks = scanLuaRisks(tmpDir);
        fs::remove_all(tmpDir, ec);
        if (imported) imported->push_back(base);
        return true;
    }
    fs::remove(zipPath, ec); ec.clear();

    auto safeEntries = [](const fs::path& d) {
        std::vector<fs::path> out; std::error_code e2;
        for (fs::directory_iterator it(d, e2), end; it != end; it.increment(e2)) {
            if (e2) { e2.clear(); continue; }
            out.push_back(it->path());
        }
        return out;
    };
    auto isDir = [](const fs::path& p) { std::error_code e2; return fs::is_directory(p, e2); };
    auto isModDir = [&isDir](const fs::path& p) {
        std::error_code e2;
        if (fs::is_regular_file(p / "descriptor.json", e2)) return true;
        for (const char* sub : {"reply", "script", "speech", "event", "model", "rulebook"})
            if (fs::is_directory(p / sub, e2)) return true;
        return false;
    };
    std::set<std::string> seen;
    auto record = [&](const std::string& name) {
        if (imported && seen.insert(name).second) imported->push_back(name);
    };
    auto placeDir = [&](const fs::path& src, const std::string& name) {
        fs::path dest = modRoot / u8(name);
        std::error_code e2;
        fs::remove_all(dest, e2); e2.clear();
        fs::rename(src, dest, e2);
        if (e2) { e2.clear(); fs::copy(src, dest, fs::copy_options::recursive, e2); }
        record(name);
    };
    auto placeFile = [&](const fs::path& src, const fs::path& dest) {
        std::error_code e2;
        fs::remove(dest, e2); e2.clear();
        fs::rename(src, dest, e2);
        if (e2) { e2.clear(); fs::copy_file(src, dest, fs::copy_options::overwrite_existing, e2); }
    };
    // 把一个目录当作「mod 库根」归位其顶层内容：<名>.json / mod 目录（或与 json 同名的目录）/ .lua。
    auto distribute = [&](const fs::path& root) {
        int moved = 0;
        auto entries = safeEntries(root);
        std::set<std::string> jsonStems;
        for (auto& p : entries) {
            if (isDir(p)) continue;
            std::string fn = dnx_u8str(p.filename());
            if (endsWith(lower(fn), ".json")) jsonStems.insert(fn.substr(0, fn.size() - 5));
        }
        for (auto& p : entries) {
            if (isDir(p)) continue;
            std::string fn = dnx_u8str(p.filename());
            std::string lf = lower(fn);
            if (endsWith(lf, ".json")) {
                placeFile(p, modRoot / u8(fn));
                record(fn.substr(0, fn.size() - 5)); ++moved;
            } else if (endsWith(lf, ".lua")) {
                std::error_code e2; fs::create_directories(pluginRoot, e2);
                placeFile(p, pluginRoot / u8(fn));
                record(fn.substr(0, fn.size() - 4)); ++moved;
            }
        }
        for (auto& p : entries) {
            if (!isDir(p)) continue;
            std::string dn = dnx_u8str(p.filename());
            if (isModDir(p) || jsonStems.count(dn)) { placeDir(p, dn); ++moved; }
        }
        return moved;
    };

    int moved = 0;
    if (isModDir(tmpDir)) {
        // zip 内容本身就是一个 mod（无包装目录）→ 以 zip 文件名为 mod 名。
        std::string modName = base;
        if (auto dot = modName.rfind('.'); dot != std::string::npos) modName = modName.substr(0, dot);
        placeDir(tmpDir, modName); moved = 1;
    } else {
        moved = distribute(tmpDir);
        if (!moved) {   // 单层包装目录（用户把 mod 库整个打包的常见形态）→ 下钻一层再归位
            auto entries = safeEntries(tmpDir);
            if (entries.size() == 1 && isDir(entries[0])) moved = distribute(entries[0]);
        }
    }
    fs::remove_all(tmpDir, ec); ec.clear();
    if (!moved)
        return fail("zip 内未找到可识别的 mod（需含 <名>.json 描述档，或 descriptor.json/reply/script/speech/model/rulebook）");
    return true;
}

// ─── 阶段二：模板格式化 + reply 派发 ─────────────────────────────
std::vector<std::string> LuaPluginManager::directoryTriggers(const LuaEventOrigin& origin, bool orders) const {
    std::vector<std::string> result;
    const auto snapshot = std::atomic_load(&directoryRules_);
    const bool privateMessage = origin.groupId.empty();
    const int trust = trustLookup_ ? trustLookup_(origin.userId, origin.groupId, origin.platform, origin.adapterId, privateMessage) : 0;
    if (trust < 0) return result;
    for (const auto& rule : *snapshot) {
        if (rule.type == "nor") continue;
        if (!privateMessage && groupGate_ && !groupGate_(origin.platform, origin.groupId, "lua:" + rule.modName, origin.adapterId)) continue;
        if (!privateMessage && replyTypeGate_ && !replyTypeGate_(origin.platform, origin.groupId, rule.type, origin.adapterId)) continue;
        if ((rule.requiresGame || !rule.requiredRule.empty()) && (!ruleGate_ || !ruleGate_(origin.platform, origin.groupId, origin.userId, rule.requiredRule, origin.adapterId))) continue;
        if (rule.limits) {
            const auto& limit = *rule.limits;
            if (!legacy_mod::idMatches(limit.users, origin.userId) || !legacy_mod::idMatches(limit.groups, origin.groupId)) continue;
            const auto self = botIdFor(origin.platform, origin.adapterId);
            if (!limit.dicePolicy.empty() && limit.dicePolicy != "ignore" && (self.empty() || origin.userId != self)) {
                const bool bot = diceLookup_ && diceLookup_(origin.userId, origin.platform, origin.adapterId);
                if (bot != (limit.dicePolicy == "only")) continue;
            }
            const auto variables = [&](const auto& conditions, const std::string& scope, bool user, bool selfOnly) {
                for (const auto& [key, condition] : conditions) {
                    // &speech keys and SelfData-file fallbacks need the runtime
                    // or filesystem. Do not guess them while browsing metadata.
                    if (key.empty() || key.front() == '&') return false;
                    const bool exists = confHas(scope,key);
                    if (selfOnly && (self.empty() || !exists)) return false;
                    const auto actual = user && key == "trust" ? json(trust)
                        : legacy_mod::storedValue(confGet(scope,key),exists);
                    if (!legacy_mod::matches(actual,condition)) return false;
                }
                return true;
            };
            if (!variables(limit.userVariables,"u:" + origin.userId,true,false)
                || (!privateMessage && !variables(limit.groupVariables,"g:" + origin.groupId,false,false))
                || !variables(limit.selfVariables,"u:" + self,false,true)) continue;
        }
        const auto& items = orders ? rule.prefixes : rule.matches;
        result.insert(result.end(), items.begin(), items.end());
        if (!orders) result.insert(result.end(), rule.searches.begin(), rule.searches.end());
    }
    return result;
}

// 解析一个 {key}：msg 变量 > speech 词条(别名 &x / 递归展开 {…}) > 内置全局。
std::string LuaPluginManager::valueOf(const std::string& key,
                                      const std::map<std::string, std::string>& vars, int depth,
                                      bool helpScope, TemplateBudget& budget) const {
    if (depth > 24 || budget.exhausted) return "";
    if (budget.steps == 0) { budget.exhausted = true; return ""; }
    --budget.steps;
    if (auto it = vars.find(key); it != vars.end()) return safe_template::hold(it->second, budget.eval);
    // Keep Next's exact variable precedence, then provide the legacy dict_ci
    // lookup without changing user-authored field names or UTF-8 bytes.
    const auto foldedKey = legacy_mod::lower(key);
    for (const auto& [name, value] : vars) if (legacy_mod::lower(name) == foldedKey) return safe_template::hold(value, budget.eval);
    if (helpScope) if (const auto help = legacyHelp_.find(key); help != legacyHelp_.end())
        return formatHelpTemplateImpl(help->second.text, vars, depth + 1, budget);
    if (auto it = speech_.find(foldedKey); it != speech_.end()) {
        if (it->second.empty()) return "";
        static thread_local std::mt19937_64 random{std::random_device{}()};
        const auto& choices = it->second;
        const std::string& v = choices.size() == 1 ? choices.front()
            : choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(random)];
        if (!v.empty() && v[0] == '&') return valueOf(v.substr(1), vars, depth + 1, helpScope, budget);
        return formatTemplateImpl(v, vars, depth + 1, helpScope, budget);
    }
    if (foldedKey.rfind("help:", 0) == 0) {
        const std::string topic = key.substr(5);
        if (const auto help = legacyHelp_.find(topic); help != legacyHelp_.end())
            return formatHelpTemplateImpl(help->second.text, vars, depth + 1, budget);
        std::string active;
        if (const auto group = vars.find("gid"); group != vars.end() && !group->second.empty()) {
            auto scope = "game:" + group->second;
            if (const auto session = confGet(scope, "__session"); !session.empty()) scope = "game:session:" + session;
            active = confGet(scope, "rule");
        }
        if (const auto manual = ruleManual(topic, active)) return formatHelpTemplateImpl(*manual, vars, depth + 1, budget);
        if (helpLookup_) {
            if (auto value = helpLookup_(topic)) return formatHelpTemplateImpl(*value, vars, depth + 1, budget);
        }
        return "";
    }
    if (message_break::isFormFeed(foldedKey)) return "\f";
    if (foldedKey == "self" || foldedKey == "strselfname" || foldedKey == "strselfnick" || foldedKey == "name") return safe_template::hold(selfName_, budget.eval);
    return "";   // 未知 → 空
}

static std::optional<std::string> dnx_currentUserDisplay(const std::map<std::string,std::string>& vars, const std::string& key) {
    const auto uid=vars.find("uid"), nick=vars.find("nick");
    if ((key=="print.uid" || (uid!=vars.end() && key=="print.uid="+uid->second))
        && uid!=vars.end() && nick!=vars.end() && !nick->second.empty())
        return session_deck::memberLabel(nick->second)+"("+uid->second+")";
    return std::nullopt;
}

std::string LuaPluginManager::formatTemplate(const std::string& text,
                                             const std::map<std::string, std::string>& vars, int depth,
                                             safe_template::Read context, safe_template::Length length,
                                             safe_template::Delay* delay) const {
    std::lock_guard<ScriptMutex> lock(mutex_);
    TemplateBudget budget;
    budget.eval.delay = delay ? delay : safe_template::contextDelay;
    budget.eval.contextLengthsOverrideArgs = static_cast<bool>(length);
    budget.eval.length = [&, length](const std::string& key) -> std::optional<size_t> {
        if (length) if (auto value = length(key)) return value;
        return templateLength_ ? templateLength_(key, invocationOrigin()) : std::nullopt;
    };
    budget.eval.read = [&, context](const std::string& key) -> std::optional<std::string> {
        if (context) if (auto value = context(key)) return value;
        if (const auto display=dnx_currentUserDisplay(vars,key)) return display;
        if (key == "selfId") return botIdFor(activePlatform_, activeAdapter_);
        if (templateLookup_) return templateLookup_(key, invocationOrigin());
        return std::nullopt;
    };
    const auto expanded = formatTemplateImpl(text, vars, depth, false, budget);
    return budget.exhausted ? "" : safe_template::materialize(expanded, {}, budget.eval);
}

std::string LuaPluginManager::formatHelpTemplate(const std::string& text,
                                                const std::map<std::string, std::string>& vars) const {
    std::lock_guard<ScriptMutex> lock(mutex_);
    TemplateBudget budget;
    budget.eval.delay = safe_template::contextDelay;
    budget.eval.length = [&](const std::string& key) -> std::optional<size_t> {
        return templateLength_ ? templateLength_(key, invocationOrigin()) : std::nullopt;
    };
    budget.eval.read = [&](const std::string& key) -> std::optional<std::string> {
        if (const auto display=dnx_currentUserDisplay(vars,key)) return display;
        return templateLookup_ ? templateLookup_(key, invocationOrigin()) : std::nullopt;
    };
    const auto expanded = formatHelpTemplateImpl(text, vars, 0, budget);
    return budget.exhausted ? "" : safe_template::materialize(expanded, {}, budget.eval);
}

std::string LuaPluginManager::formatHelpTopic(const std::string& topic,
                                           const std::map<std::string, std::string>& vars,
                                           const std::optional<LuaEventOrigin>& origin) const {
    std::lock_guard<ScriptMutex> lock(mutex_);
    TemplateBudget budget;
    budget.eval.delay = safe_template::contextDelay;
    budget.eval.length = [&](const std::string& key) -> std::optional<size_t> {
        return templateLength_ ? templateLength_(key, origin.value_or(invocationOrigin())) : std::nullopt;
    };
    budget.eval.read = [&](const std::string& key) -> std::optional<std::string> {
        if (const auto display=dnx_currentUserDisplay(vars,key)) return display;
        return templateLookup_ ? templateLookup_(key, origin.value_or(invocationOrigin())) : std::nullopt;
    };
    const auto expanded = valueOf("help:" + topic, vars, 0, false, budget);
    return budget.exhausted ? "" : safe_template::materialize(expanded, {}, budget.eval);
}

std::string LuaPluginManager::formatHelpTemplateImpl(const std::string& text,
        const std::map<std::string, std::string>& vars, int depth, TemplateBudget& budget) const {
    if (depth > 24 || budget.exhausted) return "";
    if (!text.empty() && text[0] == '&') return valueOf(text.substr(1), vars, depth + 1, true, budget);
    return formatTemplateImpl(text, vars, depth, true, budget);
}

std::string LuaPluginManager::formatTemplateImpl(const std::string& text,
        const std::map<std::string, std::string>& vars, int depth, bool helpScope, TemplateBudget& budget) const {
    if (depth > 24 || budget.exhausted) return "";
    constexpr size_t maxOutput = 4 * 1024 * 1024;
    if (text.size() > maxOutput) { budget.exhausted = true; return ""; }
    auto program = safe_template::compile(text);
    // Lua's case-insensitive context/speech precedence also applies to this
    // legacy builtin, unlike native authored control nodes.
    const auto restoreLegacyControl = [&](auto&& self, std::vector<safe_template::Node>& nodes) -> void {
        for (auto& node : nodes) {
            if (node.kind == safe_template::Kind::formFeed) node.kind = safe_template::Kind::variable;
            for (auto& branch : node.branches) self(self, branch);
        }
    };
    restoreLegacyControl(restoreLegacyControl, program.nodes);
    const safe_template::Resolve named = [&](const std::string& key, unsigned nested, safe_template::Budget&) -> std::optional<std::string> {
        return valueOf(key, vars, nested, helpScope, budget);
    };
    const auto out = safe_template::expand(program.nodes, vars, named, depth, budget.eval, {}, named);
    if (!budget.eval.nodes || !budget.eval.bytes || out.size() > maxOutput) budget.exhausted = true;
    return budget.exhausted ? "" : out;
}

std::string LuaPluginManager::resolveAmp(const std::string& key) const {
    std::lock_guard<ScriptMutex> lk(mutex_);
    TemplateBudget budget;
    const auto expanded = valueOf(key, {}, 0, false, budget);
    return budget.exhausted ? "" : safe_template::materialize(expanded, {}, budget.eval);
}

double LuaPluginManager::yieldedDelaySeconds(lua_State* coroutine, int resultCount) {
    double delay = 0.0;
    if (resultCount > 0 && lua_isnumber(coroutine, -1)) delay = lua_tonumber(coroutine, -1);
    lua_settop(coroutine, 0);   // 移除 yield 给宿主的值，保留 Lua 内部调用帧
    if (delay < 0.0) delay = 0.0;
    if (delay > 7.0 * 24.0 * 3600.0) delay = 7.0 * 24.0 * 3600.0;
    return delay;
}

void LuaPluginManager::scheduleCoroutineResume(int threadRef, double delaySeconds) {
    const uint64_t generation = runtimeGeneration_;
    if (!postCallback(delaySeconds, [threadRef, generation](LuaPluginManager& owner) {
        if (generation == owner.runtimeGeneration_) owner.resumeCoroutine(threadRef);
    })) {
        const auto pending = pendingCoroutines_.find(threadRef);
        if (pending != pendingCoroutines_.end()) {
            releaseLegacyLocks(pending->second.locks);
            luaL_unref(state_, LUA_REGISTRYINDEX, pending->second.messageRef);
            luaL_unref(state_, LUA_REGISTRYINDEX, threadRef);
            pendingCoroutines_.erase(pending);
        }
    }
}

bool LuaPluginManager::postCallback(double delay, std::function<void(LuaPluginManager&)> callback) {
    if (!scheduler_) return false;
    const std::weak_ptr<CallbackState> lifetime = callbackState_;
    try {
        scheduler_(delay, [lifetime, callback = std::move(callback)]() {
            const auto state = lifetime.lock();
            if (!state) return;
            std::lock_guard<std::recursive_mutex> guard(state->mutex);
            if (state->owner) {
                try { callback(*state->owner); }
                catch (const ScriptBusy& busy) {   // 回调尚未开始执行：交给插件调度器稍后重试
                    if (!busy.nested()) throw;
                    DICE_LOG_ERROR("Lua 异步任务发生异常：{}", busy.what());
                }
                catch (const std::exception& ex) { DICE_LOG_ERROR("Lua 异步任务发生异常：{}", ex.what()); }
                catch (...) { DICE_LOG_ERROR("Lua 异步任务发生未知异常。"); }
            }
        });
        return true;
    } catch (const std::exception& ex) { DICE_LOG_ERROR("Lua 任务调度失败：{}", ex.what()); }
    catch (...) { DICE_LOG_ERROR("Lua 任务调度失败。 "); }
    return false;
}

bool LuaPluginManager::matchesLimits(const ReplyRule& rule, const std::string& uid, const std::string& gid,
                                     bool isPrivate, int trust, const std::string& platform, const std::string& adapterId) const {
    if (rule.type == "nor" || (rule.type != "order" && rule.type != "reply" && rule.type != "both" && rule.type != "game")) return false;
    if (!isPrivate && replyTypeGate_ && !replyTypeGate_(platform, gid, rule.type, adapterId)) return false;
    if (!rule.limits) return true; // single-file msg_order has no declarative limit
    const auto& limit = *rule.limits;
    const auto selfId = botIdFor(platform, adapterId);
    if (!legacy_mod::idMatches(limit.users, uid) || !legacy_mod::idMatches(limit.groups, isPrivate ? "0" : gid)) return false;
    if (!limit.dicePolicy.empty() && limit.dicePolicy != "ignore" && (selfId.empty() || uid != selfId)) {
        const bool bot = diceLookup_ && diceLookup_(uid, platform, adapterId);
        if (bot != (limit.dicePolicy == "only")) return false;
    }
    auto variablesMatch = [&](const auto& conditions, const std::string& scope, bool user) {
        for (const auto& [rawKey, condition] : conditions) {
            const auto key = !rawKey.empty() && rawKey.front() == '&' ? resolveAmp(rawKey.substr(1)) : rawKey;
            const auto actual = user && key == "trust" ? json(trust)
                : legacy_mod::storedValue(confGet(scope, key), confHas(scope, key));
            if (!legacy_mod::matches(actual, condition)) return false;
        }
        return true;
    };
    if (!variablesMatch(limit.userVariables, "u:" + uid, true)) return false;
    // Original group variables do not apply to private messages.
    if (!isPrivate && !gid.empty() && !variablesMatch(limit.groupVariables, "g:" + gid, false)) return false;
    for (const auto& [rawKey, condition] : limit.selfVariables) {
        const auto key = !rawKey.empty() && rawKey.front() == '&' ? resolveAmp(rawKey.substr(1)) : rawKey;
        json actual = selfId.empty() ? json() : legacy_mod::storedValue(confGet("u:" + selfId, key), confHas("u:" + selfId, key));
        if (actual.is_null()) {
            // SelfData uses a filename stem followed by dotted JSON fields. Never read host config/secrets.
            auto dot = key.rfind('.');
            while (dot != std::string::npos) {
                const auto stem = key.substr(0, dot);
                if (stem.find_first_of("/\\:") != std::string::npos || stem == "." || stem == "..") break;
                const auto path = selfDataPath(stem);
                std::error_code ec;
                const auto size = fs::file_size(path, ec);
                if (!ec && size <= 4 * 1024 * 1024 && dnx_safeUnder(path, fs::path("data/self_data"))) {
                    std::ifstream input(path, std::ios::binary);
                    json value = json::parse(input, nullptr, false);
                    if (!value.is_discarded()) {
                        for (const auto& field : legacy_mod::split(key.substr(dot + 1), '.')) {
                            if (!value.is_object() || !value.contains(field)) { value = nullptr; break; }
                            value = json(value.at(field));
                        }
                        actual = std::move(value);
                        break;
                    }
                }
                if (dot == 0) break;
                dot = key.rfind('.', dot - 1);
            }
        }
        if (selfId.empty() && actual.is_null()) return false;
        if (!legacy_mod::matches(actual, condition)) return false;
    }
    return true;
}

std::string LuaPluginManager::limitScopeKey(int scope, const std::string& uid, const std::string& gid,
                                           bool isPrivate, const std::string& platform, const std::string& adapterId) const {
    const auto kind = static_cast<legacy_mod::LimitScope>(scope);
    if (kind == legacy_mod::LimitScope::Global) return "global";
    if (kind == legacy_mod::LimitScope::User) return json::array({"user", platform, uid}).dump();
    return json::array({isPrivate ? "private" : "group", platform, adapterId, isPrivate ? uid : gid}).dump();
}

bool LuaPluginManager::consumeLimits(const ReplyRule& rule, const std::string& uid, const std::string& gid,
                                     bool isPrivate, const std::string& platform, const std::string& adapterId,
                                     std::string& notice, std::vector<std::string>& locks, bool& busy) {
    if (!rule.limits) return true;
    const auto& limit = *rule.limits;
    auto keyFor = [&](const legacy_mod::CounterLimit& counter) {
        return json::array({limitScopeKey(static_cast<int>(counter.scope), uid, gid, isPrivate, platform, adapterId),
                            counter.name.empty() ? rule.name : counter.name}).dump();
    };
    for (const auto& counter : limit.locks) {
        const auto key = keyFor(counter);
        if (activeLegacyLocks_.count(key)) busy = true;
        locks.push_back(key);
    }
    if (busy) return false;
    const auto now = nowSeconds();
    const auto day = utils::formatTimeInTimezone(now, "%Y%m%d");
    std::vector<std::tuple<std::string, std::string, std::string>> writes;
    auto number = [&](const std::string& scope, const std::string& key) -> long long {
        try { return std::stoll(confGet(scope, key)); } catch (...) { return 0; }
    };
    for (const auto& counter : limit.cooldowns) {
        if (counter.amount <= 0) continue;
        const auto key = keyFor(counter);
        const auto until = number("legacy:cd", key);
        if (until > now) { notice = limit.cooldownNotice; return false; }
        writes.emplace_back("legacy:cd", key, std::to_string(now + counter.amount));
    }
    for (const auto& counter : limit.daily) {
        const auto key = keyFor(counter);
        const auto count = number("legacy:today:" + day, key);
        if (count >= counter.amount) { notice = limit.dailyNotice; return false; }
        writes.emplace_back("legacy:today:" + day, key, std::to_string(count + 1));
    }
    if (!writes.empty() && !confSetBatch(writes)) {
        DICE_LOG_ERROR("模组「{}」回复「{}」的限额写入失败，已拒绝执行。", rule.modName, rule.name);
        return false;
    }
    for (const auto& key : locks) activeLegacyLocks_.insert(key);
    return true;
}

static thread_local int dnx_lockReplayDepth = 0;
static thread_local std::string dnx_queuedRuleKey;

void LuaPluginManager::releaseLegacyLocks(const std::vector<std::string>& locks) {
    for (const auto& key : locks) activeLegacyLocks_.erase(key);
    if (queuedDispatches_.empty() || lockDrainScheduled_) return;
    lockDrainScheduled_ = true;
    const uint64_t generation = runtimeGeneration_;
    if (!postCallback(0.001, [generation](LuaPluginManager& owner) { owner.drainLegacyLocks(generation); }))
        lockDrainScheduled_ = false;
}

void LuaPluginManager::drainLegacyLocks(uint64_t generation) {
    std::unique_lock<ScriptMutex> guard(mutex_);
    if (!state_ || generation != runtimeGeneration_) return;
    lockDrainScheduled_ = false;
    for (size_t i = 0, count = queuedDispatches_.size(); i < count; ++i) {
        auto task = std::move(queuedDispatches_.front()); queuedDispatches_.pop_front();
        if (task.generation != generation) continue;
        bool blocked = false;
        for (const auto& key : task.locks) if (activeLegacyLocks_.count(key)) { blocked = true; break; }
        if (blocked) { queuedDispatches_.push_back(std::move(task)); continue; }
        ProcessingScope processing(std::move(task.context));
        DispatchResult result;
        {
            struct ReplayScope {
                int depth; std::string key;
                ~ReplayScope() { dnx_lockReplayDepth = depth; dnx_queuedRuleKey = std::move(key); }
            } replay{std::exchange(dnx_lockReplayDepth, task.depth), std::exchange(dnx_queuedRuleKey, task.ruleKey)};
            const int trust = trustLookup_ ? trustLookup_(task.uid, task.gid, task.platform, task.adapterId, task.isPrivate) : task.trust;
            if (trust < 0) continue;
            VirtualEventScope virtualScope(*this, task.virtualEvent);
            result = dispatch(task.text, task.uid, task.gid, task.nick, task.card, task.isPrivate, trust, task.platform, task.adapterId);
        }
        const auto send = asyncReply_;
        const auto scopedSend = scopedAsyncReply_;
        // Do not invoke host transport while holding the runtime lock.
        if (!result.reply.empty() && (send || scopedSend)) {
            guard.unlock();
            try { if (scopedSend) scopedSend(task.platform, task.adapterId, result.reply, task.gid, task.uid); else send(task.platform, task.gid, task.uid, result.reply); }
            catch (const std::exception& ex) { DICE_LOG_ERROR("Lua 排队回复发送失败：{}", ex.what()); }
            guard.lock();
            if (generation != runtimeGeneration_) return;
        }
    }
}

std::string LuaPluginManager::finishCoroutine(PendingCoroutine& pending, lua_State* coroutine,
                                               int resultCount) {
    std::string result;
    if (resultCount > 0) {
        const int firstResult = lua_gettop(coroutine) - resultCount + 1;
        result = dnx_normalizeLuaText(argStr(coroutine, firstResult));
    }
    lua_rawgeti(state_, LUA_REGISTRYINDEX, pending.messageRef);
    if (lua_istable(state_, -1)) {
        lua_pushnil(state_);
        while (lua_next(state_, -2)) {
            lua_pushvalue(state_, -2);
            const std::string key = dnx_normalizeLuaText(argStr(state_, -1));
            lua_pop(state_, 1);
            if (!key.empty() && (lua_isstring(state_, -1) || lua_isboolean(state_, -1)))
                pending.vars[key] = lua_isboolean(state_, -1) ? (lua_toboolean(state_, -1) ? "true" : "false")
                    : dnx_normalizeLuaText(argStr(state_, -1));
            lua_pop(state_, 1);
        }
    }
    const auto rendered = dnx_normalizeLuaText(formatTemplate(result, pending.vars, 0, dnx_messageReader(state_, -1), dnx_messageLength(state_, -1)));
    lua_pop(state_, 1);
    return rendered;
}

void LuaPluginManager::resumeCoroutine(int threadRef) {
    std::unique_lock<ScriptMutex> lock(mutex_);
    auto it = pendingCoroutines_.find(threadRef);
    if (!state_ || it == pendingCoroutines_.end()) return;
    PendingCoroutine& pending = it->second;
    auto context = pending.logContext;
    context.trigger += " → sleepTime 延时续跑";
    ProcessingScope logScope(std::move(context));
    if (pending.generation != runtimeGeneration_) {
        pendingCoroutines_.erase(it);
        return;
    }
    if (suspendedMods_.count(pending.modName)
        || (groupGate_ && !pending.gid.empty() && !groupGate_(pending.platform, pending.gid, "lua:" + pending.modName, pending.adapterId))
        || (trustLookup_ && !pending.uid.empty() && trustLookup_(pending.uid, pending.gid, pending.platform, pending.adapterId, pending.gid.empty()) < 0)) {
        releaseLegacyLocks(pending.locks);
        luaL_unref(state_, LUA_REGISTRYINDEX, pending.messageRef);
        luaL_unref(state_, LUA_REGISTRYINDEX, threadRef);
        pendingCoroutines_.erase(it); return;
    }
    lua_rawgeti(state_, LUA_REGISTRYINDEX, threadRef);
    lua_State* coroutine = lua_tothread(state_, -1);
    lua_pop(state_, 1);
    if (!coroutine) {
        releaseLegacyLocks(pending.locks);
        luaL_unref(state_, LUA_REGISTRYINDEX, pending.messageRef);
        luaL_unref(state_, LUA_REGISTRYINDEX, threadRef);
        pendingCoroutines_.erase(it);
        return;
    }
    lua_rawgeti(state_, LUA_REGISTRYINDEX, pending.messageRef);
    if (pending.event) { lua_pushvalue(state_, -1); lua_setglobal(state_, "event"); }
    lua_setglobal(state_, "msg");
    const std::string oldPlatform = activePlatform_;
    const std::string oldAdapter = activeAdapter_;
    const std::string oldDir = loadingModDir_;
    activePlatform_ = pending.platform;
    activeAdapter_ = pending.adapterId;
    loadingModDir_ = pending.modDir;
    InvocationScope invocation(*this, pending.platform, pending.adapterId, pending.gid, pending.uid, pending.modName, pending.virtualEvent);
    int resultCount = 0;
    const int status = dnx_resume(coroutine, state_, 0, &resultCount, true);
    activePlatform_ = oldPlatform;
    activeAdapter_ = oldAdapter;
    loadingModDir_ = oldDir;
    if (status == LUA_YIELD) {
        const double delay = yieldedDelaySeconds(coroutine, resultCount);
        scheduleCoroutineResume(threadRef, delay);
        return;
    }
    std::string reply;
    std::string error;
    if (status == LUA_OK) { if (!pending.event) reply = finishCoroutine(pending, coroutine, resultCount); }
    else error = dnx_normalizeLuaText(dnx_coroutineError(coroutine));
    if (ScriptExecution::expired()) suspendedMods_.insert(pending.modName);
    const std::string platform = pending.platform;
    const std::string adapterId = pending.adapterId;
    const std::string gid = pending.gid;
    const std::string uid = pending.uid;
    const std::string ruleName = pending.ruleName;
    releaseLegacyLocks(pending.locks);
    luaL_unref(state_, LUA_REGISTRYINDEX, pending.messageRef);
    luaL_unref(state_, LUA_REGISTRYINDEX, threadRef);
    pendingCoroutines_.erase(it);
    const auto sender = asyncReply_;
    const auto scopedSender = scopedAsyncReply_;
    lock.unlock();
    if (!error.empty()) {
        logPluginFailure("执行延时回复「" + ruleName + "」", error);
        return;
    }
    if (!reply.empty() && (sender || scopedSender)) {
        auto complete = activeProcessingContext;
        complete.result = "延时处理完成，生成 1 条回复";
        ProcessingScope completedScope(std::move(complete));
        try { if (scopedSender) scopedSender(platform, adapterId, reply, gid, uid); else sender(platform, gid, uid, reply); }
        catch (const std::exception& e) { DICE_LOG_ERROR("[lua] async reply host callback failed: {}", e.what()); }
        catch (...) { DICE_LOG_ERROR("[lua] async reply host callback failed"); }
    }
}
static std::string dnx_pickInlineReply(const std::string& raw) {
    const auto cards = json::parse(raw, nullptr, false);
    if (!cards.is_array() || cards.empty() || cards.size() > 10000) return {};
    std::vector<std::pair<std::string, double>> choices;
    std::vector<double> weights;
    for (const auto& card : cards) {
        if (!card.is_string()) return {}; // data only; never interpret actions inside an array
        auto text = card.get<std::string>();
        double weight = 1;
        if (text.rfind("::", 0) == 0) {
            const auto end = text.find("::", 2);
            if (end != std::string::npos) {
                const auto number = json::parse(text.substr(2, end - 2), nullptr, false);
                if (number.is_number()) {
                    weight = number.get<double>();
                    if (!std::isfinite(weight) || weight < 0 || weight > 1000000000) return {};
                    text.erase(0, end + 2);
                }
            }
        }
        choices.emplace_back(std::move(text), weight); weights.push_back(weight);
    }
    if (std::all_of(weights.begin(), weights.end(), [](double n) { return n == 0; })) return {};
    static thread_local std::mt19937_64 random{std::random_device{}()};
    const auto index = std::discrete_distribution<size_t>(weights.begin(), weights.end())(random);
    return choices[index].first;
}

bool LuaPluginManager::hasCommandTrigger(
        const std::string& text, const std::string& uid, const std::string& gid,
        const std::string& nick, const std::string& groupCard, bool isPrivate,
        int trust, const std::string& platform, const std::string& adapterId) {
    std::lock_guard<ScriptMutex> lk(mutex_);
    if (!state_ || text.empty() || replyRules_.empty()) return false;
    const auto foldedText = legacy_mod::lower(text);
    std::map<std::string, std::string> vars = {
        {"uid", uid}, {"user", uid}, {"gid", gid}, {"grp", gid},
        {"platform", platform}, {"nick", nick}, {"card", groupCard}, {"pc", nick}, {"self", selfName_},
        {"fromMsg", text}, {"fromUser", uid}, {"fromGroup", gid},
    };
    for (const auto& rule : replyRules_) {
        if (suspendedMods_.count(rule.modName)) continue;
        if (groupGate_ && !gid.empty() && !groupGate_(platform, gid, "lua:" + rule.modName, adapterId)) continue;
        if ((rule.requiresGame || !rule.requiredRule.empty())
            && (gid.empty() || !ruleGate_ || !ruleGate_(platform, gid, uid, rule.requiredRule, adapterId))) continue;
        if (!matchesLimits(rule, uid, gid, isPrivate, trust, platform, adapterId)) continue;
        for (const auto& pat : rule.matchPatterns)
            if (rule.legacySourceFile.empty() ? legacy_mod::lower(formatTemplate(pat, vars)) == foldedText : formatTemplate(pat, vars) == text) return true;
        for (const auto& pat : rule.prefixPatterns)
            if (rule.legacySourceFile.empty() ? legacy_mod::lower(formatTemplate(pat, vars)) == foldedText : formatTemplate(pat, vars) == text) return true;
    }
    return false;
}

LuaPluginManager::DispatchResult LuaPluginManager::dispatch(
        const std::string& text, const std::string& uid, const std::string& gid,
        const std::string& nick, const std::string& groupCard, bool isPrivate, int trust, const std::string& platform, const std::string& adapterId) {
    std::lock_guard<ScriptMutex> lk(mutex_);
    DispatchResult res;
    if (!state_) return res;
    // eventMsg can synchronously re-enter dispatch. Its temporary global msg
    // must not replace the outer message used by closures or final rendering.
    lua_getglobal(state_, "msg");
    const int previousMessage = luaL_ref(state_, LUA_REGISTRYINDEX);
    struct RestoreMessage {
        lua_State*& current;
        const std::atomic<uint64_t>& generation;
        lua_State* savedState;
        uint64_t savedGeneration;
        int reference;
        ~RestoreMessage() {
            if (current != savedState || generation != savedGeneration) return;
            lua_rawgeti(current, LUA_REGISTRYINDEX, reference);
            lua_setglobal(current, "msg");
            luaL_unref(current, LUA_REGISTRYINDEX, reference);
        }
    } restoreMessage{state_, runtimeGeneration_, state_, runtimeGeneration_, previousMessage};
    const auto foldedText = legacy_mod::lower(text);

    std::map<std::string, std::string> base = {
        {"uid", uid}, {"user", uid}, {"gid", gid}, {"grp", gid},
        {"platform", platform}, {"nick", nick}, {"card", groupCard}, {"pc", nick}, {"self", selfName_},
        {"fromMsg", text}, {"fromUser", uid}, {"fromGroup", gid},
    };
    struct Candidate { ReplyRule* rule; int kind; size_t prefixLength; std::string suffix; };
    std::vector<Candidate> candidates;
    for (auto& rule : replyRules_) {
        const auto ruleKey = json::array({rule.modName, rule.name}).dump();
        if (!dnx_queuedRuleKey.empty() && ruleKey != dnx_queuedRuleKey) continue;
        if (suspendedMods_.count(rule.modName)) continue;
        if (groupGate_ && !gid.empty() && !groupGate_(platform, gid, "lua:" + rule.modName, adapterId)) continue;  // 分群停用
        if ((rule.requiresGame || !rule.requiredRule.empty())
            && (gid.empty() || !ruleGate_ || !ruleGate_(platform, gid, uid, rule.requiredRule, adapterId))) continue;
        if (!matchesLimits(rule, uid, gid, isPrivate, trust, platform, adapterId)) continue;
        const bool directoryRule = rule.legacySourceFile.empty();
        bool hit = false; std::string suffix; int kind = 0; size_t prefixLength = 0;
        for (auto& pat : rule.matchPatterns)
            if (directoryRule ? legacy_mod::lower(formatTemplate(pat, base)) == foldedText : formatTemplate(pat, base) == text) { hit = true; break; }
        if (!hit) { kind = 1; for (auto& pat : rule.prefixPatterns) {
            std::string p = formatTemplate(pat, base);
            if (!p.empty() && (directoryRule ? foldedText.rfind(legacy_mod::lower(p), 0) == 0 : text.rfind(p, 0) == 0) && p.size() > prefixLength) {
                hit = true;
                size_t s = p.size(); while (s < text.size() && text[s] == ' ') ++s;   // 跳过前缀后空格
                prefixLength = p.size(); suffix = text.substr(s);
            }
        }
        }
        if (!hit) { kind = 2; for (auto& pat : rule.searchPatterns) {
            std::string p = formatTemplate(pat, base);
            if (!p.empty() && (directoryRule ? foldedText.find(legacy_mod::lower(p)) != std::string::npos : text.find(p) != std::string::npos)) { hit = true; break; }
        }
        }
        if (!hit && text.size() <= 400) { kind = 3; for (const auto& p : rule.regexPatterns) {
            if (p.empty()) continue;
            try {
                const auto match = boundedRegex(p, text, RegexMode::Full);
                if (match.matched) {
                    hit = true;
                    if (match.groups.size() > 1) suffix = match.groups[1];
                    break;
                }
            } catch (const std::regex_error& ex) {
                DICE_LOG_WARN("[lua] reply '{}:{}' invalid Regex '{}': {}", rule.modName, rule.name, p, ex.what());
            }
        }
        }
        if (hit) candidates.push_back({&rule, kind, prefixLength, std::move(suffix)});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        const bool ag = a.rule->requiresGame || !a.rule->requiredRule.empty();
        const bool bg = b.rule->requiresGame || !b.rule->requiredRule.empty();
        if (ag != bg) return ag;
        if (ag && a.rule->requiredRule.size() != b.rule->requiredRule.size()) return a.rule->requiredRule.size() > b.rule->requiredRule.size();
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.kind == 1 && a.prefixLength != b.prefixLength) return a.prefixLength > b.prefixLength;
        const bool ad = a.rule->legacySourceFile.empty(), bd = b.rule->legacySourceFile.empty();
        if (ad != bd) return ad;
        return ad ? a.rule > b.rule : false; // directory mods: last-loaded same trigger wins
    });
    static thread_local std::mt19937_64 random{std::random_device{}()};
    for (const auto& candidate : candidates) {
        auto& rule = *candidate.rule;
        InvocationScope invocation(*this, platform, adapterId, gid, uid, rule.modName);
        const auto& suffix = candidate.suffix;
        ScriptFaultScope fault(suspendedMods_, rule.modName);
        if (dnx_queuedRuleKey.empty() && rule.limits && rule.limits->probability > 0
            && std::uniform_int_distribution<int>(1, 100)(random) > rule.limits->probability) continue;
        base["suffix"] = suffix;
        const auto source = loggingSource(rule.modName, rule.name);
        ProcessingScope logScope(source, text);
        res.sources = {source};
        std::string notice; std::vector<std::string> locks; bool busy = false;
        if (!consumeLimits(rule, uid, gid, isPrivate, platform, adapterId, notice, locks, busy)) {
            res.matched = true;
            if (busy && scheduler_ && (asyncReply_ || scopedAsyncReply_) && queuedDispatches_.size() < 256 && dnx_lockReplayDepth < 16) {
                QueuedDispatch task;
                task.text = text; task.uid = uid; task.gid = gid; task.nick = nick; task.card = groupCard;
                task.platform = platform; task.adapterId = adapterId; task.isPrivate = isPrivate; task.trust = trust;
                task.ruleKey = json::array({rule.modName, rule.name}).dump();
                task.depth = dnx_lockReplayDepth + 1; task.generation = runtimeGeneration_;
                task.locks = std::move(locks); task.context = activeProcessingContext;
                task.virtualEvent = invocationOrigin_.virtualEvent;
                queuedDispatches_.push_back(std::move(task));
                res.pending = true;
            } else if (busy) {
                res.failed = true;
                DICE_LOG_WARN("模组「{}」回复「{}」的同步锁等待队列已满或不可用，本次拒绝执行。", rule.modName, rule.name);
            }
            res.reply = dnx_normalizeLuaText(formatTemplate(notice, base));
            return res;
        }
        LimitLease lease{*this, std::move(locks)};
        if (rule.staticEchoDefined || !rule.echoDeckJson.empty()) {
            res.matched = true;
            const auto raw = rule.staticEchoDefined ? rule.staticEcho : dnx_pickInlineReply(rule.echoDeckJson);
            res.reply = dnx_normalizeLuaText(formatTemplate(raw, base));
            return res;
        }

        // 构造 msg 全局表（echo 可读写）。
        lua_newtable(state_);
        auto setf = [&](const char* k, const std::string& v) {
            lua_pushlstring(state_, v.data(), v.size()); lua_setfield(state_, -2, k);
        };
        setf("uid", uid); setf("gid", gid); setf("platform", platform); setf("nick", nick); setf("card", groupCard);
        setf("fromMsg", text); setf("fromUser", uid); setf("fromGroup", gid); setf("fromQQ", uid); setf("suffix", suffix);
        // Context 化（原版 msg userdata 语义）：挂方法 echo/format/get/inc + 动态字段
        // char/game/user/grp/at/pc…，并把 uid/gid 转为整数（原版类型）。失败则降级为纯表。
        lua_getglobal(state_, "__dnx_wrap_msg");
        if (lua_isfunction(state_, -1)) {
            lua_pushvalue(state_, -2);
            lua_pushinteger(state_, trust);
            if (dnx_pcall(state_, 2, 1) == LUA_OK) lua_remove(state_, -2);
            else { DICE_LOG_ERROR("[lua] wrap_msg: {}", argStr(state_, -1)); lua_pop(state_, 1); }
        } else lua_pop(state_, 1);
        lua_setglobal(state_, "msg");

        // echo 函数、脚本与旧 msg_order 共用可挂起协程；没有调度器的离线
        // 调用保持同步，不会创建后台线程或执行尚未授权的旧回调。
        loadingModDir_ = rule.modDir;
        int sbase = lua_gettop(state_);
        const std::string previousPlatform = activePlatform_;
        const std::string previousAdapter = activeAdapter_;
        activePlatform_ = platform; activeAdapter_ = adapterId;
        bool callOk = false;
        bool usedCoroutine = false;
        std::string callError;
        std::string tmpl;
        bool legacyCallableReady = false;
        if (rule.echoScript.empty() && !rule.legacySourceFile.empty()) {
            const fs::path sourcePath(std::u8string(
                rule.legacySourceFile.begin(), rule.legacySourceFile.end()));
            const int loadStatus = dnx_loadLegacyEnvironment(state_, sourcePath);
            if (loadStatus != LUA_OK) {
                callError = dnx_normalizeLuaText(argStr(state_, -1));
            } else {
                const int env = lua_absindex(state_, -1);
                lua_getfield(state_, env, rule.legacyFunctionName.c_str());
                if (lua_isfunction(state_, -1)) {
                    lua_remove(state_, env);  // closure retains its fresh plugin environment
                    legacyCallableReady = true;
                } else {
                    lua_pop(state_, 1);
                    lua_remove(state_, env);
                    callError = "legacy function '" + rule.legacyFunctionName + "' not found";
                }
            }
        }
        if (callError.empty() && !rule.echoScript.empty()) {
            const auto script = resolveScript(rule.echoScript, rule.modDir);
            const int status = !script.empty() ? dnx_loadfile(state_, script)
                : luaL_loadbufferx(state_, rule.echoScript.data(), rule.echoScript.size(), rule.name.c_str(), "t");
            if (status != LUA_OK) callError = dnx_normalizeLuaText(argStr(state_, -1));
            else { dnx_isolateScriptChunk(state_); legacyCallableReady = true; }
        }
        const int argumentCount = rule.echoScript.empty() && rule.echoTakesMessage ? 1 : 0;
        if (!callError.empty()) {
            // The common error path below logs and restores the stack.
        } else if (scheduler_) {
            if (pendingCoroutines_.size() >= 256) {
                DICE_LOG_WARN("[lua] suspended coroutine limit reached; refusing reply '{}'", rule.name);
                lua_settop(state_, sbase);
                activePlatform_ = previousPlatform;
                activeAdapter_ = previousAdapter;
                loadingModDir_.clear();
                res.matched = true;
                res.reply = "Lua 插件等待中的任务过多，请稍后重试。";
                res.failed = true;
                return res;
            }
            usedCoroutine = true;
            lua_getglobal(state_, "msg");
            const int messageRef = luaL_ref(state_, LUA_REGISTRYINDEX);
            lua_State* coroutine = lua_newthread(state_);
            const int threadRef = luaL_ref(state_, LUA_REGISTRYINDEX);
            if (!legacyCallableReady)
                lua_rawgeti(state_, LUA_REGISTRYINDEX, rule.echoRef);
            lua_xmove(state_, coroutine, 1);
            if (argumentCount) {
                lua_rawgeti(state_, LUA_REGISTRYINDEX, messageRef);
                lua_xmove(state_, coroutine, 1);
            }
            int resultCount = 0;
            const int status = dnx_resume(coroutine, state_, argumentCount, &resultCount, true);
            if (status == LUA_YIELD) {
                PendingCoroutine pending;
                pending.generation = runtimeGeneration_;
                pending.threadRef = threadRef;
                pending.messageRef = messageRef;
                pending.ruleName = rule.name; pending.modName = rule.modName; pending.modDir = rule.modDir;
                pending.platform = platform; pending.adapterId = adapterId; pending.gid = gid; pending.uid = uid;
                pending.vars = base;
                pending.logContext = activeProcessingContext;
                pending.virtualEvent = invocationOrigin_.virtualEvent;
                pending.locks = lease.keys; lease.retain = true;
                pendingCoroutines_[threadRef] = std::move(pending);
                const double delay = yieldedDelaySeconds(coroutine, resultCount);
                scheduleCoroutineResume(threadRef, delay);
                lua_rawgeti(state_, LUA_REGISTRYINDEX, messageRef);
                lua_setglobal(state_, "msg");
                activePlatform_ = previousPlatform;
                activeAdapter_ = previousAdapter;
                loadingModDir_.clear();
                lua_settop(state_, sbase);
                res.matched = true;
                res.pending = pendingCoroutines_.count(threadRef) != 0;
                res.failed = !res.pending;
                return res;
            }
            if (status == LUA_OK) {
                if (resultCount > 0) {
                    const int firstResult = lua_gettop(coroutine) - resultCount + 1;
                    tmpl = dnx_normalizeLuaText(argStr(coroutine, firstResult));
                }
                callOk = true;
            } else callError = dnx_normalizeLuaText(dnx_coroutineError(coroutine));
            lua_rawgeti(state_, LUA_REGISTRYINDEX, messageRef);
            lua_setglobal(state_, "msg");
            luaL_unref(state_, LUA_REGISTRYINDEX, messageRef);
            luaL_unref(state_, LUA_REGISTRYINDEX, threadRef);
        } else {
            if (!legacyCallableReady)
                lua_rawgeti(state_, LUA_REGISTRYINDEX, rule.echoRef);
            if (argumentCount) lua_getglobal(state_, "msg");
            callOk = (dnx_pcall(state_, argumentCount, LUA_MULTRET) == LUA_OK);
        }
        activePlatform_ = previousPlatform;
        activeAdapter_ = previousAdapter;
        loadingModDir_.clear();
        if (!callOk) {
            res.failed = true;
            if (callError.empty()) callError = dnx_normalizeLuaText(argStr(state_, -1));
            logPluginFailure("执行回复「" + rule.name + "」", callError);
            lua_settop(state_, sbase); res.matched = true; return res;
        }
        // 取第一个返回值作回复（msg_order 常 return reply, "" 两值）。
        if (!usedCoroutine)
            tmpl = (lua_gettop(state_) > sbase) ? dnx_normalizeLuaText(argStr(state_, sbase + 1)) : "";
        lua_settop(state_, sbase);
        // 读回 msg 表里 echo 设置的字段（msg.favor 等）→ 合入 vars。
        std::map<std::string, std::string> vars = base;
        lua_getglobal(state_, "msg");
        if (lua_istable(state_, -1)) {
            lua_pushnil(state_);
            while (lua_next(state_, -2)) {
                lua_pushvalue(state_, -2);                     // 复制 key
                std::string k = dnx_normalizeLuaText(argStr(state_, -1)); lua_pop(state_, 1);
                if (!k.empty() && (lua_isstring(state_, -1) || lua_isboolean(state_, -1)))
                    vars[k] = lua_isboolean(state_, -1) ? (lua_toboolean(state_, -1) ? "true" : "false")
                        : dnx_normalizeLuaText(argStr(state_, -1));
                lua_pop(state_, 1);
            }
        }
        res.matched = true;
        res.reply = dnx_normalizeLuaText(formatTemplate(tmpl, vars, 0, dnx_messageReader(state_, -1), dnx_messageLength(state_, -1)));
        lua_pop(state_, 1);
        if (!res.failed && !res.pending && res.reply.empty())
            logProcessingResult(source, "处理完成，没有返回文本回复", activeProcessingContext.trigger);
        return res;
    }
    return res;
}

ProcessingSource LuaPluginManager::loggingSource(const std::string& name, const std::string& entry) const {
    for (const auto& mod : mods_) if (mod.name == name)
        return {"Lua 插件", mod.title.empty() ? name : mod.title, mod.dir, entry};
    return {"Lua 插件", name.empty() ? "未识别插件" : name, "", entry};
}

}  // namespace dice
