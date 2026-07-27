#include "causal_rule_manager.h"
#include "../../common/logger.h"
#include "../../common/utils.h"

#include <sqlite_orm/sqlite_orm.h>
#include <regex>
#include <algorithm>
#include <random>
#include <sstream>

namespace dice {

namespace orm = sqlite_orm;

// ═══════════════════════════════════════════════════════════════
// Enum string conversion
// ═══════════════════════════════════════════════════════════════

const char* CausalRuleManager::condTypeStr(CausalCondType t) {
    switch (t) {
        case CausalCondType::Keyword:     return "keyword";
        case CausalCondType::Prefix:      return "prefix";
        case CausalCondType::Regex:       return "regex";
        case CausalCondType::Search:      return "search";
        case CausalCondType::UserFilter:  return "user_filter";
        case CausalCondType::GroupFilter: return "group_filter";
        case CausalCondType::Cooldown:    return "cooldown";
        case CausalCondType::CounterCheck:return "counter_check";
    }
    return "keyword";
}

CausalCondType CausalRuleManager::condTypeFromStr(const std::string& s) {
    if (s == "prefix")       return CausalCondType::Prefix;
    if (s == "regex")        return CausalCondType::Regex;
    if (s == "search")       return CausalCondType::Search;
    if (s == "user_filter")  return CausalCondType::UserFilter;
    if (s == "group_filter") return CausalCondType::GroupFilter;
    if (s == "cooldown")     return CausalCondType::Cooldown;
    if (s == "counter_check")return CausalCondType::CounterCheck;
    return CausalCondType::Keyword;
}

const char* CausalRuleManager::actionTypeStr(CausalActionType t) {
    switch (t) {
        case CausalActionType::Reply:        return "reply";
        case CausalActionType::CounterSet:   return "counter_set";
        case CausalActionType::CounterAdd:   return "counter_add";
        case CausalActionType::CounterReset: return "counter_reset";
        case CausalActionType::ApiCall:      return "api_call";
    }
    return "reply";
}

CausalActionType CausalRuleManager::actionTypeFromStr(const std::string& s) {
    if (s == "counter_set")   return CausalActionType::CounterSet;
    if (s == "counter_add")   return CausalActionType::CounterAdd;
    if (s == "counter_reset") return CausalActionType::CounterReset;
    if (s == "api_call")      return CausalActionType::ApiCall;
    return CausalActionType::Reply;
}

// ═══════════════════════════════════════════════════════════════
// JSON serialization
// ═══════════════════════════════════════════════════════════════

json CausalRule::toJSON() const {
    json j;
    j["id"] = id;
    j["name"] = name;
    j["scope"] = scope;
    j["scopeIds"] = scopeIds;
    j["enabled"] = enabled;
    j["priority"] = priority;
    j["cooldownMs"] = cooldownMs;
    j["cooldownKey"] = cooldownKey;
    j["logic"] = logic;
    j["createdAt"] = createdAt;
    j["updatedAt"] = updatedAt;

    json conds = json::array();
    for (auto& c : conditions) {
        json cj;
        cj["type"] = CausalRuleManager::condTypeStr(c.type);
        cj["content"] = c.content;
        if (c.type == CausalCondType::CounterCheck) {
            cj["op"] = c.op;
            cj["value"] = c.value;
            cj["counterName"] = c.counterName;
        }
        conds.push_back(cj);
    }
    j["conditions"] = conds;

    json acts = json::array();
    for (auto& a : actions) {
        json aj;
        aj["type"] = CausalRuleManager::actionTypeStr(a.type);
        if (a.type == CausalActionType::Reply) {
            aj["replies"] = a.replies;
        } else if (a.type == CausalActionType::CounterAdd ||
                   a.type == CausalActionType::CounterSet) {
            aj["counterName"] = a.counterName;
            aj["counterScope"] = a.counterScope;
            aj["counterDelta"] = a.counterDelta;
        } else if (a.type == CausalActionType::CounterReset) {
            aj["counterName"] = a.counterName;
            aj["counterScope"] = a.counterScope;
        } else if (a.type == CausalActionType::ApiCall) {
            aj["apiUrl"] = a.apiUrl;
            aj["apiVar"] = a.apiVar;
        }
        acts.push_back(aj);
    }
    j["actions"] = acts;
    return j;
}

CausalRule CausalRule::fromJSON(const json& j) {
    CausalRule r;
    r.id = j.value("id", 0);
    r.name = j.value("name", std::string());
    r.scope = j.value("scope", std::string("global"));
    if (j.contains("scopeIds") && j["scopeIds"].is_array()) {
        for (auto& s : j["scopeIds"]) if (s.is_string()) r.scopeIds.push_back(s.get<std::string>());
    }
    r.enabled = j.value("enabled", true);
    r.priority = j.value("priority", 100);
    r.cooldownMs = j.value("cooldownMs", 0);
    r.cooldownKey = j.value("cooldownKey", std::string("per-user"));
    r.logic = j.value("logic", std::string("or"));
    r.createdAt = j.value("createdAt", std::string());
    r.updatedAt = j.value("updatedAt", std::string());

    if (j.contains("conditions") && j["conditions"].is_array()) {
        for (auto& cj : j["conditions"]) {
            CausalCondition c;
            c.type = CausalRuleManager::condTypeFromStr(cj.value("type", std::string("keyword")));
            c.content = cj.value("content", std::string());
            c.op = cj.value("op", std::string(">="));
            c.value = cj.value("value", 0);
            c.counterName = cj.value("counterName", std::string());
            if (c.counterName.empty()) c.counterName = cj.value("counter_name", std::string());
            r.conditions.push_back(c);
        }
    }

    if (j.contains("actions") && j["actions"].is_array()) {
        for (auto& aj : j["actions"]) {
            CausalAction a;
            a.type = CausalRuleManager::actionTypeFromStr(aj.value("type", std::string("reply")));
            if (a.type == CausalActionType::Reply) {
                if (aj.contains("replies") && aj["replies"].is_array()) {
                    for (auto& r : aj["replies"]) if (r.is_string()) a.replies.push_back(r.get<std::string>());
                }
            } else if (a.type == CausalActionType::CounterAdd || a.type == CausalActionType::CounterSet) {
                a.counterName = aj.value("counterName", aj.value("counter_name", std::string()));
                a.counterScope = aj.value("counterScope", aj.value("counter_scope", std::string("per-user")));
                a.counterDelta = aj.value("counterDelta", aj.value("counter_delta", 0));
                if (a.type == CausalActionType::CounterSet)
                    a.counterDelta = aj.value("counterValue", aj.value("counter_value", a.counterDelta));
            } else if (a.type == CausalActionType::CounterReset) {
                a.counterName = aj.value("counterName", aj.value("counter_name", std::string()));
                a.counterScope = aj.value("counterScope", aj.value("counter_scope", std::string("per-user")));
            } else if (a.type == CausalActionType::ApiCall) {
                a.apiUrl = aj.value("apiUrl", aj.value("api_url", std::string()));
                a.apiVar = aj.value("apiVar", aj.value("api_var", std::string("result")));
            }
            r.actions.push_back(a);
        }
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

CausalRuleManager::CausalRuleManager(Database& db, ConfigManager& cfg,
                                      CooldownManager& cooldownMgr,
                                      CounterStore& counterStore)
    : db_(db), cfg_(cfg), cooldownMgr_(cooldownMgr), counterStore_(counterStore) {}

// ═══════════════════════════════════════════════════════════════
// CRUD
// ═══════════════════════════════════════════════════════════════

void CausalRuleManager::loadRules() {
    auto* st = db_.getStorage();
    if (!st) return;
    // 在本地建好完整列表再整体换指针——正在遍历旧快照的消息线程不受影响。
    auto next = std::make_shared<std::vector<CausalRule>>();
    try {
        auto rows = st->get_all<CausalRuleRow>();
        next->reserve(rows.size());
        for (auto& row : rows) {
            CausalRule r;
            r.id = row.id;
            r.name = row.name;
            r.scope = row.scope.empty() ? "global" : row.scope;
            r.enabled = row.enabled;
            r.priority = row.priority;
            r.cooldownMs = row.cooldownMs;
            r.cooldownKey = row.cooldownKey.empty() ? "per-user" : row.cooldownKey;
            r.logic = row.logic.empty() ? "or" : row.logic;
            r.createdAt = row.createdAt;
            r.updatedAt = row.updatedAt;

            // Parse scopeIds
            if (!row.scopeIds.empty()) {
                try {
                    auto arr = json::parse(row.scopeIds);
                    if (arr.is_array()) {
                        for (auto& s : arr) if (s.is_string()) r.scopeIds.push_back(s.get<std::string>());
                    }
                } catch (...) {}
            }

            // Parse conditions
            if (!row.conditions.empty()) {
                try {
                    auto arr = json::parse(row.conditions);
                    if (arr.is_array()) {
                        for (auto& cj : arr) {
                            CausalCondition c;
                            c.type = condTypeFromStr(cj.value("type", std::string("keyword")));
                            c.content = cj.value("content", std::string());
                            c.op = cj.value("op", std::string(">="));
                            c.value = cj.value("value", 0);
                            c.counterName = cj.value("counterName", cj.value("counter_name", std::string()));
                            r.conditions.push_back(c);
                        }
                    }
                } catch (...) {}
            }

            // Parse actions
            if (!row.actions.empty()) {
                try {
                    auto arr = json::parse(row.actions);
                    if (arr.is_array()) {
                        for (auto& aj : arr) {
                            CausalAction a;
                            a.type = actionTypeFromStr(aj.value("type", std::string("reply")));
                            if (a.type == CausalActionType::Reply) {
                                if (aj.contains("replies") && aj["replies"].is_array())
                                    for (auto& rp : aj["replies"]) if (rp.is_string()) a.replies.push_back(rp.get<std::string>());
                            } else if (a.type == CausalActionType::CounterAdd || a.type == CausalActionType::CounterSet) {
                                a.counterName = aj.value("counterName", aj.value("counter_name", std::string()));
                                a.counterScope = aj.value("counterScope", aj.value("counter_scope", std::string("per-user")));
                                a.counterDelta = aj.value("counterDelta", aj.value("counter_delta", 0));
                                if (a.type == CausalActionType::CounterSet)
                                    a.counterDelta = aj.value("counterValue", aj.value("counter_value", a.counterDelta));
                            } else if (a.type == CausalActionType::CounterReset) {
                                a.counterName = aj.value("counterName", aj.value("counter_name", std::string()));
                                a.counterScope = aj.value("counterScope", aj.value("counter_scope", std::string("per-user")));
                            } else if (a.type == CausalActionType::ApiCall) {
                                a.apiUrl = aj.value("apiUrl", aj.value("api_url", std::string()));
                                a.apiVar = aj.value("apiVar", aj.value("api_var", std::string("result")));
                            }
                            r.actions.push_back(a);
                        }
                    }
                } catch (...) {}
            }

            next->push_back(std::move(r));
        }
        // Sort by priority descending
        std::sort(next->begin(), next->end(),
                  [](const CausalRule& a, const CausalRule& b) { return a.priority > b.priority; });
        DICE_LOG_INFO("CausalRuleManager: loaded {} rules", next->size());
        std::lock_guard<std::mutex> lock(rulesMutex_);
        rules_ = std::move(next);
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CausalRuleManager: loadRules failed: {}", e.what());
    }
}

int CausalRuleManager::addRule(const CausalRule& rule) {
    auto* st = db_.getStorage();
    if (!st) return -1;
    try {
        CausalRuleRow row;
        row.name = rule.name;
        row.scope = rule.scope;
        json scopeIdsJson = json::array();
        for (auto& s : rule.scopeIds) scopeIdsJson.push_back(s);
        row.scopeIds = scopeIdsJson.dump();
        row.enabled = rule.enabled;
        row.priority = rule.priority;
        row.cooldownMs = rule.cooldownMs;
        row.cooldownKey = rule.cooldownKey;

        json condsJson = json::array();
        for (auto& c : rule.conditions) {
            json cj;
            cj["type"] = condTypeStr(c.type);
            cj["content"] = c.content;
            if (c.type == CausalCondType::CounterCheck) {
                cj["op"] = c.op;
                cj["value"] = c.value;
                cj["counterName"] = c.counterName;
            }
            condsJson.push_back(cj);
        }
        row.conditions = condsJson.dump();
        row.logic = rule.logic;

        json actsJson = json::array();
        for (auto& a : rule.actions) {
            json aj;
            aj["type"] = actionTypeStr(a.type);
            if (a.type == CausalActionType::Reply) {
                aj["replies"] = a.replies;
            } else if (a.type == CausalActionType::CounterAdd || a.type == CausalActionType::CounterSet) {
                aj["counterName"] = a.counterName;
                aj["counterScope"] = a.counterScope;
                aj["counterDelta"] = a.counterDelta;
                if (a.type == CausalActionType::CounterSet) aj["counterValue"] = a.counterDelta;
            } else if (a.type == CausalActionType::CounterReset) {
                aj["counterName"] = a.counterName;
                aj["counterScope"] = a.counterScope;
            } else if (a.type == CausalActionType::ApiCall) {
                aj["apiUrl"] = a.apiUrl;
                aj["apiVar"] = a.apiVar;
            }
            actsJson.push_back(aj);
        }
        row.actions = actsJson.dump();
        row.createdAt = utils::nowIso8601();
        row.updatedAt = row.createdAt;

        int newId = (int)st->insert(row);
        loadRules();  // hot reload
        DICE_LOG_INFO("CausalRuleManager: added rule '{}' (id={})", rule.name, newId);
        return newId;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CausalRuleManager: addRule failed: {}", e.what());
        return -1;
    }
}

bool CausalRuleManager::updateRule(int id, const CausalRule& rule) {
    auto* st = db_.getStorage();
    if (!st) return false;
    try {
        auto rows = st->get_all<CausalRuleRow>(
            orm::where(orm::c(&CausalRuleRow::id) == id), orm::limit(1));
        if (rows.empty()) return false;

        auto row = rows.front();
        row.name = rule.name;
        row.scope = rule.scope;
        json scopeIdsJson = json::array();
        for (auto& s : rule.scopeIds) scopeIdsJson.push_back(s);
        row.scopeIds = scopeIdsJson.dump();
        row.enabled = rule.enabled;
        row.priority = rule.priority;
        row.cooldownMs = rule.cooldownMs;
        row.cooldownKey = rule.cooldownKey;

        json condsJson = json::array();
        for (auto& c : rule.conditions) {
            json cj;
            cj["type"] = condTypeStr(c.type);
            cj["content"] = c.content;
            if (c.type == CausalCondType::CounterCheck) {
                cj["op"] = c.op;
                cj["value"] = c.value;
                cj["counterName"] = c.counterName;
            }
            condsJson.push_back(cj);
        }
        row.conditions = condsJson.dump();
        row.logic = rule.logic;

        json actsJson = json::array();
        for (auto& a : rule.actions) {
            json aj;
            aj["type"] = actionTypeStr(a.type);
            if (a.type == CausalActionType::Reply) {
                aj["replies"] = a.replies;
            } else if (a.type == CausalActionType::CounterAdd || a.type == CausalActionType::CounterSet) {
                aj["counterName"] = a.counterName;
                aj["counterScope"] = a.counterScope;
                aj["counterDelta"] = a.counterDelta;
                if (a.type == CausalActionType::CounterSet) aj["counterValue"] = a.counterDelta;
            } else if (a.type == CausalActionType::CounterReset) {
                aj["counterName"] = a.counterName;
                aj["counterScope"] = a.counterScope;
            } else if (a.type == CausalActionType::ApiCall) {
                aj["apiUrl"] = a.apiUrl;
                aj["apiVar"] = a.apiVar;
            }
            actsJson.push_back(aj);
        }
        row.actions = actsJson.dump();
        row.updatedAt = utils::nowIso8601();

        st->update(row);
        loadRules();  // hot reload
        // Clear cooldown for this rule on edit
        // (the key pattern includes ruleId, so we can't easily clear all;
        //  CooldownManager.clearAll is a sledgehammer but acceptable)
        DICE_LOG_INFO("CausalRuleManager: updated rule id={}", id);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CausalRuleManager: updateRule({}) failed: {}", id, e.what());
        return false;
    }
}

bool CausalRuleManager::deleteRule(int id) {
    auto* st = db_.getStorage();
    if (!st) return false;
    try {
        st->remove_all<CausalRuleRow>(orm::where(orm::c(&CausalRuleRow::id) == id));
        counterStore_.deleteByRule(id);
        loadRules();  // hot reload
        DICE_LOG_INFO("CausalRuleManager: deleted rule id={}", id);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CausalRuleManager: deleteRule({}) failed: {}", id, e.what());
        return false;
    }
}

bool CausalRuleManager::toggleRule(int id) {
    auto* st = db_.getStorage();
    if (!st) return false;
    try {
        auto rows = st->get_all<CausalRuleRow>(
            orm::where(orm::c(&CausalRuleRow::id) == id), orm::limit(1));
        if (rows.empty()) return false;
        auto row = rows.front();
        row.enabled = !row.enabled;
        row.updatedAt = utils::nowIso8601();
        st->update(row);
        loadRules();
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CausalRuleManager: toggleRule({}) failed: {}", id, e.what());
        return false;
    }
}

std::optional<CausalRule> CausalRuleManager::getRuleById(int id) const {
    auto snap = snapshot();
    for (auto& r : *snap) if (r.id == id) return r;
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// Matching & Execution
// ═══════════════════════════════════════════════════════════════

CausalMatchResult CausalRuleManager::matchAndExecute(const std::string& msg,
                                                      const std::string& userId,
                                                      const std::string& groupId,
                                                      const std::string& nick,
                                                      bool dryRun) {
    CausalMatchResult result;
    auto snap = snapshot();
    for (auto& rule : *snap) {
        if (!rule.enabled) continue;
        if (!checkScope(rule, userId, groupId)) continue;

        if (!evalConditions(rule, msg, userId, groupId, dryRun)) continue;

        // Conditions met — check cooldown
        std::string cdKey = buildCooldownKey(rule, userId, groupId);
        if (!dryRun && cooldownMgr_.isCooling(cdKey, rule.cooldownMs)) {
            DICE_LOG_DEBUG("CausalRuleManager: rule '{}' on cooldown (key={})", rule.name, cdKey);
            continue;
        }

        // Trigger cooldown
        if (!dryRun) cooldownMgr_.trigger(cdKey);

        // Execute actions
        result.matched = true;
        result.ruleId = rule.id;
        result.ruleName = rule.name;
        result.reply = executeActions(rule, msg, userId, groupId, nick,
                                       result.counterChanges, dryRun);
        return result;  // First match wins
    }
    return result;
}

bool CausalRuleManager::checkScope(const CausalRule& rule,
                                    const std::string& userId,
                                    const std::string& groupId) const {
    if (rule.scope == "global") return true;
    if (rule.scope == "group") {
        if (groupId.empty()) return false;
        if (rule.scopeIds.empty()) return true;  // any group
        for (auto& id : rule.scopeIds) if (id == groupId) return true;
        return false;
    }
    if (rule.scope == "user") {
        if (userId.empty()) return false;
        if (rule.scopeIds.empty()) return true;  // any user
        for (auto& id : rule.scopeIds) if (id == userId) return true;
        return false;
    }
    return true;
}

bool CausalRuleManager::evalConditions(const CausalRule& rule,
                                        const std::string& msg,
                                        const std::string& userId,
                                        const std::string& groupId,
                                        bool dryRun) const {
    if (rule.conditions.empty()) return true;  // no conditions = always match

    bool isAnd = (rule.logic == "and");
    for (auto& cond : rule.conditions) {
        bool met = evalCondition(cond, msg, userId, groupId, rule.id, dryRun);
        if (isAnd && !met) return false;
        if (!isAnd && met) return true;
    }
    return isAnd;  // AND: all met → true; OR: none met → false
}

bool CausalRuleManager::evalCondition(const CausalCondition& cond,
                                       const std::string& msg,
                                       const std::string& userId,
                                       const std::string& groupId,
                                       int ruleId,
                                       bool dryRun) const {
    switch (cond.type) {
        case CausalCondType::Keyword:
        case CausalCondType::Prefix:
        case CausalCondType::Regex:
        case CausalCondType::Search:
            return matchText(msg, cond.content, cond.type);

        case CausalCondType::UserFilter: {
            auto [isWhitelist, ids] = parseFilter(cond.content);
            bool inList = std::find(ids.begin(), ids.end(), userId) != ids.end();
            return isWhitelist ? inList : !inList;
        }

        case CausalCondType::GroupFilter: {
            auto [isWhitelist, ids] = parseFilter(cond.content);
            bool inList = std::find(ids.begin(), ids.end(), groupId) != ids.end();
            return isWhitelist ? inList : !inList;
        }

        case CausalCondType::Cooldown: {
            // Uses the rule's cooldownMs and cooldownKey
            // In dry-run, don't check cooldown (for testing)
            if (dryRun) return true;
            // The actual cooldown check happens in matchAndExecute; this condition
            // type is for explicit "check cooldown" — but since matchAndExecute
            // already checks rule.cooldownMs, this is a no-op pass-through.
            return true;
        }

        case CausalCondType::CounterCheck: {
            if (cond.counterName.empty()) return false;
            // Build the counter key using per-user scope by default
            std::string ckey = buildCounterKey(ruleId, cond.counterName,
                                                "per-user", userId, groupId);
            int current = counterStore_.get(ckey);
            if (cond.op == ">=") return current >= cond.value;
            if (cond.op == "<=") return current <= cond.value;
            if (cond.op == "==") return current == cond.value;
            if (cond.op == "!=") return current != cond.value;
            if (cond.op == ">")  return current > cond.value;
            if (cond.op == "<")  return current < cond.value;
            return false;
        }
    }
    return false;
}

bool CausalRuleManager::matchText(const std::string& msg, const std::string& pattern,
                                   CausalCondType type) const {
    if (pattern.empty()) return false;
    std::string msgLower = msg;
    std::string patLower = pattern;
    std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);
    std::transform(patLower.begin(), patLower.end(), patLower.begin(), ::tolower);

    switch (type) {
        case CausalCondType::Keyword:
            return msgLower == patLower;
        case CausalCondType::Prefix:
            return msgLower.rfind(patLower, 0) == 0;
        case CausalCondType::Search:
            return msgLower.find(patLower) != std::string::npos;
        case CausalCondType::Regex:
            try {
                std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
                return std::regex_search(msg, re);
            } catch (...) { return false; }
        default:
            return false;
    }
}

std::pair<bool, std::vector<std::string>> CausalRuleManager::parseFilter(const std::string& content) {
    bool isWhitelist = true;
    std::string idsStr = content;
    if (content.rfind("whitelist:", 0) == 0) {
        isWhitelist = true;
        idsStr = content.substr(10);
    } else if (content.rfind("blacklist:", 0) == 0) {
        isWhitelist = false;
        idsStr = content.substr(10);
    }
    std::vector<std::string> ids;
    std::stringstream ss(idsStr);
    std::string id;
    while (std::getline(ss, id, ',')) {
        // trim whitespace
        while (!id.empty() && (id.front() == ' ' || id.front() == '\t')) id.erase(id.begin());
        while (!id.empty() && (id.back() == ' ' || id.back() == '\t')) id.pop_back();
        if (!id.empty()) ids.push_back(id);
    }
    return {isWhitelist, ids};
}

std::string CausalRuleManager::buildCooldownKey(const CausalRule& rule,
                                                 const std::string& userId,
                                                 const std::string& groupId) const {
    std::string scopeId;
    if (rule.cooldownKey == "per-user") scopeId = userId;
    else if (rule.cooldownKey == "per-group") scopeId = groupId;
    else scopeId = "";  // global
    return std::to_string(rule.id) + ":" + rule.cooldownKey + ":" + scopeId;
}

std::string CausalRuleManager::buildCounterKey(int ruleId, const std::string& counterName,
                                                const std::string& scope,
                                                const std::string& userId,
                                                const std::string& groupId) const {
    std::string scopeId;
    if (scope == "per-user") scopeId = userId;
    else if (scope == "per-group") scopeId = groupId;
    else scopeId = "";  // global
    return std::to_string(ruleId) + ":" + counterName + ":" + scope + ":" + scopeId;
}

std::string CausalRuleManager::executeActions(const CausalRule& rule,
                                               const std::string& msg,
                                               const std::string& userId,
                                               const std::string& groupId,
                                               const std::string& nick,
                                               std::vector<CounterChange>& changes,
                                               bool dryRun) {
    std::string reply;
    // Collect counter values for {counter:name} resolution
    std::map<std::string, std::string> counterCtx;

    for (auto& action : rule.actions) {
        switch (action.type) {
            case CausalActionType::Reply: {
                if (action.replies.empty()) break;
                // Pick a random reply template
                std::string tmpl;
                if (action.replies.size() == 1) {
                    tmpl = action.replies[0];
                } else {
                    std::random_device rd;
                    std::mt19937 gen(rd());
                    std::uniform_int_distribution<> dist(0, (int)action.replies.size() - 1);
                    tmpl = action.replies[dist(gen)];
                }
                // Apply basic variable substitution ({nick}, {self}, etc.)
                // Note: full renderReply is done by the caller (main.cpp)
                // Here we just set the reply template; counter values are
                // injected into counterCtx for {counter:name} resolution.
                for (auto& [k, v] : counterCtx) {
                    std::string tok = "{counter:" + k + "}";
                    size_t pos = 0;
                    while ((pos = tmpl.find(tok, pos)) != std::string::npos) {
                        tmpl.replace(pos, tok.size(), v);
                        pos += v.size();
                    }
                }
                reply = tmpl;
                break;
            }

            case CausalActionType::CounterAdd: {
                if (action.counterName.empty()) break;
                std::string ckey = buildCounterKey(rule.id, action.counterName,
                                                    action.counterScope, userId, groupId);
                int oldVal = counterStore_.get(ckey);
                int newVal = oldVal + action.counterDelta;
                if (!dryRun) counterStore_.set(ckey, newVal);
                counterCtx[action.counterName] = std::to_string(newVal);
                changes.push_back({action.counterName, oldVal, newVal});
                break;
            }

            case CausalActionType::CounterSet: {
                if (action.counterName.empty()) break;
                std::string ckey = buildCounterKey(rule.id, action.counterName,
                                                    action.counterScope, userId, groupId);
                int oldVal = counterStore_.get(ckey);
                int newVal = action.counterDelta;
                if (!dryRun) counterStore_.set(ckey, newVal);
                counterCtx[action.counterName] = std::to_string(newVal);
                changes.push_back({action.counterName, oldVal, newVal});
                break;
            }

            case CausalActionType::CounterReset: {
                if (action.counterName.empty()) break;
                std::string ckey = buildCounterKey(rule.id, action.counterName,
                                                    action.counterScope, userId, groupId);
                int oldVal = counterStore_.get(ckey);
                if (!dryRun) counterStore_.reset(ckey);
                counterCtx[action.counterName] = "0";
                changes.push_back({action.counterName, oldVal, 0});
                break;
            }

            case CausalActionType::ApiCall: {
                // API calls are handled by the caller via renderReply's {api:URL}.
                // Here we just inject a placeholder; the actual fetch happens in
                // CommandRouter::renderReply when the reply template contains {api:URL}.
                // For now, we leave apiVar empty as the caller will handle it.
                break;
            }
        }
    }

    return reply;
}

}  // namespace dice
