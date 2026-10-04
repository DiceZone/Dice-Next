#pragma once
// Refuse archive-preview/temp-directory launches before any persistent writes.
// The path policy is platform-independent so Windows path edge cases are testable.
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace dice::startup {
inline std::wstring normalizeWindowsPath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (path.rfind(L"\\\\?\\unc\\", 0) == 0) path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    const bool unc = path.rfind(L"\\\\", 0) == 0;
    const bool drive = path.size() >= 3 && path[1] == L':' && path[2] == L'\\';
    if (!unc && !drive) return {}; // Relative paths are resolved by the Win32 wrapper.
    const std::wstring prefix = unc ? L"\\\\" : path.substr(0, 3);
    std::vector<std::wstring> parts;
    size_t pos = unc ? 2 : 3;
    while (pos < path.size()) {
        auto end = path.find(L'\\', pos);
        const auto part = path.substr(pos, end == std::wstring::npos ? end : end - pos);
        if (part == L"..") {
            if (parts.size() > (unc ? 2u : 0u)) parts.pop_back();
        } else if (!part.empty() && part != L".") parts.push_back(part);
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    std::wstring result = prefix;
    for (const auto& part : parts) {
        if (!result.empty() && result.back() != L'\\') result += L'\\';
        result += part;
    }
    return result;
}

inline bool isWithinTemp(const std::wstring& path, const std::vector<std::wstring>& roots) {
    const auto candidate = normalizeWindowsPath(path);
    if (candidate.empty()) return false;
    for (const auto& raw : roots) {
        const auto root = normalizeWindowsPath(raw);
        // Never turn an entire drive or UNC share into a temporary directory.
        if (root.size() <= 3 || (root.rfind(L"\\\\", 0) == 0 && root.find(L'\\', 2) == root.rfind(L'\\'))) continue;
        if (candidate == root || (candidate.size() > root.size() &&
            candidate.compare(0, root.size(), root) == 0 && candidate[root.size()] == L'\\')) return true;
    }
    return false;
}
} // namespace dice::startup

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "user32.lib")

namespace dice::startup {
inline std::wstring environmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::vector<wchar_t> buffer(size);
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), size);
    return length && length < size ? std::wstring(buffer.data(), length) : std::wstring();
}

inline std::wstring absolutePath(const std::wstring& path) {
    if (path.empty()) return {};
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return length && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring();
}

inline std::wstring resolvedPath(const std::wstring& path) {
    // Resolve junctions and short (8.3) aliases as well as spelling differences.
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return absolutePath(path);
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    return length && length < buffer.size() ? std::wstring(buffer.data(), length) : absolutePath(path);
}

inline std::wstring executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    std::wstring path(buffer.data(), length);
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

inline std::wstring currentDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
    return length && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring();
}

inline bool containsWebDirectory(const std::wstring& dir) {
    const auto attributes = GetFileAttributesW((dir + L"\\web").c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline std::vector<std::wstring> temporaryDirectories() {
    std::vector<std::wstring> roots;
    const auto profile = normalizeWindowsPath(resolvedPath(environmentPath(L"USERPROFILE")));
    std::vector<wchar_t> windows(32768);
    const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    const std::wstring windowsDir = length && length < windows.size() ? std::wstring(windows.data(), length) : std::wstring();
    const auto windowsRoot = normalizeWindowsPath(resolvedPath(windowsDir));
    const auto add = [&](const std::wstring& raw) {
        if (raw.empty()) return;
        const auto absolute = absolutePath(raw);
        const auto canonical = resolvedPath(raw);
        const auto normalized = normalizeWindowsPath(canonical);
        // GetTempPath falls back to USERPROFILE or Windows itself. Those are not temp roots.
        if (normalized == profile || normalized == windowsRoot) return;
        if (!absolute.empty()) roots.push_back(absolute);
        if (!canonical.empty()) roots.push_back(canonical);
    };
    add(environmentPath(L"TMP"));
    add(environmentPath(L"TEMP"));
    const auto local = environmentPath(L"LOCALAPPDATA");
    if (!local.empty()) add(local + L"\\Temp");
    if (!windowsDir.empty()) { add(windowsDir + L"\\Temp"); add(windowsDir + L"\\SystemTemp"); }
    // Dynamic lookup preserves support for older Windows versions.
    using TempPathFn = DWORD (WINAPI*)(DWORD, LPWSTR);
    const auto newer = reinterpret_cast<TempPathFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W"));
    std::vector<wchar_t> temp(32768);
    const DWORD tempLength = newer ? newer(static_cast<DWORD>(temp.size()), temp.data())
                                  : GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (tempLength && tempLength < temp.size()) add(std::wstring(temp.data(), tempLength));
    return roots;
}

inline bool allowLaunch(const std::wstring& executableDir, const std::wstring& runtimeDir) {
    const auto roots = temporaryDirectories();
    for (const auto& dir : {executableDir, runtimeDir}) {
        if (!isWithinTemp(absolutePath(dir), roots) && !isWithinTemp(resolvedPath(dir), roots)) continue;
        MessageBoxW(nullptr,
            L"检测到 Dice!Next 正在临时目录中运行，已阻止启动。\n\n"
            L"请先完整解压安装包到一个固定文件夹，再运行 dice-next.exe。"
            L"不要在压缩包内直接双击，也不要把程序放在临时目录中。\n\n"
            L"Dice!Next cannot start from a temporary directory.\n\n"
            L"Extract the entire archive to a permanent folder, then run dice-next.exe. "
            L"Do not run it directly inside the archive or keep it in a temporary folder.",
            L"Dice!Next — 无法启动 / Cannot start", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return false;
    }
    return true;
}

inline bool prepareCoreLaunch() {
    const auto dir = executableDirectory();
    // Distribution builds use their web/ directory; keep development builds in their CWD.
    // Check the intended directory before chdir, then the real CWD before any writes.
    if (containsWebDirectory(dir)) {
        if (!allowLaunch(dir, dir)) return false;
        if (!SetCurrentDirectoryW(dir.c_str())) {
            MessageBoxW(nullptr,
                L"无法切换到程序目录，已阻止启动。请检查文件夹是否存在及访问权限。\n\n"
                L"Cannot access the program directory. Please check that the folder exists and is accessible.",
                L"Dice!Next — 无法启动 / Cannot start", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return false;
        }
    }
    return allowLaunch(dir, currentDirectory());
}
} // namespace dice::startup
#else
namespace dice::startup { inline bool prepareCoreLaunch() { return true; } }
#endif
