#pragma once
#include "notice_webhook.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dice::notice_rules {
using Json = nlohmann::json;
inline std::recursive_mutex &configMutex() {
    static std::recursive_mutex m;
    return m;
}
inline const std::vector<std::pair<std::string, int>> &catalog() {
    static const std::vector<std::pair<std::string, int>> value{
        {"trust", 1},           {"alias", 1},         {"friend_req", 2},
        {"friend_add", 2},      {"group_invite", 2},  {"leave", 2},
        {"dismiss", 2},         {"group_left", 2},    {"blacklist_leave", 2},
        {"nonfriend_leave", 2}, {"keyword_leave", 2}, {"schedule", 2},
        {"global", 2},          {"censor", 2},        {"update_available", 2},
        {"update_result", 2},   {"admin", 4},         {"blacklist", 4},
        {"error", 8},           {"update_error", 8},
        {"group_joined", 2},    {"group_request", 2}, {"group_ban", 2},
        {"group_admin", 2},     {"group_size_leave", 2}, {"group_auto_clean", 2},
        {"usergroup_leave", 2}, {"leave_error", 8},
        {"group_control", 2},   {"schedule_manage", 1}, {"notice_manage", 1}};
    return value;
}
inline bool clean(const std::string &s, size_t max = 1024) {
    return s.size() <= max &&
           std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
inline bool mailbox(const std::string &s) {
    const auto at = s.find('@');
    return clean(s, 254) && at != std::string::npos && at > 0 && at + 1 < s.size() &&
           s.find('@', at + 1) == std::string::npos && s.find_first_of(" <>\"\\,;:") == std::string::npos;
}
inline bool webhook(const std::string &s) {
    return notice_webhook::validUrl(s);
}
// Destination identity is independent of subscriptions and sending accounts.
inline std::string destinationKey(const Json& channel) {
    const auto kind = channel.value("kind", std::string());
    if (kind == "chat") return Json::array({kind, channel.value("platform", std::string()), channel.value("chat_id", std::string()), channel.value("is_group", true)}).dump();
    auto address = channel.value(kind == "smtp" ? "to" : "url", std::string());
    if (kind == "smtp") std::transform(address.begin(), address.end(), address.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return Json::array({kind, address}).dump();
}
inline std::string legacyId(const Json &w) {
    const auto text = Json::array({w.value("platform", std::string()), w.value("adapter_id", std::string()),
                                   w.value("chat_id", std::string()), w.value("is_group", true)})
                          .dump();
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "legacy-window-" << std::hex << hash;
    return out.str();
}
inline Json strings(const Json &value, size_t max = 128) {
    if (!value.is_array() || value.size() > max)
        throw std::invalid_argument("Invalid selection");
    Json out = Json::array();
    std::set<std::string> seen;
    for (const auto &item : value) {
        if (!item.is_string() || item.get_ref<const std::string &>().empty() ||
            !clean(item.get_ref<const std::string &>(), 128))
            throw std::invalid_argument("Invalid selection item");
        if (seen.insert(item.get<std::string>()).second)
            out.push_back(item);
    }
    return out;
}
inline Json normalize(const Json &in) {
    if (!in.is_object())
        throw std::invalid_argument("Invalid notification rule");
    const auto name = in.value("name", std::string());
    if (!clean(name, 240))
        throw std::invalid_argument("Invalid name");
    const auto mode = in.value("subscription", std::string("levels"));
    if (mode != "levels" && mode != "events")
        throw std::invalid_argument("Invalid subscription mode");
    const int mask = in.value("level_mask", 15);
    if (mask < 0 || mask > 15)
        throw std::invalid_argument("Invalid level mask");
    Json events = strings(in.value("events", Json::array()));
    for (const auto &event : events)
        if (std::none_of(catalog().begin(), catalog().end(),
                         [&](const auto &c) { return c.first == event.get<std::string>(); }))
            throw std::invalid_argument("Unknown notification event");
    const auto source = in.value("source", Json::object());
    const auto scope = source.value("mode", std::string("all"));
    if (scope != "all" && scope != "platforms" && scope != "accounts")
        throw std::invalid_argument("Invalid source scope");
    Json platforms = strings(source.value("platforms", Json::array())),
         ids = strings(source.value("adapter_ids", Json::array()));
    if ((scope == "platforms" && platforms.empty()) || (scope == "accounts" && ids.empty()))
        throw std::invalid_argument("Choose notification sources");
    const auto input = in.value("channels", Json::array());
    if (!input.is_array() || input.empty() || input.size() > 128)
        throw std::invalid_argument("Choose notification destinations");
    Json channels = Json::array();
    std::set<std::string> seen;
    for (const auto &c : input) {
        const auto kind = c.value("kind", std::string());
        Json ch;
        if (kind == "chat") {
            const auto id = c.value("chat_id", std::string()), platform = c.value("platform", std::string());
            const auto delivery = c.value("delivery", std::string("auto"));
            auto accounts = strings(c.value("adapter_ids", Json::array()));
            if (id.empty() || !clean(id, 128) || platform.empty() || !clean(platform, 64) ||
                (delivery != "auto" && delivery != "selected") ||
                (delivery == "selected" && accounts.empty()))
                throw std::invalid_argument("Invalid chat destination");
            auto label = c.value("name", std::string());
            if (!clean(label, 240))
                throw std::invalid_argument("Invalid destination name");
            ch = {{"kind", kind},           {"platform", platform},
                  {"chat_id", id},          {"is_group", c.value("is_group", true)},
                  {"name", label},          {"delivery", delivery},
                  {"adapter_ids", accounts}};
        } else if (kind == "smtp") {
            const auto to = c.value("to", std::string());
            if (!mailbox(to))
                throw std::invalid_argument("Invalid recipient email");
            ch = {{"kind", kind},
                  {"to", to},
                  {"transport", c.value("transport", std::string()) == "legacy" ? "legacy" : "shared"}};
        } else if (kind == "webhook") {
            const auto url = c.value("url", std::string());
            if (!webhook(url))
                throw std::invalid_argument("Invalid Webhook URL");
            ch = notice_webhook::normalize(c);
        } else
            throw std::invalid_argument("Unknown notification channel");
        if (seen.insert(ch.dump()).second)
            channels.push_back(std::move(ch));
    }
    return {{"id", in.value("id", std::string())},
            {"name", name},
            {"enabled", in.value("enabled", true)},
            {"subscription", mode},
            {"level_mask", mask},
            {"events", events},
            {"source",
             {{"mode", scope},
              {"platforms", platforms},
              {"adapter_ids", ids},
              {"include_system", source.value("include_system", true)}}},
            {"channels", channels}};
}
// Legacy data is projected, not rewritten at startup. Commands and imported masks keep working.
inline Json all(const Json &conf) {
    Json out = Json::array();
    for (const auto &w : conf.value("windows", Json::array()))
        if (w.is_object() && !w.value("chat_id", std::string()).empty()) {
            const auto id = w.value("adapter_id", std::string());
            auto events = w.value("events", Json::array());
            out.push_back({{"id", legacyId(w)},
                           {"legacy", true},
                           {"name", w.value("name", std::string())},
                           {"enabled", w.value("enabled", true)},
                           {"subscription", w.value("subscription", events.empty() ? std::string("levels")
                                                                                   : std::string("events"))},
                           {"level_mask", w.value("level_mask", 15)},
                           {"events", events},
                           {"source",
                            {{"mode", id.empty() ? "all" : "accounts"},
                             {"platforms", Json::array()},
                             {"adapter_ids", id.empty() ? Json::array() : Json::array({id})},
                             {"include_system", true}}},
                           {"channels", Json::array({{{"kind", "chat"},
                                                      {"platform", w.value("platform", std::string())},
                                                      {"chat_id", w["chat_id"]},
                                                      {"is_group", w.value("is_group", true)},
                                                      {"name", w.value("name", std::string())},
                                                      {"delivery", "auto"},
                                                      {"adapter_ids", Json::array()},
                                                      {"preferred_adapter", id}}})}});
        }
    for (const auto *kind : {"smtp", "webhook"})
        if (conf.contains(kind) && conf[kind].is_object()) {
            const auto &old = conf[kind];
            const bool mail = std::string(kind) == "smtp";
            if (old.value("rule_hidden", false) || old.value(mail ? "to" : "url", std::string()).empty())
                continue;
            Json ch = {{"kind", kind}};
            if (mail) {
                ch["to"] = old["to"];
                ch["transport"] = "legacy";
            } else
                ch["url"] = old["url"];
            out.push_back({{"id", std::string("legacy-") + kind},
                           {"legacy", true},
                           {"name", ""},
                           {"enabled", old.value("enabled", false)},
                           {"subscription", "levels"},
                           {"events", Json::array()},
                           {"level_mask", old.value("level_mask", 15)},
                           {"source",
                            {{"mode", "all"},
                             {"include_system", true},
                             {"platforms", Json::array()},
                             {"adapter_ids", Json::array()}}},
                           {"channels", Json::array({ch})}});
        }
    for (const auto &r : conf.value("rules", Json::array()))
        if (r.is_object())
            out.push_back(r);
    return out;
}
inline bool subscribed(const Json &r, int level, const std::string &op) {
    if (!r.value("enabled", true))
        return false;
    const auto events = r.value("events", Json::array());
    const auto mode = r.value("subscription", events.empty() ? std::string("levels") : std::string("events"));
    return mode == "events" ? std::find(events.begin(), events.end(), Json(op)) != events.end()
                            : (r.value("level_mask", 15) & level) != 0;
}
inline bool sourceMatches(const Json &r, const std::string &platform, const std::string &adapter) {
    const auto s = r.value("source", Json::object());
    if (adapter.empty())
        return s.value("include_system", true);
    const auto mode = s.value("mode", std::string("all"));
    if (mode == "all")
        return true;
    const auto values = s.value(mode == "platforms" ? "platforms" : "adapter_ids", Json::array());
    return std::find(values.begin(), values.end(), Json(mode == "platforms" ? platform : adapter)) !=
           values.end();
}
inline bool remove(Json &conf, const std::string &id) {
    bool found = false;
    Json wins = Json::array(), rules = Json::array();
    for (const auto &w : conf.value("windows", Json::array())) {
        if (w.is_object() && legacyId(w) == id)
            found = true;
        else
            wins.push_back(w);
    }
    for (const auto &r : conf.value("rules", Json::array())) {
        if (r.value("id", std::string()) == id)
            found = true;
        else
            rules.push_back(r);
    }
    conf["windows"] = wins;
    conf["rules"] = rules;
    for (const auto *kind : {"smtp", "webhook"})
        if (id == std::string("legacy-") + kind && conf.contains(kind)) {
            conf[kind]["enabled"] = false;
            conf[kind]["rule_hidden"] = true;
            found = true;
        }
    return found;
}
inline Json smtpTransport(const Json &root, const Json &notice, const Json &channel) {
    const auto shared = root.value("identity_email", Json::object()),
               legacy = notice.value("smtp", Json::object());
    auto out =
        channel.value("transport", std::string()) == "legacy" && !legacy.value("host", std::string()).empty()
            ? legacy
        : !shared.value("host", std::string()).empty() ? shared
                                                       : legacy;
    out["to"] = channel.value("to", std::string());
    return out;
}
// .notice off removes this destination only, not the other channels on a multi-channel rule.
inline void removeChat(Json &conf, const std::string &platform, const std::string &adapter,
                       const std::string &id, bool group) {
    for (auto &r : conf["rules"]) {
        Json keep = Json::array();
        for (const auto &c : r.value("channels", Json::array())) {
            bool match = c.value("kind", std::string()) == "chat" &&
                         c.value("platform", std::string()) == platform &&
                         c.value("chat_id", std::string()) == id && c.value("is_group", true) == group;
            const auto ids = c.value("adapter_ids", Json::array());
            if (match && c.value("delivery", std::string("auto")) == "selected") {
                match = std::find(ids.begin(), ids.end(), Json(adapter)) != ids.end();
                if (match) {
                    Json others = Json::array();
                    for (const auto &i : ids)
                        if (i != adapter)
                            others.push_back(i);
                    if (!others.empty()) {
                        auto remaining = c;
                        remaining["adapter_ids"] = others;
                        keep.push_back(remaining);
                    }
                }
            }
            if (!match)
                keep.push_back(c);
        }
        r["channels"] = keep;
    }
    Json remaining = Json::array();
    for (const auto &r : conf["rules"])
        if (!r.value("channels", Json::array()).empty())
            remaining.push_back(r);
    conf["rules"] = remaining;
}
} // namespace dice::notice_rules
