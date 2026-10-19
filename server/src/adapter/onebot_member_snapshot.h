#pragma once
#include "../common/legacy_lua_event.h"
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_map>
#include <vector>

namespace dice::onebot_members {
struct Snapshot { std::vector<std::string> ids; std::unordered_map<std::string, std::string> names; };
inline std::string id(const nlohmann::json& row) {
    if (!row.is_object() || !row.contains("user_id")) return {};
    const auto& value = row["user_id"];
    const auto text = value.is_string() ? value.get<std::string>() : value.is_number_integer() ? value.dump() : std::string();
    return positiveQQId(text) ? text : std::string();
}
inline std::vector<std::string> ids(const nlohmann::json& rows) {
    std::vector<std::string> result; std::set<std::string> seen;
    if (!rows.is_array() || rows.size() > 10000) return result;
    for (const auto& row : rows) { const auto user = id(row); if (!user.empty() && seen.insert(user).second) result.push_back(user); }
    return result;
}
inline std::string name(const nlohmann::json& rows, const std::string& user) {
    if (!rows.is_array() || rows.size() > 10000) return {};
    for (const auto& row : rows) if (id(row) == user) {
        for (const auto key : {"card", "nickname"})
            if (row.contains(key) && row[key].is_string() && !row[key].get_ref<const std::string&>().empty())
                return row[key].get<std::string>();
        break;
    }
    return {};
}
inline Snapshot snapshot(const nlohmann::json& rows) {
    Snapshot result;
    if (!rows.is_array() || rows.size() > 10000) return result;
    for (const auto& row : rows) {
        const auto user = id(row);
        if (user.empty() || result.names.count(user)) continue;
        result.ids.push_back(user);
        std::string display;
        for (const auto key : {"card", "nickname"})
            if (row.contains(key) && row[key].is_string() && !row[key].get_ref<const std::string&>().empty()) {
                display = row[key].get<std::string>(); break;
            }
        result.names[user] = std::move(display);
    }
    return result;
}
} // namespace dice::onebot_members
