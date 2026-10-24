#pragma once
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>

namespace dice::notice_rules {
// Match visible metadata only. Otherwise a read-only administrator could guess
// a redacted Webhook credential by probing the number of search results.
inline bool matchesSearch(const nlohmann::json &rule, std::string query) {
    if (query.empty())
        return true;
    nlohmann::json searchable;
    for (const auto* key : {"name", "source", "subscription", "level_mask", "events", "subscriptions"})
        if (rule.contains(key)) searchable[key] = rule[key];
    searchable["channels"] = nlohmann::json::array();
    for (const auto& channel : rule.value("channels", nlohmann::json::array())) {
        nlohmann::json visible;
        for (const auto* key : {"kind", "platform", "chat_id", "is_group", "name", "display_name", "delivery", "adapter_ids", "to", "profile"})
            if (channel.contains(key)) visible[key] = channel[key];
        searchable["channels"].push_back(std::move(visible));
    }
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    return lower(searchable.dump()).find(lower(query)) != std::string::npos;
}
} // namespace dice::notice_rules
