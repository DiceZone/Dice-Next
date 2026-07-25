#pragma once
// ─── Dice!Next — 单实例锁 + 端口可用性检测 ─────────────────────
// 1) acquireInstanceLock：同一套数据（同一 data 目录）只允许一个进程运行。
//    用数据目录下的 .instance.lock 独占文件实现；进程正常退出或崩溃时由 OS
//    释放句柄（自愈，不留死锁）。
// 2) findAvailablePort：若配置端口被占用，自动 +1 直到找到空闲端口。
//    占用判定用「connect 到 127.0.0.1:port」探测——能连上即有人监听（连
//    仍处于 LISTEN 的僵尸进程也能识别，正是本项目的老坑）。
// Windows 实现 + POSIX 回退。

#include <string>
#include <cstdint>

#if defined(_WIN32)
#include <winsock2.h>     // 必须在 <windows.h> 之前；它会阻止 windows.h 引入旧 winsock.h
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace dice {

// 获取与 @p lockFilePath 绑定的独占锁。成功返回 true 并在整个进程生命周期持有；
// 若已有实例持有则返回 false。句柄故意泄漏（held for process lifetime）。
inline bool acquireInstanceLock(const std::string& lockFilePath) {
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, lockFilePath.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, lockFilePath.c_str(), -1, &wpath[0], wlen);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0 /* 不共享 */, nullptr,
                           CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // 共享冲突 = 已有实例
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    DWORD wr = 0; if (n > 0) WriteFile(h, buf, static_cast<DWORD>(n), &wr, nullptr);
    return true;   // 句柄随进程存活，退出/崩溃由 OS 释放并删除锁文件
#else
    int fd = ::open(lockFilePath.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return false; }
    return true;   // fd 随进程存活
#endif
}

// 127.0.0.1:port 当前是否有人监听（connect 探测；回环上 refused 会立刻返回，无阻塞风险）。
inline bool portInUse(uint16_t port) {
#if defined(_WIN32)
    static bool inited = false;
    if (!inited) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); inited = true; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bool used = (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    closesocket(s);
    return used;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bool used = (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(s);
    return used;
#endif
}

// 从 @p start 起向上找第一个空闲端口（最多扫 @p range 个）。都被占用则返回 start。
inline uint16_t findAvailablePort(uint16_t start, int range = 50) {
    for (int i = 0; i < range; ++i) {
        uint16_t p = static_cast<uint16_t>(start + i);
        if (p < start) break;   // 溢出保护
        if (!portInUse(p)) return p;
    }
    return start;
}

} // namespace dice
