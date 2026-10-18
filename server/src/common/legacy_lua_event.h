#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

namespace dice {
inline std::optional<int64_t> positiveQQId(const std::string& text) {
    int64_t value = 0;
    if (text.empty()) return std::nullopt;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value <= 0) return std::nullopt;
    return value;
}
struct LuaEventOrigin {
    std::string platform, adapterId, groupId, userId, pluginId;
    bool virtualEvent = false;
};
struct LegacyLuaGroupBan { std::string groupId, userId; int minutes = 0; };
inline std::optional<LegacyLuaGroupBan> parseLegacyLuaGroupBan(const std::string& text) {
    std::istringstream input(text);
    std::string command, group, action, user, duration, extra;
    if (!(input >> command >> group >> action >> user >> duration) || (input >> extra)
        || command != ".group" || action != "ban" || !positiveQQId(group) || !positiveQQId(user)) return std::nullopt;
    int minutes = -1;
    const auto result = std::from_chars(duration.data(), duration.data() + duration.size(), minutes);
    // Legacy duration is minutes. OneBot permits at most 30 days.
    if (result.ec != std::errc{} || result.ptr != duration.data() + duration.size() || minutes < 0 || minutes > 43200)
        return std::nullopt;
    return LegacyLuaGroupBan{group, user, minutes};
}
}
