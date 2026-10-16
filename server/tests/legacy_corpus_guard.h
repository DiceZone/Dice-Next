#pragma once

#include "../src/core/mod/lua_plugin_manager.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dice::test {

// Only for locally audited, opt-in corpora. This is defense in depth for test
// fixtures, NOT an OS sandbox or a promise that arbitrary hostile Lua is safe.
inline void copyAuditedCorpus(const std::filesystem::path& source,
                              const std::filesystem::path& destination) {
    namespace fs = std::filesystem;
    size_t files = 0;
    std::uintmax_t bytes = 0;
    auto plain = [](const fs::path& path) {
        if (fs::is_symlink(fs::symlink_status(path))) return false;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        return true;
    };
    // Check ancestors too: a plain file below a junction is not an isolated input.
    for (auto path = fs::absolute(source); !path.empty(); path = path.parent_path()) {
        if (!plain(path)) throw std::runtime_error("linked corpus path rejected");
        if (path == path.root_path()) break;
    }
    auto copy = [&](const fs::path& from, const fs::path& to) {
        if (++files > 10000 || !plain(from)) throw std::runtime_error("unsafe or oversized corpus tree");
        if (fs::is_directory(from)) { fs::create_directories(to); return; }
        if (!fs::is_regular_file(from)) throw std::runtime_error("non-file corpus input");
        const auto size = fs::file_size(from);
        bytes += size;
        if (size > 16 * 1024 * 1024 || bytes > 64 * 1024 * 1024)
            throw std::runtime_error("oversized corpus input");
        fs::create_directories(to.parent_path());
        fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    };
    copy(source, destination);
    if (fs::is_directory(source))
        for (const auto& item : fs::recursive_directory_iterator(source))
            copy(item.path(), destination / item.path().lexically_relative(source));
}

inline bool constrainAuditedCorpus(LuaPluginManager& manager,
                                   const std::filesystem::path& root,
                                   std::string* error = nullptr) {
    // Test roots are ASCII. JSON quoting is also valid for these Lua strings.
    const auto quotedRoot = nlohmann::json(std::filesystem::absolute(root).generic_string()).dump();
    return manager.eval("local corpusRoot = " + quotedRoot + R"LUA(
do
  local function denied() error('disabled in audited corpus fixture', 2) end
  local function safePath(path)
    if type(path) ~= 'string' or path:find('%z') then return nil end
    path = path:gsub('\\', '/')
    if path:sub(1,2) == '//' then return nil end
    if not path:match('^%a:/') and path:sub(1,1) ~= '/' then path = corpusRoot .. '/' .. path end
    if path:find(':', path:match('^%a:/') and 3 or 1, true) then return nil end
    local parts = {}
    for part in path:gmatch('[^/]+') do
      if part == '..' or (part ~= '.' and part:match('[%. ]$')) then return nil end
      local device = (part:match('^[^%.]+') or ''):lower()
      if device == 'con' or device == 'prn' or device == 'aux' or device == 'nul'
         or device:match('^com[1-9]$') or device:match('^lpt[1-9]$') then return nil end
      if part ~= '.' then parts[#parts+1] = part end
    end
    path = (path:sub(1,1) == '/' and '/' or '') .. table.concat(parts, '/')
    local cmp, base = path, corpusRoot
    if package.config:sub(1,1) == '\\' then cmp, base = cmp:lower(), base:lower() end
    if cmp == base or cmp:sub(1,#base+1) == base .. '/' then return path end
    return nil
  end
  local open, remove, rename, mkdir = io.open, os.remove, os.rename, mkDirs
  io.open = function(path, mode)
    local safe = safePath(path)
    -- Production's legacy resolver remaps moved absolute cache references. Do
    -- not let their first attempted open read the author's actual installation.
    if not safe and (mode == nil or mode == 'r' or mode == 'rb') then
      local mapped = __dnx_resolve_plugin_file(path)
      if mapped then safe = safePath(mapped) end
    end
    if not safe then return nil, 'outside audited corpus fixture' end
    return open(safe, mode)
  end
  os.remove = function(path)
    local safe = safePath(path)
    if not safe then return nil, 'outside audited corpus fixture' end
    return remove(safe)
  end
  os.rename = function(from, to)
    local a, b = safePath(from), safePath(to)
    if not a or not b then return nil, 'outside audited corpus fixture' end
    return rename(a, b)
  end
  mkDirs = function(path)
    local safe = safePath(path)
    if not safe then return false end
    return mkdir(safe)
  end
  for _, key in ipairs({'popen','input','output','lines','tmpfile','read','write'}) do io[key] = denied end
  io.stdin, io.stdout, io.stderr = nil, nil, nil
  for _, key in ipairs({'execute','exit','getenv','tmpname','setlocale'}) do os[key] = denied end
  for key in pairs(debug) do if key ~= 'getinfo' and key ~= 'traceback' then debug[key] = nil end end
  loadfile, dofile = denied, denied
  local textLoad = load
  load = function(chunk, name, mode, ...)
    if type(chunk) ~= 'string' then return nil, 'only text in audited corpus fixture' end
    if select('#', ...) > 0 then return textLoad(chunk, name, 't', ...) end
    return textLoad(chunk, name, 't')
  end
  local allowed = {}
  for _, name in ipairs({'json','yaml','Set','Actor','GameTable','SelfData'}) do allowed[name] = package.loaded[name] end
  require = function(name)
    if allowed[name] == nil then denied() end
    return allowed[name]
  end
  package.loadlib, package.searchpath = denied, denied
  package.searchers, package.loaders, package.preload = {}, {}, {}
  package.cpath = ''
end
)LUA", error);
}

} // namespace dice::test
