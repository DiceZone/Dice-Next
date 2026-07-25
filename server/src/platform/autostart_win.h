#pragma once
// ─── Dice!Next — 开机自启动（Windows 当前用户）────────────────
// 通过 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 写/删 exe 路径。
// 注：exe 启动时会 chdir 到自身目录（见 main.cpp），故从 Run 启动也能找到 data/。
// Windows 实现 + 其它平台 no-op。

#include <string>

#if defined(_WIN32)
#include <windows.h>

namespace dice {

inline const wchar_t* autostartName() { return L"DiceNext"; }

inline std::wstring exePathW() {
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

inline bool isAutostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = 0;
    bool ok = RegQueryValueExW(k, autostartName(), nullptr, &type, nullptr, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

inline bool setAutostart(bool enable) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok;
    if (enable) {
        std::wstring p = L"\"" + exePathW() + L"\"";
        ok = RegSetValueExW(k, autostartName(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(p.c_str()),
                            static_cast<DWORD>((p.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(k, autostartName());
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(k);
    return ok;
}

}  // namespace dice

#else
namespace dice {
inline bool isAutostartEnabled() { return false; }
inline bool setAutostart(bool) { return false; }
}
#endif
