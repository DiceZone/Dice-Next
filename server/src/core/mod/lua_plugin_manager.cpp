#include "core/mod/lua_plugin_manager.h"
#include "common/logger.h"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <yaml-cpp/yaml.h>
#include <sqlite3.h>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <sstream>
#include <random>
#include <ctime>
#include <algorithm>

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
static inline std::string dnx_u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// 宽路径读全文（fs::path 构造 ifstream 走宽字符 API，任何文件名都能打开——
// 窄 .string() 对 emoji 等抛异常，u8 串又打不开 GBK 中文名，两头都不行）。
static inline bool dnx_readFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}
// luaL_dofile 的宽路径安全替代：读文本 + loadbuffer（chunkname 用 UTF-8 文件名）+ pcall。
// 复刻 lua loadfile 的头部处理：剥 UTF-8 BOM（EF BB BF）与 shebang 行（loadbuffer 不做，
// 不剥则带 BOM 的社区插件全部报 unexpected symbol near '<\239>'）。
static inline int dnx_dofile(lua_State* L, const std::filesystem::path& p) {
    std::string src;
    if (!dnx_readFile(p, src)) {
        lua_pushfstring(L, "cannot open %s", dnx_u8str(p.filename()).c_str());
        return LUA_ERRFILE;
    }
    size_t off = 0;
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB
        && (unsigned char)src[2] == 0xBF) off = 3;                        // UTF-8 BOM
    if (off < src.size() && src[off] == '#') {                            // shebang 行
        while (off < src.size() && src[off] != '\n') ++off;
    }
    // chunkname 用完整路径（对齐 luaL_dofile 的 source="@fullpath"——有插件靠
    // debug.getinfo(1).source 定位自己的资源目录，只给文件名会破坏它们）。
    std::string chunk = "@" + dnx_u8str(p);
    if (int r = luaL_loadbuffer(L, src.data() + off, src.size() - off, chunk.c_str()); r != LUA_OK) return r;
    return lua_pcall(L, 0, LUA_MULTRET, 0);
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
// 取配置键，并解析 &field（经 speech 模板 → 实际字段名）。
static std::string resolvedKey(LuaPluginManager* m, lua_State* L, int i) {
    std::string key = argStr(L, i);
    if (m && !key.empty() && key[0] == '&') key = m->resolveAmp(key.substr(1));
    return key;
}
// 把字符串值压栈：能转成数字就压数字（好感等算术），否则压字符串。
static void pushConfValue(lua_State* L, const std::string& v) {
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
    std::time_t tt = std::time(nullptr); std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &tt);
#else
    lt = *std::localtime(&tt);
#endif
    char b[16]; std::strftime(b, sizeof b, "%Y%m%d", &lt); return b;
}

// ── Lua ↔ JSON（人物卡可存嵌套结构：数组/对象）──────────────────
static void jsonToLua(lua_State* L, const json& j) {
    switch (j.type()) {
        case json::value_t::null: lua_pushnil(L); break;
        case json::value_t::boolean: lua_pushboolean(L, j.get<bool>()); break;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned: lua_pushinteger(L, (lua_Integer)j.get<long long>()); break;
        case json::value_t::number_float: lua_pushnumber(L, j.get<double>()); break;
        case json::value_t::string: { auto s = j.get<std::string>(); lua_pushlstring(L, s.data(), s.size()); break; }
        case json::value_t::array: {
            lua_newtable(L); int i = 1;
            for (auto& e : j) { jsonToLua(L, e); lua_rawseti(L, -2, i++); }
            break;
        }
        case json::value_t::object: {
            lua_newtable(L);
            for (auto it = j.begin(); it != j.end(); ++it) {
                jsonToLua(L, it.value());
                lua_setfield(L, -2, it.key().c_str());
            }
            break;
        }
        default: lua_pushnil(L); break;
    }
}
static json luaToJson(lua_State* L, int idx, int depth = 0) {
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
        if (m->playerCardRead(uid, scope, byName, key, value)) jsonToLua(L, value);
        else lua_pushnil(L);
        return 1;
    }
    json j = json::parse(m->cardLoad(uid, scope), nullptr, false);
    if (j.is_object() && j.contains(key)) jsonToLua(L, j[key]); else lua_pushnil(L);
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
        if (lua_pcall(L, 3, 1, 0) == LUA_OK) return 1;
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
        if (m->playerCardRead(uid, scope, byName, attr, value)) { jsonToLua(L, value); return 1; }
        if (m->playerCardRead(uid, scope, byName, "&" + attr, value)) { jsonToLua(L, value); return 1; }  // &key 兜底（DiceLua.cpp 973）
        if (lua_gettop(L) >= 4 && !lua_isnoneornil(L, 4)) { lua_pushvalue(L, 4); return 1; }
        lua_pushnil(L); return 1;
    }
    json j = json::parse(m->cardLoad(uid, scope), nullptr, false);
    if (j.is_object() && j.contains(attr)) { jsonToLua(L, j[attr]); return 1; }
    if (j.is_object() && j.contains("&" + attr)) { jsonToLua(L, j["&" + attr]); return 1; }   // &key 兜底
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
    for (int i = 1; i <= n; ++i) { if (i > 1) s += '\t'; s += argStr(L, i); }
    DICE_LOG_INFO("[lua] {}", s);
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
        for (auto& [id, val] : m->confAllUsers(key)) { pushConfValue(L, val); lua_setfield(L, -2, id.c_str()); }
        return 1;
    }
    std::string uid = argStr(L, 1);
    if (key.empty()) { lua_pushnil(L); return 1; }   // uid 可空（如 getDiceQQ() 未配=机器人自身CD），仍走查询+默认
    std::string v = m->confGet("u:" + uid, key);
    if (v.empty() && !m->/*has*/confHas("u:" + uid, key)) {   // 未存 → 返回默认(arg3)或 nil
        if (lua_gettop(L) >= 3 && !lua_isnoneornil(L, 3)) { lua_pushvalue(L, 3); return 1; }
        lua_pushnil(L); return 1;
    }
    pushConfValue(L, v); return 1;
}
static int l_setUserConf(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string uid = argStr(L, 1), key = resolvedKey(m, L, 2);
    if (key.empty()) return 0;   // uid 可空（机器人自身存储）
    if (lua_isnoneornil(L, 3)) m->confSet("u:" + uid, key, std::string());   // nil → 删除
    else m->confSet("u:" + uid, key, argStr(L, 3));
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
    pushConfValue(L, v); return 1;
}
static int l_setGroupConf(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string gid = argStr(L, 1), key = resolvedKey(m, L, 2);
    if (gid.empty() || key.empty()) return 0;
    if (lua_isnoneornil(L, 3)) m->confSet("g:" + gid, key, std::string());
    else m->confSet("g:" + gid, key, argStr(L, 3));
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
    pushConfValue(L, v); return 1;
}
static int l_setUserToday(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string uid = argStr(L, 1), key = argStr(L, 2);
    std::string scope = "today:" + todayStr() + ":" + uid;
    if (lua_isnoneornil(L, 3)) m->confSet(scope, key, std::string());
    else m->confSet(scope, key, argStr(L, 3));
    return 0;
}
static int l_getDiceDir(lua_State* L) {
    // 返回 mod/plugin 的父目录（如 data），让插件的 getDiceDir().."/plugin/x" 解析正确。
    auto* m = mgrOf(L); std::error_code ec;
    fs::path base = (m && !m->modDir().empty())
        ? fs::absolute(fs::path(m->modDir()).parent_path(), ec) : fs::current_path(ec);
    std::string s = base.string();
    for (auto& c : s) if (c == '\\') c = '/';   // 统一正斜杠（插件常拼接路径）
    lua_pushstring(L, s.c_str()); return 1;
}
// getDiceQQ()：机器人账号（main 注入 botId_，未知则空）。
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
    if (base.size() < 5 || base.substr(base.size() - 5) != ".json") base += ".json";
    return std::filesystem::path("data/self_data") / base;
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
static int l_formatTpl(lua_State* L) {
    auto* m = mgrOf(L);
    std::string text = argStr(L, 1);
    if (!m) { lua_pushlstring(L, text.data(), text.size()); return 1; }
    std::map<std::string, std::string> vars;
    vars["uid"] = argStr(L, 2); vars["gid"] = argStr(L, 3); vars["nick"] = argStr(L, 4);
    vars["fromQQ"] = vars["uid"]; vars["fromGroup"] = vars["gid"];
    std::string out = m->formatTemplate(text, vars);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// ── __dnx_conf(scope, key [, val])：通用配置读写（GameTable/团表 等 proxy 后端）──
static int l_confRaw(lua_State* L) {
    auto* m = mgrOf(L); if (!m) return 0;
    std::string scope = argStr(L, 1), key = argStr(L, 2);
    if (scope.empty() || key.empty()) return 0;
    if (lua_gettop(L) >= 3) {   // 写（nil=删）
        std::string val = lua_isnoneornil(L, 3) ? std::string() : argStr(L, 3);
        m->confSet(scope, key, val);
        return 0;
    }
    std::string v = m->confGet(scope, key);
    if (v.empty() && !m->confHas(scope, key)) return 0;
    lua_pushlstring(L, v.data(), v.size());
    return 1;
}
static int l_mkDirs(lua_State* L) {
    std::error_code ec; fs::create_directories(argStr(L, 1), ec);
    lua_pushboolean(L, !ec); return 1;
}
static int l_sleepTime(lua_State*) { return 0; }   // 原版阻塞计时；这里空操作（避免卡住消息回合）
static int l_drawDeck(lua_State* L) {
    auto* m = mgrOf(L);
    // 原版 drawDeck(fromGID, fromUID, deckName) 三参（DiceLua.cpp 1027）；牌名是最后一个参数。
    // 也容忍 1 参 drawDeck(name)。gid/uid 用于原版的 session 牌堆优先，本实现只抽公共牌堆。
    int n = lua_gettop(L);
    std::string name = (n >= 3) ? argStr(L, 3) : argStr(L, 1);
    std::string r = (m && m->deckDraw_) ? m->deckDraw_(name) : std::string();
    lua_pushstring(L, r.c_str()); return 1;
}
// sendMsg(text, groupId, userId)：插件主动发消息（经注入的 sender 路由适配器）。
static int l_sendMsg(lua_State* L) {
    auto* m = mgrOf(L);
    std::string text = argStr(L, 1), gid = argStr(L, 2), uid = argStr(L, 3);
    if (m && m->sender_ && !text.empty()) m->sender_(text, gid, uid);
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
    if (gid == "0") gid.clear();   // 原版 0=非群（私聊/系统）
    if (text.empty()) return 0;
    m->eventMsg_(text, gid, uid);
    return 0;
}

// ── json 模块（require("json") + 全局 json）：encode/decode，backed nlohmann ──
static int l_jsonEncode(lua_State* L) {
    try { std::string s = luaToJson(L, 1).dump(); lua_pushlstring(L, s.data(), s.size()); }
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
    if (m && m->httpFetch_) body = m->httpFetch_("GET", url, "", "", status);
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
    if (m && m->httpFetch_) body = m->httpFetch_("POST", url, headers, content, status);
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
static int l_loadLua(lua_State* L) {
    auto* m = mgrOf(L); std::string name = argStr(L, 1);
    if (!m || name.empty()) return 0;
    // 原版点分命名空间：loadLua("BRP.overview") → script/BRP/overview.lua（DiceLua.cpp fmt->lua_path）。
    std::string rel = name; for (auto& c : rel) if (c == '.') c = '/';
    fs::path p = fs::path(m->loadingModDir_) / "script" / (rel + ".lua");
    std::error_code ec;
    if (!fs::exists(p, ec)) { DICE_LOG_ERROR("[lua] loadLua: not found {}", dnx_u8str(p)); return 0; }
    if (dnx_dofile(L, p) != LUA_OK) {
        DICE_LOG_ERROR("[lua] loadLua '{}' error: {}", name, argStr(L, -1)); lua_pop(L, 1); return 0;
    }
    return 1;   // 脚本的返回值留在栈顶
}

// ─── LuaPluginManager ────────────────────────────────────────
LuaPluginManager::~LuaPluginManager() {
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
        {"lockPlayerCard", l_lockPlayerCard}, {"unlockPlayerCard", l_unlockPlayerCard},
        {"isPlayerCardLocked", l_isPlayerCardLocked},
        {"askExtra", l_askExtra},                                  // 平台扩展查询（原版 DD::getExtra）
        {"__dnx_sd_load", l_sdLoad}, {"__dnx_sd_save", l_sdSave},  // SelfData 后端（bootstrap 用）
        {"__dnx_roll", l_rollExpr},                                // pc:rollDice 引擎桥
        {"__dnx_fmt", l_formatTpl},                                // msg:format 模板桥
        {"__dnx_conf", l_confRaw},                                 // 通用配置读写（GameTable 等）
        {nullptr, nullptr},
    };
    for (const luaL_Reg* f = fns; f->name; ++f) lua_register(state_, f->name, f->func);
}

bool LuaPluginManager::init() {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (state_) return true;
    state_ = luaL_newstate();
    if (!state_) { DICE_LOG_ERROR("[lua] luaL_newstate failed"); return false; }
    luaL_openlibs(state_);
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
  SetMethods.add     = function(s, v) local d = rawget(s,'__d'); local had = d[v] ~= nil; d[v] = true; return not had end
  SetMethods.remove  = function(s, v) local d = rawget(s,'__d'); local had = d[v] ~= nil; d[v] = nil; return had end
  SetMethods['in']   = function(s, v) return rawget(s,'__d')[v] ~= nil end
  SetMethods.totable = function(s) local t = {}; for k in pairs(rawget(s,'__d')) do t[#t+1] = k end; return t end
  local SetM = {
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
  Set = { new = function() local o = {}; rawset(o, '__d', {}); return setmetatable(o, SetM) end }

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
      return __dnx_conf(game_scope(rawget(gt,'__g')), tostring(k))
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
        if not noFmt then text = __dnx_fmt(text, uidS, gidS, rawget(t,'nick') or '') end
        sendMsg(text, gidS, uidS)
      end,
      format = function(t, text) return __dnx_fmt(tostring(text or ''), uidS, gidS, rawget(t,'nick') or '') end,
      get = function(t, key, def)
        if key == nil then return t end
        local v = t[key]
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
    if (luaL_dostring(state_, kBootstrap) != LUA_OK) {
        DICE_LOG_ERROR("[lua] bootstrap failed: {}", argStr(state_, -1));
        lua_pop(state_, 1);
    }
    DICE_LOG_INFO("[lua] runtime initialized");
    return true;
}

void LuaPluginManager::freeRuntime() {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (state_) { lua_close(state_); state_ = nullptr; }
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

bool LuaPluginManager::eval(const std::string& code, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (!state_ && !init()) { if (err) *err = "no state"; return false; }
    if (luaL_dostring(state_, code.c_str()) != LUA_OK) {
        if (err) *err = argStr(state_, -1);
        lua_pop(state_, 1);
        return false;
    }
    return true;
}

int LuaPluginManager::loadDir(const std::string& dir) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
static void parseModDescriptor(const fs::path& descPath, LuaPluginManager::LuaMod& m) {
    try {
        std::ifstream f(descPath, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        json j = json::parse(stripJsonTrailingCommas(raw), nullptr, true, true);
        m.title = j.value("title", j.value("mod", m.name));
        m.author = j.value("author", "");
        m.version = j.value("ver", j.value("version", ""));
        m.brief = j.value("brief", "");
        if (j.contains("helpdoc") && j["helpdoc"].is_object())
            for (auto it = j["helpdoc"].begin(); it != j["helpdoc"].end(); ++it)
                if (it.value().is_string()) m.helpdoc[it.key()] = it.value().get<std::string>();
    } catch (const std::exception& ex) {
        DICE_LOG_ERROR("[lua] mod descriptor '{}' parse error: {}", dnx_u8str(descPath.filename()), ex.what());
    }
}

int LuaPluginManager::loadDirLocked(const std::string& dir) {
    dir_ = dir;
    mods_.clear();
    speech_.clear();
    replyRules_.clear();   // echo refs 随旧 state 关闭已失效
    std::error_code ec;
    int n = 0;

    // require("X") 能找到 mod/plugin 目录下的 X.lua（跨插件依赖，如 lua_useful_extensions）。
    if (state_) {
        fs::path base = fs::path(dir).parent_path();   // data
        auto fwd = [](fs::path p) { std::string s = p.string(); for (auto& c : s) if (c == '\\') c = '/'; return s; };
        std::string paths = fwd(fs::path(dir) / "?.lua") + ";" + fwd(fs::path(dir) / "?" / "init.lua") + ";"
                          + fwd(base / "plugin" / "?.lua") + ";" + fwd(base / "plugin" / "?" / "init.lua") + ";";
        for (auto& ed : extraDirs_)   // 规则包附加 lua 目录也可 require
            paths += fwd(fs::path(ed) / "?.lua") + ";" + fwd(fs::path(ed) / "?" / "init.lua") + ";";
        lua_getglobal(state_, "package");
        lua_getfield(state_, -1, "path");
        std::string cur = (lua_type(state_, -1) == LUA_TSTRING) ? argStr(state_, -1) : "";
        lua_pop(state_, 1);
        std::string np = paths + cur;
        lua_pushlstring(state_, np.data(), np.size());
        lua_setfield(state_, -2, "path");
        lua_pop(state_, 1);
    }

    // 统计 mod 目录的 reply/script/model + 触发加载，收尾 push。
    auto finishMod = [&](LuaMod& m, const fs::path& modPath, bool hasModDir) {
        if (hasModDir) {
            m.dir = modPath.string();
            std::error_code e2;
            if (fs::is_directory(modPath / "reply", e2))
                for (auto& r : fs::directory_iterator(modPath / "reply", e2))
                    if (!e2 && r.is_regular_file() && (r.path().extension() == ".lua" || r.path().extension() == ".toml")) ++m.replies;
            if (fs::is_directory(modPath / "script", e2))
                for (auto& s : fs::directory_iterator(modPath / "script", e2))
                    if (!e2 && s.is_regular_file() && s.path().extension() == ".lua") ++m.scripts;
            // 规则类信号：含 model/*.xml 属性模板（COC7.xml/DND5E.xml…）。
            if (fs::is_directory(modPath / "model", e2))
                for (auto& x : fs::directory_iterator(modPath / "model", e2))
                    if (!e2 && x.is_regular_file() && x.path().extension() == ".xml") { m.ruleCompat = true; break; }
            // 旧版 yaml 规则手册：rulebook/*.yaml（{rule, manual:{术语:解释}}）→ 并入 mod helpdoc，
            // 供 .help <术语> 查询（COC 法术/武器/神话、DND 法术/怪物/物品、Maid 手册等速查）。
            if (fs::is_directory(modPath / "rulebook", e2))
                for (auto& f : fs::directory_iterator(modPath / "rulebook", e2)) {
                    if (e2 || !f.is_regular_file()) continue;
                    auto ext = f.path().extension();
                    if (ext != ".yaml" && ext != ".yml") continue;
                    try {
                        std::ifstream yf(f.path(), std::ios::binary);
                        std::string content((std::istreambuf_iterator<char>(yf)), std::istreambuf_iterator<char>());
                        YAML::Node doc = YAML::Load(content);
                        if (doc["manual"] && doc["manual"].IsMap())
                            for (auto it = doc["manual"].begin(); it != doc["manual"].end(); ++it) {
                                std::string term = it->first.as<std::string>(""), text = it->second.as<std::string>("");
                                if (!term.empty() && !text.empty()) m.helpdoc.emplace(term, text);   // 不覆盖 descriptor.helpdoc
                            }
                    } catch (...) { /* 跳过坏 yaml */ }
                }
            if (m.enabled) loadModReplies(m);   // speech + reply(.lua/.toml) + event
        }
        mods_.push_back(std::move(m));
        ++n;
    };

    // 非抛版目录列举：遇坏文件名也不中断（throwing operator++ 会让单个怪文件名废掉整批）。
    auto safeList = [](const fs::path& d) {
        std::vector<fs::path> out; std::error_code e2;
        for (fs::directory_iterator it(d, e2), end; it != end; it.increment(e2)) {
            if (e2) { e2.clear(); continue; }
            out.push_back(it->path());
        }
        return out;
    };
    auto isDir = [](const fs::path& p) { std::error_code e2; return fs::is_directory(p, e2); };
    auto isFile = [](const fs::path& p) { std::error_code e2; return fs::is_regular_file(p, e2); };

    // 扫描一个 mod 目录（Pass1 同层 .json 描述档 / Pass2 目录型 mod / Pass3 单文件 .lua）。
    // 供主目录 data/mod 与各规则包 data/rulepacks/<包>/lua 共用。
    auto scanOne = [&](const std::string& dstr) {
        fs::path d = fs::path(std::u8string(dstr.begin(), dstr.end()));   // 按 UTF-8 构造（规则包中文目录安全）
        if (!isDir(d)) return;
        auto entries = safeList(d);
        std::set<std::string> claimed;
        for (auto& p : entries) {   // Pass 1
            try {
                if (!isFile(p) || p.extension() != ".json") continue;
                std::string name = dnx_u8str(p.stem());
                LuaMod m; m.name = name; m.title = name; m.enabled = true;
                parseModDescriptor(p, m);
                fs::path modPath = d / name;
                bool hasModDir = isDir(modPath);
                if (hasModDir) claimed.insert(name);
                finishMod(m, modPath, hasModDir);
            } catch (const std::exception& ex) { DICE_LOG_ERROR("[lua] mod '{}' load failed: {}", dnx_u8str(p.filename()), ex.what()); }
        }
        for (auto& p : entries) {   // Pass 2
            try {
                if (!isDir(p)) continue;
                std::string dname = dnx_u8str(p.filename());
                bool disabled = dname.size() > 9 && dname.substr(dname.size() - 9) == ".disabled";
                std::string name = disabled ? dname.substr(0, dname.size() - 9) : dname;
                if (claimed.count(name)) continue;
                fs::path inner = p / "descriptor.json";
                bool hasInner = isFile(inner);
                if (!hasInner && !isDir(p / "reply") && !isDir(p / "script")) continue;
                LuaMod m; m.name = name; m.title = name; m.enabled = !disabled;
                if (hasInner) parseModDescriptor(inner, m);
                finishMod(m, p, true);
            } catch (const std::exception& ex) { DICE_LOG_ERROR("[lua] mod dir '{}' load failed: {}", dnx_u8str(p.filename()), ex.what()); }
        }
        for (auto& p : entries) {   // Pass 3
            if (!isFile(p)) continue;
            std::string fn = dnx_u8str(p.filename());
            bool dis = fn.size() > 13 && fn.substr(fn.size() - 13) == ".lua.disabled";
            if (p.extension() == ".lua" || dis) { try { loadModFile(p, !dis); } catch (const std::exception& ex) { DICE_LOG_ERROR("[lua] plugin '{}' load failed: {}", fn, ex.what()); } }
        }
    };
    scanOne(dir);
    for (auto& ed : extraDirs_) scanOne(ed);   // 规则包附加 lua 目录

    // Pass 4：插件目录 data/plugin（单文件 .lua = msg_order 插件；子目录为资源，由插件引用）。
    fs::path pluginDir = fs::path(dir).parent_path() / "plugin";
    if (isDir(pluginDir))
        for (auto& p : safeList(pluginDir)) {
            if (!isFile(p)) continue;
            std::string fn = dnx_u8str(p.filename());
            bool dis = fn.size() > 13 && fn.substr(fn.size() - 13) == ".lua.disabled";
            if (p.extension() == ".lua" || dis) { try { loadModFile(p, !dis); } catch (const std::exception& ex) { DICE_LOG_ERROR("[lua] plugin '{}' load failed: {}", fn, ex.what()); } }
        }

    DICE_LOG_INFO("[lua] loaded from {} (+plugin): {} mod/plugin(s), {} reply rule(s)",
                  dir, (int)mods_.size(), (int)replyRules_.size());
    return (int)mods_.size();
}

// 载入一个 mod 的 speech/*.yaml（合并进全局 speech_）+ reply/*.lua（→ replyRules_）。
void LuaPluginManager::loadModReplies(const LuaMod& mod) {
    std::error_code ec;
    // 1) speech 词条：平铺 key:value 标量。
    fs::path sp = fs::path(mod.dir) / "speech";
    if (fs::is_directory(sp, ec)) {
        for (auto& f : fs::directory_iterator(sp, ec)) {
            if (ec || !f.is_regular_file()) continue;
            auto ext = f.path().extension();
            if (ext != ".yaml" && ext != ".yml") continue;
            try {
                std::string ytext; if (!dnx_readFile(f.path(), ytext)) continue;
                YAML::Node doc = YAML::Load(ytext);
                if (doc.IsMap())
                    for (auto it = doc.begin(); it != doc.end(); ++it)
                        if (it->second.IsScalar()) speech_[it->first.as<std::string>()] = it->second.as<std::string>();
            } catch (const std::exception& ex) {
                DICE_LOG_ERROR("[lua] speech '{}' parse error: {}", dnx_u8str(f.path().filename()), ex.what());
            }
        }
    }
    // 2) reply/*.lua：每个文件在新的全局 msg_reply 表里执行，再枚举其条目。
    fs::path rp = fs::path(mod.dir) / "reply";
    if (!fs::is_directory(rp, ec)) return;
    loadingModDir_ = mod.dir;
    for (auto& f : fs::directory_iterator(rp, ec)) {
        if (ec || !f.is_regular_file() || f.path().extension() != ".lua") continue;
        lua_newtable(state_); lua_setglobal(state_, "msg_reply");          // 清空表
        if (dnx_dofile(state_, f.path()) != LUA_OK) {
            DICE_LOG_ERROR("[lua] reply '{}' load error: {}", dnx_u8str(f.path().filename()), argStr(state_, -1));
            lua_pop(state_, 1); continue;
        }
        lua_getglobal(state_, "msg_reply");
        if (!lua_istable(state_, -1)) { lua_pop(state_, 1); continue; }
        lua_pushnil(state_);
        while (lua_next(state_, -2)) {                                     // key=-2, entry=-1
            lua_pushvalue(state_, -2);                                     // 复制 key（避免 lua_next 中转换原 key）
            ReplyRule rule;
            rule.name = argStr(state_, -1); rule.modName = mod.name; rule.modDir = mod.dir;
            lua_pop(state_, 1);
            if (lua_istable(state_, -1)) {
                // keyword.match[] / keyword.prefix[]
                // keyword.match/prefix/search 可为「字符串」或「字符串数组」（真实 mod 两种都有）。
                auto readArr = [&](const char* field, std::vector<std::string>& out) {
                    lua_getfield(state_, -1, field);
                    if (lua_type(state_, -1) == LUA_TSTRING) {
                        std::string p = argStr(state_, -1); if (!p.empty()) out.push_back(p);
                    } else if (lua_istable(state_, -1)) {
                        lua_Integer len = (lua_Integer)lua_rawlen(state_, -1);
                        for (lua_Integer i = 1; i <= len; ++i) {
                            lua_rawgeti(state_, -1, i);
                            std::string p = argStr(state_, -1); lua_pop(state_, 1);
                            if (!p.empty()) out.push_back(p);
                        }
                    }
                    lua_pop(state_, 1);
                };
                lua_getfield(state_, -1, "keyword");
                if (lua_istable(state_, -1)) {
                    readArr("match", rule.matchPatterns);
                    readArr("prefix", rule.prefixPatterns);
                    readArr("search", rule.searchPatterns);
                }
                lua_pop(state_, 1);       // keyword
                // limit.cd / limit.user_var.trust.at_least / limit.grp_id → 仅群
                lua_getfield(state_, -1, "limit");
                if (lua_istable(state_, -1)) {
                    lua_getfield(state_, -1, "cd");
                    if (lua_isnumber(state_, -1)) { rule.cdUser = rule.cdGrp = (int)lua_tointeger(state_, -1); }
                    else if (lua_istable(state_, -1)) {
                        lua_getfield(state_, -1, "user"); if (lua_isnumber(state_, -1)) rule.cdUser = (int)lua_tointeger(state_, -1); lua_pop(state_, 1);
                        lua_getfield(state_, -1, "grp");  if (lua_isnumber(state_, -1)) rule.cdGrp  = (int)lua_tointeger(state_, -1); lua_pop(state_, 1);
                    }
                    lua_pop(state_, 1);   // cd
                    lua_getfield(state_, -1, "user_var");
                    if (lua_istable(state_, -1)) {
                        lua_getfield(state_, -1, "trust");
                        if (lua_istable(state_, -1)) {
                            lua_getfield(state_, -1, "at_least");
                            if (lua_isnumber(state_, -1)) rule.trustAtLeast = (int)lua_tointeger(state_, -1);
                            lua_pop(state_, 1);
                        }
                        lua_pop(state_, 1);   // trust
                    }
                    lua_pop(state_, 1);       // user_var
                    lua_getfield(state_, -1, "grp_id");
                    if (lua_istable(state_, -1)) rule.groupOnly = true;
                    lua_pop(state_, 1);
                }
                lua_pop(state_, 1);       // limit
                // echo：函数(→registry ref) 或 {lua="脚本名"}(→跑 script/<名>.lua)
                lua_getfield(state_, -1, "echo");
                if (lua_isfunction(state_, -1)) rule.echoRef = luaL_ref(state_, LUA_REGISTRYINDEX);
                else if (lua_istable(state_, -1)) {
                    lua_getfield(state_, -1, "lua");
                    if (lua_isstring(state_, -1)) rule.echoScript = argStr(state_, -1);
                    lua_pop(state_, 1);   // lua 字段
                    lua_pop(state_, 1);   // echo 表
                } else lua_pop(state_, 1);
            }
            if ((rule.echoRef != 0 || !rule.echoScript.empty())
                && (!rule.matchPatterns.empty() || !rule.prefixPatterns.empty() || !rule.searchPatterns.empty()))
                replyRules_.push_back(std::move(rule));
            lua_pop(state_, 1);   // entry value
        }
        lua_pop(state_, 1);       // msg_reply
    }
    loadingModDir_.clear();
}

// 单文件 Lua 插件：dofile 后读 msg_order[关键字]=函数名 → 前缀匹配调用该全局函数(msg)。
void LuaPluginManager::loadModFile(const fs::path& path, bool enabled) {
    LuaMod m;
    std::string fn = dnx_u8str(path.filename());
    if (!enabled && fn.size() > 9 && fn.substr(fn.size() - 9) == ".disabled") fn = fn.substr(0, fn.size() - 9);
    m.name = (fn.rfind('.') != std::string::npos) ? fn.substr(0, fn.rfind('.')) : fn;   // 去扩展名（fn 是 UTF-8，不回喂窄 path）
    m.title = m.name;
    m.dir = path.parent_path().string();
    m.singleFile = true;
    m.enabled = enabled;
    if (!enabled) { mods_.push_back(std::move(m)); return; }

    loadingModDir_ = path.parent_path().string();
    lua_newtable(state_); lua_setglobal(state_, "msg_order");
    if (dnx_dofile(state_, path) != LUA_OK) {
        DICE_LOG_ERROR("[lua] plugin '{}' load error: {}", m.name, argStr(state_, -1));
        lua_pop(state_, 1); loadingModDir_.clear(); mods_.push_back(std::move(m)); return;
    }
    lua_getglobal(state_, "msg_order");
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
    std::sort(orders.begin(), orders.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (auto& [key, func] : orders) {
        lua_getglobal(state_, func.c_str());
        if (!lua_isfunction(state_, -1)) { lua_pop(state_, 1); continue; }
        ReplyRule rule;
        rule.name = m.name + ":" + key; rule.modName = m.name; rule.modDir = m.dir;
        rule.prefixPatterns.push_back(key);
        rule.echoRef = luaL_ref(state_, LUA_REGISTRYINDEX);   // 捕获该函数（弹出）
        replyRules_.push_back(std::move(rule));
        ++m.replies;
    }
    loadingModDir_.clear();
    mods_.push_back(std::move(m));
}

int LuaPluginManager::reload() {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    freeRuntime();
    if (!init()) return 0;
    // 规则包热卸载时目录可能刚被移除，或其中有损坏的脚本。不能让一次插件扫描
    // 异常逃到 WebUI 请求线程，否则运行时已被销毁而骰娘会停在半重载状态。
    try {
        return loadDirLocked(dir_);
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("[lua] reload failed; runtime remains available with no newly loaded mods: {}", e.what());
    } catch (...) {
        DICE_LOG_ERROR("[lua] reload failed with an unknown error; runtime remains available with no newly loaded mods");
    }
    return 0;
}

std::vector<LuaPluginManager::LuaMod> LuaPluginManager::mods() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return mods_;
}

// 清洗成合法 UTF-8（个别社区 mod 关键词含 GBK 残留字节，否则 nlohmann 序列化抛 type_error.316）。
// 非法字节序列替换为 '?'，保证 JSON 可输出。
static std::string scrubUtf8(const std::string& s) {
    std::string out; out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0) { out.push_back('?'); ++i; continue; }
        if (i + len > n) { out.push_back('?'); ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) if (((unsigned char)s[i + k] >> 6) != 0x2) { ok = false; break; }
        if (!ok) { out.push_back('?'); ++i; continue; }
        out.append(s, i, len); i += len;
    }
    return out;
}

std::vector<LuaPluginManager::ModCommand> LuaPluginManager::commandsOf(const std::string& modName) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::vector<HelpItem> out;
    for (const auto& m : mods_) {
        if (!m.enabled) continue;
        for (const auto& [topic, text] : m.helpdoc) {
            std::string t = text;
            if (!t.empty() && t[0] == '&') {   // 别名：&其他主题 → 取该主题文本（一级）
                auto it = m.helpdoc.find(t.substr(1));
                if (it != m.helpdoc.end()) t = it->second;
            }
            out.push_back({topic, t, m.name});
        }
    }
    return out;
}

bool LuaPluginManager::setModEnabled(const std::string& name, bool enabled) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (dir_.empty()) return false;
    std::error_code ec;
    // 目录型 <name> ↔ <name>.disabled；单文件型 <name>.lua ↔ <name>.lua.disabled。
    std::pair<fs::path, fs::path> cands[] = {
        { fs::path(dir_) / name,            fs::path(dir_) / (name + ".disabled") },
        { fs::path(dir_) / (name + ".lua"), fs::path(dir_) / (name + ".lua.disabled") },
    };
    for (auto& [on, off] : cands) {
        if (enabled && fs::exists(off, ec)) { fs::rename(off, on, ec); return !ec; }
        if (!enabled && fs::exists(on, ec)) { fs::rename(on, off, ec); return !ec; }
    }
    // 已是目标状态视为成功。
    for (auto& [on, off] : cands)
        if (fs::exists(enabled ? on : off, ec)) return true;
    return false;
}

bool LuaPluginManager::deleteMod(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (dir_.empty()) return false;
    std::error_code ec; bool removed = false;
    for (const auto& cand : { fs::path(dir_) / name, fs::path(dir_) / (name + ".disabled"),
                              fs::path(dir_) / (name + ".lua"), fs::path(dir_) / (name + ".lua.disabled") })
        if (fs::exists(cand, ec)) { fs::remove_all(cand, ec); if (!ec) removed = true; }
    return removed;
}

// ─── 阶段二：模板格式化 + reply 派发 ─────────────────────────────
// 解析一个 {key}：msg 变量 > speech 词条(别名 &x / 递归展开 {…}) > 内置全局。
std::string LuaPluginManager::valueOf(const std::string& key,
                                      const std::map<std::string, std::string>& vars, int depth) const {
    if (depth > 24) return "";
    if (auto it = vars.find(key); it != vars.end()) return it->second;
    if (auto it = speech_.find(key); it != speech_.end()) {
        const std::string& v = it->second;
        if (!v.empty() && v[0] == '&') return valueOf(v.substr(1), vars, depth + 1);   // 别名
        return formatTemplate(v, vars, depth + 1);                                     // 递归展开
    }
    if (key == "self" || key == "strSelfName" || key == "strSelfNick" || key == "Name") return selfName_;
    return "";   // 未知 → 空
}

std::string LuaPluginManager::formatTemplate(const std::string& text,
                                             const std::map<std::string, std::string>& vars, int depth) const {
    if (depth > 24) return text;
    std::string out; out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            size_t e = text.find('}', i + 1);
            if (e == std::string::npos) { out += text.substr(i); break; }
            out += valueOf(text.substr(i + 1, e - i - 1), vars, depth + 1);
            i = e + 1;
        } else out += text[i++];
    }
    return out;
}

std::string LuaPluginManager::resolveAmp(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return valueOf(key, {}, 0);
}

LuaPluginManager::DispatchResult LuaPluginManager::dispatch(
        const std::string& text, const std::string& uid, const std::string& gid,
        const std::string& nick, const std::string& groupCard, bool isPrivate, int trust, const std::string& platform) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    DispatchResult res;
    if (!state_ || replyRules_.empty()) return res;

    std::map<std::string, std::string> base = {
        {"uid", uid}, {"user", uid}, {"gid", gid}, {"grp", gid},
        {"nick", nick}, {"card", groupCard}, {"pc", nick}, {"self", selfName_},
        {"fromMsg", text}, {"fromUser", uid}, {"fromGroup", gid},
    };
    for (auto& rule : replyRules_) {
        if (rule.groupOnly && (isPrivate || gid.empty())) continue;
        if (rule.trustAtLeast > 0 && trust < rule.trustAtLeast) continue;   // 权限门槛
        if (groupGate_ && !gid.empty() && !groupGate_(platform, gid, "lua:" + rule.modName)) continue;  // 分群停用
        bool hit = false; std::string suffix;
        for (auto& pat : rule.matchPatterns)
            if (formatTemplate(pat, base) == text) { hit = true; break; }
        if (!hit) for (auto& pat : rule.prefixPatterns) {
            std::string p = formatTemplate(pat, base);
            if (!p.empty() && text.rfind(p, 0) == 0) {   // startsWith
                hit = true;
                size_t s = p.size(); while (s < text.size() && text[s] == ' ') ++s;   // 跳过前缀后空格
                suffix = text.substr(s); break;
            }
        }
        if (!hit) for (auto& pat : rule.searchPatterns) {
            std::string p = formatTemplate(pat, base);
            if (!p.empty() && text.find(p) != std::string::npos) { hit = true; break; }   // 包含
        }
        if (!hit) continue;
        base["suffix"] = suffix;
        // 冷却：cd:<rule>:<uid|grp>。命中但在冷却内 → 静默（matched 但空回复）。
        long now = (long)std::time(nullptr);
        auto cooled = [&](int cd, const std::string& scopeKey) {
            if (cd <= 0) return false;
            long last = 0; std::string v = confGet("cd:" + rule.name, scopeKey);
            if (!v.empty()) { try { last = std::stol(v); } catch (...) {} }
            return (now - last) < cd;
        };
        if (cooled(rule.cdUser, "u:" + uid) || (!gid.empty() && cooled(rule.cdGrp, "g:" + gid))) {
            res.matched = true; return res;
        }

        // 构造 msg 全局表（echo 可读写）。
        lua_newtable(state_);
        auto setf = [&](const char* k, const std::string& v) {
            lua_pushlstring(state_, v.data(), v.size()); lua_setfield(state_, -2, k);
        };
        setf("uid", uid); setf("gid", gid); setf("nick", nick); setf("card", groupCard);
        setf("fromMsg", text); setf("fromUser", uid); setf("fromGroup", gid); setf("fromQQ", uid); setf("suffix", suffix);
        // Context 化（原版 msg userdata 语义）：挂方法 echo/format/get/inc + 动态字段
        // char/game/user/grp/at/pc…，并把 uid/gid 转为整数（原版类型）。失败则降级为纯表。
        lua_getglobal(state_, "__dnx_wrap_msg");
        if (lua_isfunction(state_, -1)) {
            lua_pushvalue(state_, -2);
            lua_pushinteger(state_, trust);
            if (lua_pcall(state_, 2, 1, 0) == LUA_OK) lua_remove(state_, -2);
            else { DICE_LOG_ERROR("[lua] wrap_msg: {}", argStr(state_, -1)); lua_pop(state_, 1); }
        } else lua_pop(state_, 1);
        lua_setglobal(state_, "msg");

        // 运行 echo：函数 ref 或 {lua="name"} 脚本。
        loadingModDir_ = rule.modDir;
        int sbase = lua_gettop(state_);
        bool callOk;
        if (!rule.echoScript.empty()) {
            std::string erel = rule.echoScript; for (auto& c : erel) if (c == '.') c = '/';   // 点分命名空间→子目录
            fs::path sp = fs::path(rule.modDir) / "script" / (erel + ".lua");
            callOk = (dnx_dofile(state_, sp) == LUA_OK);
        } else {
            lua_rawgeti(state_, LUA_REGISTRYINDEX, rule.echoRef);
            lua_getglobal(state_, "msg");   // 作参数传入（msg_order 函数读 msg 参数；msg_reply echo 忽略多余参数）
            callOk = (lua_pcall(state_, 1, LUA_MULTRET, 0) == LUA_OK);
        }
        loadingModDir_.clear();
        if (!callOk) {
            DICE_LOG_ERROR("[lua] reply '{}' echo error: {}", rule.name, argStr(state_, -1));
            lua_settop(state_, sbase); res.matched = true; return res;
        }
        // 取「第一个」返回值作回复（msg_order 函数常 return reply, "" 两值；echo 单值）。
        std::string tmpl = (lua_gettop(state_) > sbase) ? argStr(state_, sbase + 1) : "";
        lua_settop(state_, sbase);

        // 读回 msg 表里 echo 设置的字段（msg.favor 等）→ 合入 vars。
        std::map<std::string, std::string> vars = base;
        lua_getglobal(state_, "msg");
        if (lua_istable(state_, -1)) {
            lua_pushnil(state_);
            while (lua_next(state_, -2)) {
                lua_pushvalue(state_, -2);                     // 复制 key
                std::string k = argStr(state_, -1); lua_pop(state_, 1);
                std::string v = argStr(state_, -1);
                if (!k.empty()) vars[k] = v;
                lua_pop(state_, 1);
            }
        }
        lua_pop(state_, 1);

        res.matched = true;
        res.reply = formatTemplate(tmpl, vars);
        if (rule.cdUser > 0) confSet("cd:" + rule.name, "u:" + uid, std::to_string(now));
        if (rule.cdGrp > 0 && !gid.empty()) confSet("cd:" + rule.name, "g:" + gid, std::to_string(now));
        return res;
    }
    return res;
}

}  // namespace dice
