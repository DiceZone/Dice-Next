#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>

namespace dice::legacy_conf {
// Only ambiguous strings and structured values need a typed envelope. Ordinary
// scalar configuration remains compatible with existing native conf readers.
inline constexpr const char* prefix = "@dice:legacy-json:";
inline constexpr const char* typeKey = "@dice:legacy:type";
inline nlohmann::json setValue(const nlohmann::json& values) {
    return {{typeKey, "set"}, {"values", values}};
}
inline bool isSet(const nlohmann::json& value) {
    return value.is_object() && value.size() == 2 && value.contains(typeKey)
        && value[typeKey] == "set" && value.contains("values") && value["values"].is_array();
}
inline bool isFunction(const nlohmann::json& value) {
    return value.is_object() && value.size() == 2 && value.contains(typeKey)
        && value[typeKey] == "function" && value.contains("byteLength") && value["byteLength"].is_number_unsigned();
}
inline std::string encode(const nlohmann::json& value) {
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (!text.empty() && text != "true" && text != "false"
            && text.rfind(prefix, 0) != 0) {
            // Match Lua's historical strtod reader as well as JSON numbers:
            // +1, 01 and whitespace-prefixed numbers are still authored text.
            char* end = nullptr;
            std::strtod(text.c_str(), &end);
            if (!end || end == text.c_str() || *end != '\0') return text;
        }
    } else if (value.is_boolean() || value.is_number()) return value.dump();
    return std::string(prefix) + value.dump();
}
inline bool decode(const std::string& text, nlohmann::json& value) {
    const std::string marker(prefix);
    if (text.rfind(marker, 0) != 0) return false;
    value = nlohmann::json::parse(text.substr(marker.size()), nullptr, false);
    return !value.is_discarded();
}
} // namespace dice::legacy_conf
