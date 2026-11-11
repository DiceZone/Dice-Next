#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace dice::aivision {

// Keep the API's read/write schema in one place. In particular, an explicit
// false is a setting, not a missing value. Older clients which omit pass_url
// must not silently re-enable URL forwarding on an existing installation.
inline nlohmann::json normalizeSettings(const nlohmann::json& input,
                                       const nlohmann::json& previous = nlohmann::json::object()) {
    using J = nlohmann::json;
    const J source = input.is_object() ? input : J::object();
    int limit = source.value("max_images", 2);
    if (limit < 1) limit = 1;
    if (limit > 4) limit = 4;
    int size = source.value("max_image_kb", previous.is_object() ? previous.value("max_image_kb", 1024) : 1024);
    size = size < 1 ? 1 : (size > 8192 ? 8192 : size);
    return J{{"enabled", source.value("enabled", false)},
             {"prompt", source.value("prompt", std::string())},
             {"max_images", limit}, {"max_image_kb", size}, {"pass_url", false}};
}

} // namespace dice::aivision
