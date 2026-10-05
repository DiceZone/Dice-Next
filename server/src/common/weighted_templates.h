#pragma once
#include "sample_template.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace dice::weighted_templates {
using json = nlohmann::json;
inline constexpr const char* prefix = "@dicenext:weighted:v1\n";

inline json validate(const json& items) {
    if (!items.is_array() || items.empty() || items.size() > 256)
        throw std::invalid_argument("Expected 1 to 256 reply variants");
    size_t total = 0, bytes = 0;
    json out = json::array();
    for (const auto& item : items) {
        if (!item.is_object() || !item.contains("text") || !item["text"].is_string()
            || !item.contains("weight") || !item["weight"].is_number_integer())
            throw std::invalid_argument("Each variant needs text and an integer weight");
        const auto weight = item["weight"].get<int64_t>();
        const auto text = item["text"].get<std::string>();
        if (weight < 0 || weight > 999999 || text.rfind(prefix, 0) == 0)
            throw std::invalid_argument("Invalid variant weight or nested variant container");
        bytes += text.size(); total += static_cast<size_t>(weight);
        out.push_back({{"text", text}, {"weight", weight}});
    }
    if (!total || bytes > 65536) throw std::invalid_argument("Reply weights must total above zero; text limit is 64 KiB");
    return out;
}
inline json decode(const std::string& value) {
    if (value.rfind(prefix, 0) != 0) return json::array();
    return validate(json::parse(value.substr(std::char_traits<char>::length(prefix))));
}
inline std::string encode(const json& items) { return std::string(prefix) + validate(items).dump(); }
inline std::string apiValue(const json& body) {
    if (body.contains("variants")) return encode(body["variants"]);
    const auto value = body.value("value", std::string());
    decode(value); // Validate native values retained by imports and older editors.
    return value;
}
inline json apiRecord(const std::string& value, const std::string& format) {
    json record{{"value", value}, {"format", format}};
    const auto items = decode(value);
    if (!items.empty()) {
        for (const auto& item : items) {
            if (item["weight"].get<size_t>() > 0) { record["value"] = item["text"]; break; }
        }
        record["variants"] = items;
    }
    return record;
}
template<class Choose>
size_t pick(const json& items, Choose& choose) {
    size_t total = 0;
    for (const auto& item : items) total += item["weight"].get<size_t>();
    size_t ticket = choose(total);
    for (size_t i = 0; i < items.size(); ++i) {
        const size_t weight = items[i]["weight"].get<size_t>();
        if (ticket < weight) return i;
        ticket -= weight;
    }
    throw std::out_of_range("Invalid weighted choice");
}
} // namespace dice::weighted_templates
