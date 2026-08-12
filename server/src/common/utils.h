#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <limits>

namespace dice {
namespace utils {

// ─── UUID v4 Generation ──────────────────────────────────────

inline std::string generateUuidV4() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    // Generate 16 random bytes
    std::vector<uint8_t> bytes(16);
    for (int i = 0; i < 2; ++i) {
        uint64_t val = dis(gen);
        for (int j = 0; j < 8; ++j) {
            bytes[i * 8 + j] = static_cast<uint8_t>((val >> (j * 8)) & 0xFF);
        }
    }

    // Set version to 4 (UUID v4)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set variant to RFC 4122
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as standard UUID string: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            oss << '-';
        }
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }

    return oss.str();
}

// ─── ISO 8601 UTC Timestamp ──────────────────────────────────

inline std::string timestampToIso8601(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    std::ostringstream oss;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time_t_val);
#else
    gmtime_r(&time_t_val, &tm);
#endif
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << millis.count() << 'Z';
    return oss.str();
}

inline std::string nowIso8601() {
    return timestampToIso8601(std::chrono::system_clock::now());
}

// ─── Timezone-aware time helpers ─────────────────────────────
// 服务器统一时区（相对 UTC 的固定分钟偏移，东为正）。INT_MIN = 跟随系统
// 本地时区（默认）。外部协议（心跳/云黑/Discord 等）仍使用 UTC，展示与
// 上传给用户的日志/审计/备份名等统一走这里。

inline int& timezoneOffsetMinutes() {
    static int value = (std::numeric_limits<int>::min)();
    return value;
}
inline void setTimezoneOffset(int minutes) { timezoneOffsetMinutes() = minutes; }

/// 系统本地时区相对 UTC 的分钟偏移（含 DST，按当前时刻估算）。
inline int systemLocalOffsetMinutes() {
    const std::time_t t = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    return static_cast<int>((t - std::mktime(&utc)) / 60);
}

inline int effectiveTimezoneOffsetMinutes() {
    const int v = timezoneOffsetMinutes();
    return v == (std::numeric_limits<int>::min)() ? systemLocalOffsetMinutes() : v;
}

/// epoch → 配置时区下的 broken-down time（固定偏移，不做 DST 换算）。
inline std::tm timezoneTm(std::time_t t) {
    const std::time_t shifted = t + static_cast<std::time_t>(effectiveTimezoneOffsetMinutes()) * 60;
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &shifted);
#else
    gmtime_r(&shifted, &out);
#endif
    return out;
}

/// 按配置时区格式化一个 epoch。
inline std::string formatTimeInTimezone(std::time_t t, const char* fmt) {
    const std::tm tm = timezoneTm(t);
    char buf[64]{};
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

/// 当前配置时区的墙上时间 ISO（无 Z）。
inline std::string nowLocalIso() {
    return formatTimeInTimezone(std::time(nullptr), "%Y-%m-%dT%H:%M:%S");
}

/// 解析 "YYYY-MM-DDTHH:MM:SS[.mmm][Z]"（按 UTC）→ epoch；失败返回 0。
inline std::time_t parseIsoUtcToEpoch(const std::string& iso) {
    if (iso.size() < 19 || iso[4] != '-' || iso[10] != 'T') return 0;
    std::tm tm{};
    try {
        tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(iso.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(iso.substr(8, 2));
        tm.tm_hour = std::stoi(iso.substr(11, 2));
        tm.tm_min  = std::stoi(iso.substr(14, 2));
        tm.tm_sec  = std::stoi(iso.substr(17, 2));
    } catch (...) { return 0; }
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

// ─── String Utilities ────────────────────────────────────────

inline std::string trim(const std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char c) { return std::isspace(c); }).base();
    return (start < end) ? std::string(start, end) : std::string();
}

inline std::string toLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

inline std::string toUpper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return result;
}

inline bool startsWith(const std::string& str, const std::string& prefix) {
    if (str.size() < prefix.size()) return false;
    return str.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string& str, const std::string& suffix) {
    if (str.size() < suffix.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;
    while (std::getline(iss, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

inline std::string join(const std::vector<std::string>& parts, const std::string& delimiter) {
    if (parts.empty()) return "";
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) oss << delimiter;
        oss << parts[i];
    }
    return oss.str();
}

inline std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    if (from.empty()) return str;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}

// ─── Random Utilities ────────────────────────────────────────

inline int randomInt(int min, int max) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(min, max);
    return dis(gen);
}

// ─── Path Utilities ──────────────────────────────────────────

inline bool pathExists(const std::string& path);

// ─── Startup Timestamp (for uptime calculation) ──────────────

inline std::chrono::system_clock::time_point g_startupTime = std::chrono::system_clock::now();

inline void setStartupEpoch() { g_startupTime = std::chrono::system_clock::now(); }
inline long long getStartupEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        g_startupTime.time_since_epoch()).count();
}

inline std::string dirName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

inline std::string baseName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

}  // namespace utils
}  // namespace dice
