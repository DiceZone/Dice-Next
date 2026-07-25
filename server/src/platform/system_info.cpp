#include "system_info.h"
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
static uint64_t ftToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
#endif

namespace dice::sysinfo {

SysInfo gather() {
    SysInfo s;
    s.cpuCores = static_cast<int>(std::thread::hardware_concurrency());
#ifdef _WIN32
    s.os = "Windows";

    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.memTotalMB = ms.ullTotalPhys / (1024ull * 1024ull);
        uint64_t availMB = ms.ullAvailPhys / (1024ull * 1024ull);
        s.memUsedMB = (s.memTotalMB > availMB) ? (s.memTotalMB - availMB) : 0;
        s.memLoadPct = static_cast<int>(ms.dwMemoryLoad);
    }

    // CPU load: two samples of system-wide times. kernel time includes idle.
    FILETIME idle0, kern0, user0, idle1, kern1, user1;
    if (GetSystemTimes(&idle0, &kern0, &user0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (GetSystemTimes(&idle1, &kern1, &user1)) {
            uint64_t idle = ftToU64(idle1) - ftToU64(idle0);
            uint64_t kern = ftToU64(kern1) - ftToU64(kern0);
            uint64_t user = ftToU64(user1) - ftToU64(user0);
            uint64_t total = kern + user;   // kernel already contains idle
            if (total > 0) s.cpuLoadPct = 100.0 * static_cast<double>(total - idle) / static_cast<double>(total);
        }
    }

    PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        s.procMemMB = pmc.WorkingSetSize / (1024ull * 1024ull);
#endif
    return s;
}

}  // namespace dice::sysinfo
