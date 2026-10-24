#pragma once
#include "notice_rules.h"
#include <map>

namespace dice::notice_windows {
using Json = nlohmann::json;
// Identity is the destination, not its sources or sending accounts. Never put
// a secret-bearing URL (or a hash of it) in a public identifier.
inline std::string key(const Json& channel) {
    return notice_rules::destinationKey(channel);
}
inline Json subscription(const Json& rule, const Json& channel) {
    Json out;
    for (const auto* field : {"source", "subscription", "level_mask", "events"})
        if (rule.contains(field)) out[field] = rule[field];
    out["enabled"] = rule.value("window_policy_enabled", rule.value("enabled", true));
    out["name"] = rule.value("policy_name", rule.value("name", std::string()));
    if (channel.value("kind", std::string()) == "chat") out["sender"] = {
        {"delivery", channel.value("delivery", std::string("auto"))},
        {"adapter_ids", channel.value("adapter_ids", Json::array())},
        {"preferred_adapter", channel.value("preferred_adapter", std::string())}};
    return out;
}
// Projection is read-only. Historical rules with different source/event pairs
// remain separate subscriptions: merging both sets would broaden delivery.
inline Json all(const Json& conf) {
    Json windows = Json::array(); std::map<std::string, size_t> indices;
    for (const auto& rule : notice_rules::all(conf)) {
        size_t channelIndex = 0;
        for (const auto& channel : rule.value("channels", Json::array())) {
            const auto identity = key(channel); auto found = indices.find(identity);
            if (found == indices.end()) {
                indices[identity] = windows.size();
                windows.push_back({{"id", rule.value("id", std::string()) + "@" + std::to_string(channelIndex)},
                    {"name", rule.value("window_name", rule.value("name", std::string()))},
                    {"enabled", rule.value("window_enabled", rule.value("enabled", true))},
                    {"channel", channel}, {"subscriptions", Json::array()}, {"rule_ids", Json::array()}, {"mixed_delivery", false}});
                found = indices.find(identity);
            }
            auto& window = windows[found->second];
            const auto policy = subscription(rule, channel);
            if (channel.value("kind", std::string()) == "chat" && !window["subscriptions"].empty() && window["subscriptions"][0]["sender"] != policy["sender"])
                window["mixed_delivery"] = true;
            if (std::find(window["subscriptions"].begin(), window["subscriptions"].end(), policy) == window["subscriptions"].end()) window["subscriptions"].push_back(policy);
            if (std::find(window["rule_ids"].begin(), window["rule_ids"].end(), rule["id"]) == window["rule_ids"].end()) window["rule_ids"].push_back(rule["id"]);
            window["enabled"] = window["enabled"].get<bool>() || rule.value("window_enabled", rule.value("enabled", true));
            ++channelIndex;
        }
    }
    return windows;
}
inline Json normalize(const Json& input) {
    if (!input.is_object() || !input.contains("channel") || !input["channel"].is_object()) throw std::invalid_argument("请选择一个通知窗口");
    const auto name = input.value("name", std::string());
    if (!notice_rules::clean(name, 240)) throw std::invalid_argument("通知窗口名称无效");
    const auto policies = input.value("subscriptions", Json::array());
    if (!policies.is_array() || policies.empty() || policies.size() > 128) throw std::invalid_argument("请为通知窗口配置 1 至 128 条订阅规则");
    Json subscriptions = Json::array(); Json destination;
    for (const auto& policy : policies) {
        if (!policy.is_object()) throw std::invalid_argument("通知订阅规则无效");
        auto channel = input["channel"];
        if (channel.value("kind", std::string()) == "chat" && policy.contains("sender")) {
            if (!policy["sender"].is_object()) throw std::invalid_argument("历史发送帐号配置无效");
            for (const auto* field : {"delivery", "adapter_ids"}) if (policy["sender"].contains(field)) channel[field] = policy["sender"][field];
        }
        auto normalized = notice_rules::normalize(Json{{"name", policy.value("name", std::string())},
            {"enabled", policy.value("enabled", true)}, {"subscription", policy.value("subscription", std::string("levels"))},
            {"level_mask", policy.value("level_mask", 15)}, {"events", policy.value("events", Json::array())},
            {"source", policy.value("source", Json::object())}, {"channels", Json::array({channel})}});
        // Legacy auto-selection preference must survive an unrelated edit.
        if (channel.contains("preferred_adapter") && channel["preferred_adapter"].is_string()) normalized["channels"][0]["preferred_adapter"] = channel["preferred_adapter"];
        if (channel.value("kind", std::string()) == "chat" && policy.contains("sender") && policy["sender"].contains("preferred_adapter"))
            normalized["channels"][0]["preferred_adapter"] = policy["sender"]["preferred_adapter"];
        auto sub = subscription(normalized, normalized["channels"][0]); sub["name"] = normalized["name"];
        subscriptions.push_back(std::move(sub));
        if (destination.is_null()) {
            auto root = notice_rules::normalize({{"channels", Json::array({input["channel"]})}});
            destination = root["channels"][0];
            if (input["channel"].contains("preferred_adapter")) destination["preferred_adapter"] = input["channel"]["preferred_adapter"];
        }
    }
    return {{"name", name}, {"enabled", input.value("enabled", true)}, {"channel", destination}, {"subscriptions", subscriptions}};
}
inline void erase(Json& conf, const Json& channel) {
    const auto identity = key(channel); Json remaining = Json::array();
    for (const auto& old : conf.value("windows", Json::array())) {
        Json ch{{"kind", "chat"}, {"platform", old.value("platform", std::string())}, {"chat_id", old.value("chat_id", std::string())}, {"is_group", old.value("is_group", true)}};
        if (key(ch) != identity) remaining.push_back(old);
    }
    conf["windows"] = remaining; Json rules = Json::array();
    for (auto rule : conf.value("rules", Json::array())) {
        Json channels = Json::array();
        for (const auto& ch : rule.value("channels", Json::array())) if (key(ch) != identity) channels.push_back(ch);
        if (!channels.empty()) { rule["channels"] = channels; rules.push_back(std::move(rule)); }
    }
    conf["rules"] = rules;
    for (const auto* kind : {"smtp", "webhook"}) if (conf.contains(kind) && conf[kind].is_object()) {
        const auto field = std::string(kind) == "smtp" ? "to" : "url";
        if (conf[kind].contains(field) && key(Json{{"kind", kind}, {field, conf[kind][field]}}) == identity) {
            conf[kind]["enabled"] = false; conf[kind]["rule_hidden"] = true;
        }
    }
}
inline void store(Json& conf, const Json& window, const std::string& idPrefix) {
    size_t index = 0;
    for (const auto& policy : window["subscriptions"]) {
        auto channel = window["channel"];
        if (policy.contains("sender")) for (const auto* field : {"delivery", "adapter_ids", "preferred_adapter"})
            if (policy["sender"].contains(field)) channel[field] = policy["sender"][field];
        auto rule = policy; rule.erase("sender");
        rule["id"] = idPrefix + "-" + std::to_string(index++); rule["channels"] = Json::array({channel});
        rule["policy_name"] = policy.value("name", std::string()); rule["window_name"] = window["name"]; rule["name"] = window["name"];
        rule["window_enabled"] = window["enabled"]; rule["window_policy_enabled"] = policy.value("enabled", true);
        rule["enabled"] = window["enabled"].get<bool>() && policy.value("enabled", true);
        conf["rules"].push_back(std::move(rule));
    }
}
} // namespace dice::notice_windows
