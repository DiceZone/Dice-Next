#pragma once

#include <ctime>
#include <cstdint>
#include <optional>
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace dice::auto_cleanup {
// Cleanup must not treat unknown, malformed or future timestamps as inactivity.
// Stored activity uses UTC ISO-8601; reject normalized dates such as February 31.
inline std::optional<std::time_t> utcTime(const std::string& iso, std::time_t now = std::time(nullptr)) {
    if (iso.size() < 19 || iso[4] != '-' || iso[7] != '-'
        || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':') return std::nullopt;
    for (int i = 0; i < 19; ++i)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (iso[i] < '0' || iso[i] > '9')) return std::nullopt;
    size_t suffix = 19;
    if (suffix < iso.size() && iso[suffix] == '.') {
        const auto start = ++suffix;
        while (suffix < iso.size() && iso[suffix] >= '0' && iso[suffix] <= '9') ++suffix;
        if (suffix == start) return std::nullopt;
    }
    if (suffix < iso.size() && iso[suffix] == 'Z') ++suffix;
    if (suffix != iso.size()) return std::nullopt;
    std::tm tm{};
    tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(iso.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(iso.substr(8, 2));
    tm.tm_hour = std::stoi(iso.substr(11, 2));
    tm.tm_min = std::stoi(iso.substr(14, 2));
    tm.tm_sec = std::stoi(iso.substr(17, 2));
    const auto requested = tm;
#ifdef _WIN32
    const auto at = _mkgmtime(&tm);
#else
    const auto at = timegm(&tm);
#endif
    if (at <= 0 || at > now || tm.tm_year != requested.tm_year || tm.tm_mon != requested.tm_mon
        || tm.tm_mday != requested.tm_mday || tm.tm_hour != requested.tm_hour
        || tm.tm_min != requested.tm_min || tm.tm_sec != requested.tm_sec) return std::nullopt;
    return at;
}

inline bool inactive(const std::string& iso, int days, std::time_t now = std::time(nullptr)) {
    if (days <= 0) return false;
    const auto at = utcTime(iso, now);
    if (!at) return false;
    return static_cast<int64_t>(now) - static_cast<int64_t>(*at) >= static_cast<int64_t>(days) * 86400;
}

inline int idleDays(const std::string& iso, std::time_t now = std::time(nullptr)) {
    const auto at = utcTime(iso, now);
    if (!at) return 0;
    return static_cast<int>((static_cast<int64_t>(now) - static_cast<int64_t>(*at)) / 86400);
}

// Validate the entire request before changing any live configuration.
inline nlohmann::json patched(nlohmann::json dice, const nlohmann::json& request) {
    if (!request.is_object()) throw std::invalid_argument("清理设置必须为对象");
    struct Field { const char* input; const char* key; int max; };
    for (const auto& field : {Field{"days", "friend_clean_days", 3650},
                             Field{"groupDays", "group_clean_days", 3650},
                             Field{"userRecordDays", "user_record_days", 3650},
                             Field{"groupRecordDays", "group_record_days", 3650},
                             Field{"groupLimit", "group_clear_limit", 10000},
                             Field{"maxGroupSize", "max_group_size", 1000000}}) {
        if (!request.contains(field.input)) continue;
        const auto& value = request[field.input];
        if (!value.is_number_integer() || value < 0 || value > field.max)
            throw std::invalid_argument(std::string(field.input) + " 必须是 0–" + std::to_string(field.max) + " 之间的整数");
        dice[field.key] = value.get<int>();
    }
    return dice;
}
} // namespace dice::auto_cleanup
