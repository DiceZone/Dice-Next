#pragma once

#include "config_manager.h"

#include <nlohmann/json.hpp>
#include <string>

namespace dice::scoped_settings {

using json = nlohmann::json;

inline json objectOrEmpty(const json& value) {
    return value.is_object() ? value : json::object();
}

inline void mergeObject(json& destination, const json& source) {
    if (!source.is_object()) return;
    for (auto it = source.begin(); it != source.end(); ++it)
        destination[it.key()] = it.value();
}

inline json rawSection(const json& all, const std::string& scope,
                       const std::string& target, const std::string& section) {
    if (scope == "global")
        return objectOrEmpty(all.value(section, json::object()));
    if (target.empty()) return json::object();
    const json dice = objectOrEmpty(all.value("dice", json::object()));
    const json roots = objectOrEmpty(dice.value("scoped_overrides", json::object()));
    const json group = objectOrEmpty(roots.value(scope, json::object()));
    const json entry = objectOrEmpty(group.value(target, json::object()));
    return objectOrEmpty(entry.value(section, json::object()));
}

inline json resolveSection(const json& all, const std::string& section,
                           const std::string& platform, const std::string& adapterId) {
    json result = objectOrEmpty(all.value(section, json::object()));
    mergeObject(result, rawSection(all, "adapter", platform, section));
    mergeObject(result, rawSection(all, "account", adapterId, section));
    return result;
}

inline std::string sourceFor(const json& all, const std::string& section,
                             const std::string& key, const std::string& platform,
                             const std::string& adapterId) {
    const json account = rawSection(all, "account", adapterId, section);
    if (account.contains(key)) return "account";
    const json adapter = rawSection(all, "adapter", platform, section);
    if (adapter.contains(key)) return "adapter";
    return "global";
}

inline bool eraseTarget(ConfigManager& cfg, const std::string& scope,
                        const std::string& target) {
    if ((scope != "adapter" && scope != "account") || target.empty()) return false;
    json roots = cfg.get<json>("dice/scoped_overrides", json::object());
    if (!roots.is_object()) roots = json::object();
    if (roots.contains(scope) && roots[scope].is_object()) roots[scope].erase(target);
    cfg.set<json>("dice/scoped_overrides", roots);
    return true;
}

inline bool setSection(ConfigManager& cfg, const std::string& scope,
                       const std::string& target, const std::string& section,
                       const json& values, const json& clearKeys = json::array()) {
    if (scope != "adapter" && scope != "account") return false;
    if (target.empty()) return false;

    json roots = cfg.get<json>("dice/scoped_overrides", json::object());
    if (!roots.is_object()) roots = json::object();
    json& sectionValues = roots[scope][target][section];
    if (!sectionValues.is_object()) sectionValues = json::object();
    if (values.is_object()) {
        for (auto it = values.begin(); it != values.end(); ++it)
            sectionValues[it.key()] = it.value();
    }
    if (clearKeys.is_array()) {
        for (const auto& key : clearKeys)
            if (key.is_string()) sectionValues.erase(key.get<std::string>());
    }
    if (sectionValues.empty()) roots[scope][target].erase(section);
    if (roots[scope][target].empty()) roots[scope].erase(target);
    cfg.set<json>("dice/scoped_overrides", roots);
    return true;
}

} // namespace dice::scoped_settings