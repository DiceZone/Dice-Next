#include "system_info.h"
#include <thread>
#include <chrono>
#include <map>
#include <vector>
#include <string>

// ─────────────────────────── Windows ───────────────────────────
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

static uint64_t ftToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// UTF-16 → UTF-8.
static std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string regString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    wchar_t buf[512]; DWORD sz = sizeof(buf);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        return narrow(buf);
    return {};
}
static DWORD regDword(HKEY root, const wchar_t* sub, const wchar_t* name) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v;
    return 0;
}

// Physical core count via the processor-relationship table.
static int physicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return 0;
    std::vector<char> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
        return 0;
    int cores = 0; char* p = buf.data(); char* end = p + len;
    while (p < end) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
        if (info->Relationship == RelationProcessorCore) ++cores;
        p += info->Size;
    }
    return cores;
}

// Memory module speed (MHz) from SMBIOS table type 17 (Memory Device).
static int ramSpeedMhz() {
    UINT sz = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (sz == 0) return 0;
    std::vector<BYTE> buf(sz);
    if (GetSystemFirmwareTable('RSMB', 0, buf.data(), sz) != sz) return 0;
    if (buf.size() <= 8) return 0;                 // 8-byte RawSMBIOSData header
    const BYTE* p = buf.data() + 8; const BYTE* end = buf.data() + buf.size();
    while (p + 4 < end) {
        BYTE type = p[0]; BYTE flen = p[1];
        if (type == 127) break;                    // end-of-table
        if (type == 17 && flen >= 0x17) {          // Memory Device
            uint16_t speed = static_cast<uint16_t>(p[0x15] | (p[0x16] << 8));
            if (speed != 0 && speed != 0xFFFF) return speed;
        }
        const BYTE* s = p + flen;                  // skip strings (double-NUL terminated)
        while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) ++s;
        p = s + 2;
    }
    return 0;
}

// Physical drive model backing a volume, via IOCTL (no WMI/COM).
static std::string volumeModel(const wchar_t* mount) {
    std::wstring vol = L"\\\\.\\";
    vol += mount;
    if (!vol.empty() && vol.back() == L'\\') vol.pop_back();
    HANDLE h = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string model;
    VOLUME_DISK_EXTENTS ext{}; DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                        &ext, sizeof(ext), &ret, nullptr) && ext.NumberOfDiskExtents > 0) {
        DWORD diskNum = ext.Extents[0].DiskNumber;
        CloseHandle(h);
        std::wstring pd = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNum);
        HANDLE hd = CreateFileW(pd.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (hd != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY q{}; q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
            BYTE out[1024]{}; DWORD rb = 0;
            if (DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), out, sizeof(out), &rb, nullptr)) {
                auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(out);
                if (desc->ProductIdOffset && desc->ProductIdOffset < rb) {
                    model = reinterpret_cast<const char*>(out) + desc->ProductIdOffset;
                    while (!model.empty() && (model.back() == ' ' || model.back() == '\0')) model.pop_back();
                    size_t b = model.find_first_not_of(' ');
                    if (b != std::string::npos) model = model.substr(b);
                }
            }
            CloseHandle(hd);
        }
        return model;
    }
    CloseHandle(h);
    return model;
}

namespace {
struct StaticHw {
    std::string cpuModel, osName;
    int cpuMhz = 0, cpuPhysical = 0, memSpeedMhz = 0;
};
const StaticHw& staticHw() {
    static StaticHw hw = [] {
        StaticHw h;
        const wchar_t* cpuKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
        h.cpuModel = regString(HKEY_LOCAL_MACHINE, cpuKey, L"ProcessorNameString");
        size_t b = h.cpuModel.find_first_not_of(' ');
        if (b != std::string::npos) h.cpuModel = h.cpuModel.substr(b);
        h.cpuMhz = static_cast<int>(regDword(HKEY_LOCAL_MACHINE, cpuKey, L"~MHz"));
        h.cpuPhysical = physicalCores();
        h.memSpeedMhz = ramSpeedMhz();
        h.osName = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
        if (h.osName.empty()) h.osName = "Windows";
        return h;
    }();
    return hw;
}
}  // namespace

// ─────────────────────────── POSIX (Linux / others) ───────────────────────────
#else
#include <fstream>
#include <sstream>
#include <set>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#if defined(__linux__)
// Read /etc/os-release into key→value (values unquoted).
static std::map<std::string, std::string> osRelease() {
    std::map<std::string, std::string> m;
    std::ifstream f("/etc/os-release"); std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        m[k] = v;
    }
    return m;
}
// First matching field from /proc/cpuinfo (e.g. "model name").
static std::string cpuinfoField(const char* key) {
    std::ifstream f("/proc/cpuinfo"); std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos) {
                std::string v = line.substr(c + 1);
                size_t b = v.find_first_not_of(" \t");
                return b == std::string::npos ? std::string() : v.substr(b);
            }
        }
    }
    return {};
}
static int linuxPhysicalCores() {
    std::ifstream f("/proc/cpuinfo"); std::string line;
    std::set<std::pair<int, int>> cores; int phys = -1, core = -1;
    while (std::getline(f, line)) {
        if (line.rfind("physical id", 0) == 0) { auto c = line.find(':'); if (c != std::string::npos) phys = atoi(line.c_str() + c + 1); }
        else if (line.rfind("core id", 0) == 0) { auto c = line.find(':'); if (c != std::string::npos) core = atoi(line.c_str() + c + 1); }
        else if (line.empty()) { if (phys >= 0 && core >= 0) cores.insert({phys, core}); phys = core = -1; }
    }
    if (phys >= 0 && core >= 0) cores.insert({phys, core});
    return static_cast<int>(cores.size());
}
// Aggregate CPU busy% from two /proc/stat samples.
static double linuxCpuLoad() {
    auto read = [](uint64_t& idle, uint64_t& total) {
        std::ifstream f("/proc/stat"); std::string cpu;
        uint64_t u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0, st = 0;
        f >> cpu >> u >> n >> s >> i >> w >> irq >> sirq >> st;
        idle = i + w; total = u + n + s + i + w + irq + sirq + st;
    };
    uint64_t i0, t0, i1, t1; read(i0, t0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    read(i1, t1);
    uint64_t dt = t1 - t0, di = i1 - i0;
    if (dt == 0) return -1.0;
    return 100.0 * static_cast<double>(dt - di) / static_cast<double>(dt);
}
#endif  // __linux__
#endif  // platform helpers

namespace dice::sysinfo {

SysInfo gather() {
    SysInfo s;
    s.cpuCores = static_cast<int>(std::thread::hardware_concurrency());

#if defined(_WIN32)
    const StaticHw& hw = staticHw();
    s.os = hw.osName; s.osId = "windows";
    s.cpuModel = hw.cpuModel; s.cpuMhz = hw.cpuMhz; s.cpuPhysical = hw.cpuPhysical; s.memSpeedMhz = hw.memSpeedMhz;

    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.memTotalMB = ms.ullTotalPhys / (1024ull * 1024ull);
        uint64_t availMB = ms.ullAvailPhys / (1024ull * 1024ull);
        s.memUsedMB = (s.memTotalMB > availMB) ? (s.memTotalMB - availMB) : 0;
        s.memLoadPct = static_cast<int>(ms.dwMemoryLoad);
    }
    FILETIME idle0, kern0, user0, idle1, kern1, user1;
    if (GetSystemTimes(&idle0, &kern0, &user0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (GetSystemTimes(&idle1, &kern1, &user1)) {
            uint64_t idle = ftToU64(idle1) - ftToU64(idle0);
            uint64_t kern = ftToU64(kern1) - ftToU64(kern0);
            uint64_t user = ftToU64(user1) - ftToU64(user0);
            uint64_t total = kern + user;
            if (total > 0) s.cpuLoadPct = 100.0 * static_cast<double>(total - idle) / static_cast<double>(total);
        }
    }
    PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        s.procMemMB = pmc.WorkingSetSize / (1024ull * 1024ull);

    static std::map<std::string, std::string> modelCache;
    wchar_t drives[512]; DWORD n = GetLogicalDriveStringsW(511, drives);
    for (wchar_t* d = drives; *d && (d - drives) < (int)n; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        DiskInfo di; di.mount = narrow(d);
        ULARGE_INTEGER freeToCaller, total, totalFree;
        if (!GetDiskFreeSpaceExW(d, &freeToCaller, &total, &totalFree) || total.QuadPart == 0) continue;
        const double GB = 1024.0 * 1024.0 * 1024.0;
        di.totalGB = static_cast<uint64_t>(total.QuadPart / GB);
        uint64_t usedBytes = (total.QuadPart > totalFree.QuadPart) ? (total.QuadPart - totalFree.QuadPart) : 0;
        di.usedGB = static_cast<uint64_t>(usedBytes / GB);
        di.loadPct = static_cast<int>(100.0 * static_cast<double>(usedBytes) / static_cast<double>(total.QuadPart));
        wchar_t label[MAX_PATH + 1] = {0}, fs[64] = {0};
        if (GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, fs, 64)) {
            di.label = narrow(label); di.fs = narrow(fs);
        }
        auto it = modelCache.find(di.mount);
        if (it == modelCache.end()) it = modelCache.emplace(di.mount, volumeModel(d)).first;
        di.model = it->second;
        s.disks.push_back(std::move(di));
    }

#elif defined(__linux__)
    auto osr = osRelease();
    s.os = osr.count("PRETTY_NAME") ? osr["PRETTY_NAME"] : "Linux";
    std::string id = osr.count("ID") ? osr["ID"] : "linux";
    if (id == "ubuntu" || id == "debian" || id == "centos" || id == "rocky") s.osId = id;
    else if (id == "rhel" || id == "fedora" || id == "almalinux") s.osId = "centos";
    else s.osId = "linux";
    s.cpuModel = cpuinfoField("model name");
    s.cpuPhysical = linuxPhysicalCores();
    { std::string mhz = cpuinfoField("cpu MHz"); if (!mhz.empty()) s.cpuMhz = static_cast<int>(atof(mhz.c_str())); }
    s.cpuLoadPct = linuxCpuLoad();
    { std::ifstream f("/proc/meminfo"); std::string k, unit; long v = 0, total = 0, avail = 0;
      while (f >> k >> v >> unit) { if (k == "MemTotal:") total = v; else if (k == "MemAvailable:") avail = v; }
      s.memTotalMB = total / 1024;
      long used = total > avail ? total - avail : 0;
      s.memUsedMB = used / 1024;
      if (total > 0) s.memLoadPct = static_cast<int>(100.0 * used / total);
    }
    { std::ifstream f("/proc/self/statm"); long sz = 0, rss = 0; if (f >> sz >> rss) {
        long pageKB = sysconf(_SC_PAGESIZE) / 1024; s.procMemMB = (rss * pageKB) / 1024; } }
    { std::ifstream f("/proc/mounts"); std::string dev, mnt, fstype, rest; std::set<std::string> seen;
      while (f >> dev >> mnt >> fstype) {
          std::getline(f, rest);
          if (dev.rfind("/dev/", 0) != 0 || fstype == "squashfs") continue;
          if (!seen.insert(mnt).second) continue;
          struct statvfs vfs;
          if (statvfs(mnt.c_str(), &vfs) != 0 || vfs.f_blocks == 0) continue;
          const double GB = 1024.0 * 1024.0 * 1024.0;
          uint64_t blk = vfs.f_frsize, total = static_cast<uint64_t>(vfs.f_blocks) * blk, freeb = static_cast<uint64_t>(vfs.f_bavail) * blk;
          if (total == 0) continue;
          DiskInfo di; di.mount = mnt; di.fs = fstype;
          di.totalGB = static_cast<uint64_t>(total / GB);
          uint64_t usedb = total > freeb ? total - freeb : 0;
          di.usedGB = static_cast<uint64_t>(usedb / GB);
          di.loadPct = static_cast<int>(100.0 * static_cast<double>(usedb) / static_cast<double>(total));
          s.disks.push_back(std::move(di));
      }
    }

#else   // generic POSIX (macOS / BSD): OS name + root disk usage
    struct utsname un; if (uname(&un) == 0) s.os = un.sysname;
#if defined(__APPLE__)
    s.os = "macOS"; s.osId = "macos";
#else
    s.osId = "linux";
#endif
    { struct statvfs vfs; if (statvfs("/", &vfs) == 0 && vfs.f_blocks) {
        const double GB = 1024.0 * 1024.0 * 1024.0;
        uint64_t blk = vfs.f_frsize, total = static_cast<uint64_t>(vfs.f_blocks) * blk, freeb = static_cast<uint64_t>(vfs.f_bavail) * blk;
        DiskInfo di; di.mount = "/"; di.totalGB = static_cast<uint64_t>(total / GB);
        uint64_t usedb = total > freeb ? total - freeb : 0;
        di.usedGB = static_cast<uint64_t>(usedb / GB);
        di.loadPct = total ? static_cast<int>(100.0 * static_cast<double>(usedb) / static_cast<double>(total)) : 0;
        s.disks.push_back(std::move(di));
    } }
#endif
    return s;
}

}  // namespace dice::sysinfo
