#pragma once
// C#53: lightweight host system-info gathering. Implemented in system_info.cpp
// so <windows.h> stays out of the big header TUs (avoids the max/min macro clash).
#include <string>
#include <cstdint>

namespace dice::sysinfo {

struct SysInfo {
    std::string os = "Unknown";
    int      cpuCores    = 0;    // logical cores
    double   cpuLoadPct  = -1.0;  // -1 = unavailable
    uint64_t memTotalMB  = 0;
    uint64_t memUsedMB   = 0;
    int      memLoadPct  = 0;
    uint64_t procMemMB   = 0;    // this process's working set (0 = unavailable)
};

/// Gather system info. On Windows samples CPU over a brief interval (~120ms).
SysInfo gather();

}  // namespace dice::sysinfo
