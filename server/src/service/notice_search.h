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
    auto searchable = rule;
    for (auto &channel : searchable["channels"])
        if (channel.is_object())
            channel.erase("url");
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    return lower(searchable.dump()).find(lower(query)) != std::string::npos;
}
} // namespace dice::notice_rules
