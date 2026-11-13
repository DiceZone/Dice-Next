#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include "ai_model_options.h"

namespace dice::aivision {

// URL forwarding is retired, including for old saved configurations. The bot
// downloads images, validates their size and format, then uploads image bytes.
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
             {"max_images", limit}, {"max_image_kb", size}, {"pass_url", false},
             {"max_tokens", ai::boundedOption(source,"max_tokens",ai::boundedOption(previous,"max_tokens",2048,1,393216),1,393216)},
             {"timeout_sec", ai::boundedOption(source,"timeout_sec",ai::boundedOption(previous,"timeout_sec",60,5,300),5,300)},
             {"thinking", ai::choiceOption(source,"thinking",{"auto","enabled","disabled"},"auto")}};
}

} // namespace dice::aivision
