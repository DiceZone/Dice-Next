#pragma once
// lightweight host system-info gathering. Implemented in system_info.cpp
// so <windows.h> stays out of the big header TUs (avoids the max/min macro clash).
#include <string>
#include <vector>
#include <cstdint>

namespace dice::sysinfo {

/// A mounted fixed disk / volume.
struct DiskInfo {
    std::string mount;          // e.g. "C:\\"
    std::string label;          // volume label (may be empty)
    std::string fs;             // filesystem, e.g. "NTFS"
    std::string model;          // physical drive model (WMI; may be empty)
    uint64_t    totalGB = 0;
    uint64_t    usedGB  = 0;
    int         loadPct = 0;
};

struct SysInfo {
    std::string os          = "Unknown";   // pretty display name, e.g. "Ubuntu 22.04 LTS"
    std::string osId        = "unknown";    // logo key: windows/ubuntu/debian/centos/rocky/linux/macos
    // CPU
    std::string cpuModel;                 // e.g. "AMD Ryzen 7 5800X"
    int      cpuCores    = 0;             // logical processors
    int      cpuPhysical = 0;             // physical cores (0 = unknown)
    int      cpuMhz      = 0;             // base frequency in MHz (0 = unknown)
    double   cpuLoadPct  = -1.0;          // -1 = unavailable
    // Memory
    uint64_t memTotalMB  = 0;
    uint64_t memUsedMB   = 0;
    int      memLoadPct  = 0;
    int      memSpeedMhz = 0;             // module speed (WMI; 0 = unknown)
    uint64_t procMemMB   = 0;             // this process's working set
    // Disks (all fixed volumes)
    std::vector<DiskInfo> disks;
};

/// Gather system info. On Windows samples CPU over a brief interval (~120ms).
/// Static hardware details (CPU model, physical cores, RAM speed, disk models)
/// are probed once and cached; only live load figures are re-sampled per call.
SysInfo gather();

}  // namespace dice::sysinfo
