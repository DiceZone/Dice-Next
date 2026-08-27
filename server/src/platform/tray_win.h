#pragma once
// ─── Dice!Next — Windows system tray icon ─────────────────────
// A native tray icon with a right-click menu: open app folder, show/hide the
// console window, open the web panel, and quit. Runs on its own thread with a
// hidden top-level window + message pump (Shell_NotifyIcon). The window is not
// message-only because it has to receive the shell's "TaskbarCreated" broadcast.
// Windows-only; a no-op stub elsewhere.

#include <cstdint>
#include <functional>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <string>
#include "autostart_win.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")   // 注册表（开机自启）

namespace dice {

namespace tray_detail {
    inline std::function<void()> g_onExit;   // captured for the (captureless) WndProc
    inline uint16_t g_port = 18088;
    inline bool g_consoleVisible = true;

    constexpr UINT WM_TRAY = WM_USER + 1;
    enum { ID_DIR = 1, ID_CONSOLE, ID_WEB, ID_AUTOSTART, ID_EXIT };

    // Explorer drops every tray icon when it restarts — after a shell crash, an
    // update, or a DPI change — and asks each owner to put its own back by
    // broadcasting the registered "TaskbarCreated" message.  Without answering
    // it the icon is gone for good while the server keeps running, which looks
    // exactly like the program died.  The icon data is kept here so the window
    // procedure can re-add it.
    inline UINT g_taskbarCreated = 0;
    inline NOTIFYICONDATAW g_nid{};

    inline LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);   // already present: returns FALSE, harmless
            return 0;
        }
        if (msg == WM_TRAY) {
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_DIR,     L"打开应用目录");
                // Label reflects the action it performs: showing → "隐藏控制台", hidden → "显示控制台".
                AppendMenuW(menu, MF_STRING, ID_CONSOLE, g_consoleVisible ? L"隐藏控制台" : L"显示控制台");
                AppendMenuW(menu, MF_STRING, ID_WEB,     L"打开网页面板");
                // 开机自启动：勾选反映当前状态，点击切换。
                AppendMenuW(menu, MF_STRING | (isAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                            ID_AUTOSTART, L"开机自启动");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_EXIT,    L"退出");                              // 退出
                SetForegroundWindow(hWnd);   // so the menu auto-closes on click-away
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(menu);
                switch (cmd) {
                    case ID_DIR: {
                        wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
                        std::wstring p(buf); auto s = p.find_last_of(L"\\/");
                        if (s != std::wstring::npos) p = p.substr(0, s);
                        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        break;
                    }
                    case ID_CONSOLE: {
                        HWND con = GetConsoleWindow();
                        if (con) { g_consoleVisible = !g_consoleVisible; ShowWindow(con, g_consoleVisible ? SW_SHOW : SW_HIDE); }
                        break;
                    }
                    case ID_WEB: {
                        std::wstring url = L"http://localhost:" + std::to_wstring(g_port) + L"/";
                        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        break;
                    }
                    case ID_AUTOSTART:
                        setAutostart(!isAutostartEnabled());   // 切换开机自启
                        break;
                    case ID_EXIT:
                        if (g_onExit) g_onExit();
                        PostQuitMessage(0);
                        break;
                }
            }
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
} // namespace tray_detail

/// Start the tray icon on a background thread. @p onExit is invoked when the
/// user picks "退出" (wire it to stop the server cleanly). @p startHidden:
/// 启动后把控制台窗口隐藏到托盘并禁用其关闭按钮，弹气泡告知「未退出，已最小化到托盘」。
inline void startSystemTray(uint16_t port, std::function<void()> onExit, bool startHidden = false) {
    tray_detail::g_onExit = std::move(onExit);
    tray_detail::g_port = port;
    std::thread([startHidden] {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = tray_detail::WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"DiceNextTray";
        RegisterClassW(&wc);
        // Registered before the window exists so no broadcast can be missed, and
        // the value is the same for every process that asks.
        tray_detail::g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        // A top-level window, never shown.  It cannot be message-only (HWND_MESSAGE):
        // those are excluded from broadcasts, and "TaskbarCreated" is broadcast, so
        // the icon would never come back.  WS_EX_TOOLWINDOW keeps it off the taskbar
        // and it is never made visible, so nothing appears on screen.
        HWND hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"DiceNext",
                                    WS_OVERLAPPED, 0, 0, 0, 0,
                                    nullptr, nullptr, inst, nullptr);
        if (!hWnd) return;

        NOTIFYICONDATAW& nid = tray_detail::g_nid;
        nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = tray_detail::WM_TRAY;
        nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));            // exe's own icon if present…
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // …else IDI_APPLICATION
        // 悬停提示带端口，区分多开：Dice!Next(18088)
        { std::wstring tip = L"Dice!Next(" + std::to_wstring(tray_detail::g_port) + L")";
          wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE); }
        Shell_NotifyIconW(NIM_ADD, &nid);

        // 启动即最小化到托盘——隐藏控制台窗口 + 禁用其关闭按钮（避免误点 X 杀进程，
        // 退出统一走托盘「退出」），并弹一次气泡告知程序仍在后台运行。
        if (startHidden) {
            if (HWND con = GetConsoleWindow()) {
                if (HMENU sysMenu = GetSystemMenu(con, FALSE))
                    EnableMenuItem(sysMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
                ShowWindow(con, SW_HIDE);
                tray_detail::g_consoleVisible = false;   // 托盘菜单据此显示「显示控制台」
            }
            NOTIFYICONDATAW info = nid;
            info.uFlags = NIF_INFO;
            info.dwInfoFlags = NIIF_INFO;
            wcsncpy_s(info.szInfoTitle, L"Dice!Next", _TRUNCATE);
            wcsncpy_s(info.szInfo,
                L"Dice!Next 并未退出，而是最小化到此处。如需退出，请右键此图标选择“退出”。",
                _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &info);
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();
}

} // namespace dice

#else   // non-Windows: no-op
namespace dice {
inline void startSystemTray(uint16_t, std::function<void()>, bool = false) {}
}
#endif
