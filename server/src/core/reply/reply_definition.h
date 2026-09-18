#pragma once

#include "reply_manager.h"
#include "../../common/weighted_reply.h"
#include <algorithm>
#include <stdexcept>

namespace dice::reply_definition {

inline bool legacyReferences(const json& values) {
    if (!values.is_array()) return false;
    return std::any_of(values.begin(), values.end(), [](const auto& value) {
        return value.is_object() && value.value("legacyReferences", false);
    });
}

inline void decodeResults(const json& values, std::vector<std::string>& texts, std::vector<int>& weights) {
    texts.clear(); weights.clear();
    if (!values.is_array()) return;
    std::vector<std::string> decodedTexts;
    std::vector<int> decodedWeights;
    for (const auto& value : values) {
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            if (text.empty()) continue;
            const auto parsed = weighted_reply::parse(text);
            decodedTexts.emplace_back(parsed.text);
            decodedWeights.push_back(static_cast<int>(parsed.weight));
        } else if (value.is_object()) {
            if (!value.at("weight").is_number_integer()) throw std::invalid_argument("result weights must be integers");
            decodedTexts.push_back(value.at("text").get<std::string>());
            decodedWeights.push_back(value.at("weight").get<int>());
        }
    }
    texts.swap(decodedTexts);
    weights.swap(decodedWeights);
}

inline std::string validateWeights(const ReplyRule& rule) {
    if (rule.resultWeights.size() != rule.results.size()) return "resultWeights must match results";
    bool active = false;
    for (size_t i = 0; i < rule.results.size(); ++i) {
        const auto weight = rule.resultWeights[i];
        if (weight < 0 || weight > 999999) return "result weight must be 0-999999";
        if (rule.results[i].empty() && weight > 0) return "non-empty reply required for positive weight";
        if (weight > 0 && !rule.results[i].empty()) active = true;
    }
    return active ? "" : "at least one non-empty reply must have positive weight";
}

inline void readResults(const json& body, ReplyRule& rule) {
    auto values = body.value("results", json::array());
    if (!values.is_array()) throw std::invalid_argument("results must be an array");
    if (values.empty()) values.push_back(body.value("replyContent", std::string()));
    if (body.contains("resultWeights")) {
        if (!body["resultWeights"].is_array()) throw std::invalid_argument("resultWeights must be an array");
        for (const auto& weight : body["resultWeights"])
            if (!weight.is_number_integer()) throw std::invalid_argument("result weights must be integers");
        // Formal weights own the meaning: text resembling ::N:: stays literal.
        rule.results = values.get<std::vector<std::string>>();
        rule.resultWeights = body.at("resultWeights").get<std::vector<int>>();
    } else decodeResults(values, rule.results, rule.resultWeights);
    rule.replyContent = rule.results.empty() ? "" : rule.results.front();
}

inline MatchType matchTypeFromStr(const std::string& type) {
    if (type == "prefix") return MatchType::kPrefix;
    if (type == "regex") return MatchType::kRegex;
    if (type == "search") return MatchType::kSearch;
    return MatchType::kKeyword;
}

inline ReplyRule replyRuleFromJson(const json& body) {
    ReplyRule rule;
    rule.priority = body.value("priority", 100);
    rule.enabled = body.value("enabled", true);
    rule.logic = body.value("logic", std::string("or")) == "and" ? "and" : "or";
    if (body.contains("conditions") && body["conditions"].is_array()) {
        for (const auto& condition : body["conditions"]) {
            const auto content = condition.value("content", std::string());
            if (!content.empty()) rule.conditions.push_back({matchTypeFromStr(condition.value("type", std::string("keyword"))), content});
        }
    }
    if (rule.conditions.empty()) rule.conditions.push_back({matchTypeFromStr(body.value("matchType", std::string("keyword"))), body.value("matchContent", std::string())});
    rule.matchType = rule.conditions.front().type;
    rule.matchContent = rule.conditions.front().content;
    readResults(body, rule);
    rule.prob = (std::clamp)(body.value("prob", 100), 0, 100);
    rule.cooldownSec = (std::max)(0, body.value("cooldownSec", 0));
    rule.dayLimit = (std::max)(0, body.value("dayLimit", 0));
    rule.cooldownNotice = body.value("cooldownNotice", std::string());
    rule.dayLimitNotice = body.value("dayLimitNotice", std::string());
    rule.scopeMode = body.value("scopeMode", std::string());
    if (rule.scopeMode != "allow" && rule.scopeMode != "deny") rule.scopeMode.clear();
    rule.scopeIds = body.value("scopeIds", std::string());
    rule.scopeUsersMode = body.value("scopeUsersMode", std::string());
    if (rule.scopeUsersMode != "allow" && rule.scopeUsersMode != "deny") rule.scopeUsersMode.clear();
    rule.scopeUsers = body.value("scopeUsers", std::string());
    return rule;
}

inline std::string replyRuleValidate(const ReplyRule& rule, bool eventTrigger = false) {
    if (auto error = validateWeights(rule); !error.empty()) return error;
    if (!eventTrigger && (rule.conditions.empty() || rule.conditions.front().content.empty())) return "match content required";
    for (const auto& condition : rule.conditions) if (condition.type == MatchType::kRegex) {
        std::string error;
        if (!ReplyMatcher::validateRegex(condition.content, &error)) return "invalid regex: " + error;
    }
    return {};
}

} // namespace dice::reply_definition
