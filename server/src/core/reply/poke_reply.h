#pragma once
#include "reply_definition.h"
#include "../../config/scoped_settings.h"

namespace dice::poke_reply {

// A closer legacy text/command override must not be shadowed by an inherited
// new reply definition. Enabled-only overrides still inherit the reply itself.
inline json resolveEvents(const json& all, const std::string& platform, const std::string& adapterId) {
    json events = scoped_settings::rawSection(all, "global", "", "events");
    for (const auto& scope : {std::string("adapter"), std::string("account")}) {
        const auto local = scoped_settings::rawSection(all, scope, scope == "adapter" ? platform : adapterId, "events");
        if (!local.contains("poke_reply") && (local.contains("poke") || local.contains("poke_command"))) events.erase("poke_reply");
        scoped_settings::mergeObject(events, local);
    }
    return events;
}

inline json definition(const json& events, const std::string& defaultText) {
    json body = events.value("poke_reply", json());
    if (!body.is_object()) {
        const auto text = events.value("poke", std::string());
        body = {{"results", json::array({text.empty() ? defaultText : text})},
                {"command", events.value("poke_command", std::string())}};
    }
    body["enabled"] = events.value("poke_enabled", true) && body.value("enabled", true);
    return body;
}

} // namespace dice::poke_reply
