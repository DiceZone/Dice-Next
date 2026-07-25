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
    oss << std::put_time(std::gmtime(&time_t_val), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << millis.count() << 'Z';
    return oss.str();
}

inline std::string nowIso8601() {
    return timestampToIso8601(std::chrono::system_clock::now());
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
inline long getStartupEpoch() {
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
