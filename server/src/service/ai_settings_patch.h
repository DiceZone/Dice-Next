#pragma once
#include <nlohmann/json.hpp>
#include <mutex>
#include <stdexcept>

namespace dice::ai {
inline std::mutex& settingsMutex() { static std::mutex mutex; return mutex; }
// A switch submits just its leaf. Arrays deliberately replace as a whole.
inline nlohmann::json mergeSettings(nlohmann::json current, const nlohmann::json& patch) {
    if (!patch.is_object()) throw std::invalid_argument("AI settings must be an object");
    if (!current.is_object()) current = nlohmann::json::object();
    current.merge_patch(patch);
    return current;
}
}
