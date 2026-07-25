#include "reply_manager.h"
#include "../../common/logger.h"
#include "../../common/utils.h"

#include <algorithm>
#include <random>

namespace dice {

// Parse a match-type string into the enum (mirrors the API's mapping).
static MatchType parseMatchType(const std::string& s) {
    if (s == "prefix") return MatchType::kPrefix;
    if (s == "regex")  return MatchType::kRegex;
    if (s == "search") return MatchType::kSearch;
    return MatchType::kKeyword;
}

// Fill a rule's conditions/results vectors from its stored JSON, falling back to
// the legacy single match_content/reply_content when the JSON is absent/empty.
static void normalizeRule(ReplyRule& rule, const std::string& conditionsJson,
                          const std::string& logic, const std::string& resultsJson) {
    rule.conditions.clear();
    rule.results.clear();
    rule.logic = (logic == "and") ? "and" : "or";
    try {
        if (!conditionsJson.empty()) {
            json arr = json::parse(conditionsJson);
            if (arr.is_array())
                for (auto& c : arr) {
                    ReplyCondition cond;
                    cond.type = parseMatchType(c.value("type", std::string("keyword")));
                    cond.content = c.value("content", std::string());
                    if (!cond.content.empty()) rule.conditions.push_back(cond);
                }
        }
    } catch (...) {}
    try {
        if (!resultsJson.empty()) {
            json arr = json::parse(resultsJson);
            if (arr.is_array())
                for (auto& r : arr) if (r.is_string() && !r.get<std::string>().empty())
                    rule.results.push_back(r.get<std::string>());
        }
    } catch (...) {}
    // Legacy fallbacks + keep the back-compat scalar fields aligned to [0].
    if (rule.conditions.empty()) rule.conditions.push_back({rule.matchType, rule.matchContent});
    else { rule.matchType = rule.conditions[0].type; rule.matchContent = rule.conditions[0].content; }
    if (rule.results.empty()) rule.results.push_back(rule.replyContent);
    else rule.replyContent = rule.results[0];
}

// Serialize a rule's conditions/results into the row's JSON columns, keeping the
// legacy scalar columns pointed at the first condition/result.
static void serializeRule(const ReplyRule& rule, ReplyRuleRow& row) {
    if (rule.conditions.size() > 1) {
        json arr = json::array();
        for (const auto& c : rule.conditions)
            arr.push_back({{"type", matchTypeToString(c.type)}, {"content", c.content}});
        row.conditions = arr.dump();
    } else row.conditions = "";   // single → use legacy column only
    row.logic = (rule.logic == "and") ? "and" : "or";
    if (rule.results.size() > 1) { json a = rule.results; row.results = a.dump(); }
    else row.results = "";
    // back-compat scalars
    if (!rule.conditions.empty()) {
        row.matchType = static_cast<int>(rule.conditions[0].type);
        row.matchContent = rule.conditions[0].content;
    } else { row.matchType = static_cast<int>(rule.matchType); row.matchContent = rule.matchContent; }
    row.replyContent = rule.results.empty() ? rule.replyContent : rule.results[0];
}

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

ReplyManager::ReplyManager(Database& db, ConfigManager& configMgr)
    : db_(db)
    , configMgr_(configMgr) {

    loadRules();

    // Register hot-reload callback
    configMgr_.onConfigChanged([this]() {
        DICE_LOG_INFO("ReplyManager: config changed, reloading rules");
        loadRules();
    });

    DICE_LOG_INFO("ReplyManager: initialized with {} rules", rules_.size());
}

// ═══════════════════════════════════════════════════════════════
// CRUD
// ═══════════════════════════════════════════════════════════════

void ReplyManager::loadRules() {
    auto* storage = db_.getStorage();
    if (!storage) {
        DICE_LOG_WARN("ReplyManager: database not open, cannot load rules");
        return;
    }

    try {
        auto rows = storage->get_all<ReplyRuleRow>();

        rules_.clear();
        rules_.reserve(rows.size());

        for (const auto& row : rows) {
            ReplyRule rule;
            rule.id           = row.id;
            rule.matchType    = static_cast<MatchType>(row.matchType);
            rule.matchContent = row.matchContent;
            rule.replyContent = row.replyContent;
            rule.enabled      = row.enabled;
            rule.priority     = row.priority;
            rule.createdAt    = row.createdAt;
            rule.updatedAt    = row.updatedAt;
            normalizeRule(rule, row.conditions, row.logic, row.results);
            rules_.push_back(std::move(rule));
        }

        // Sort by priority desc, then id asc
        std::sort(rules_.begin(), rules_.end(),
            [](const ReplyRule& a, const ReplyRule& b) {
                if (a.priority != b.priority) return a.priority > b.priority;
                return a.id < b.id;
            });

        DICE_LOG_INFO("ReplyManager: loaded {} rules", rules_.size());
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ReplyManager: failed to load rules: {}", e.what());
    }
}

int ReplyManager::addRule(const ReplyRule& rule) {
    auto* storage = db_.getStorage();
    if (!storage) {
        DICE_LOG_ERROR("ReplyManager: database not open");
        return -1;
    }

    try {
        ReplyRuleRow row;
        row.matchType    = static_cast<int>(rule.matchType);
        row.matchContent = rule.matchContent;
        row.replyContent = rule.replyContent;
        row.enabled      = rule.enabled;
        row.priority     = rule.priority;
        row.createdAt    = utils::nowIso8601();
        row.updatedAt    = row.createdAt;
        serializeRule(rule, row);

        int newId = static_cast<int>(storage->insert(row));

        // Reload to keep cache consistent
        loadRules();

        DICE_LOG_INFO("ReplyManager: added rule id={}", newId);
        return newId;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ReplyManager: failed to add rule: {}", e.what());
        return -1;
    }
}

bool ReplyManager::updateRule(int id, const ReplyRule& rule) {
    auto* storage = db_.getStorage();
    if (!storage) {
        DICE_LOG_ERROR("ReplyManager: database not open");
        return false;
    }

    try {
        auto rows = storage->get_all<ReplyRuleRow>(
            orm::where(orm::c(&ReplyRuleRow::id) == id)
        );

        if (rows.empty()) {
            DICE_LOG_WARN("ReplyManager: rule id={} not found for update", id);
            return false;
        }

        ReplyRuleRow row = rows[0];
        row.matchType    = static_cast<int>(rule.matchType);
        row.matchContent = rule.matchContent;
        row.replyContent = rule.replyContent;
        row.enabled      = rule.enabled;
        row.priority     = rule.priority;
        row.updatedAt    = utils::nowIso8601();
        serializeRule(rule, row);

        storage->update(row);

        // Reload to keep cache consistent
        loadRules();

        DICE_LOG_INFO("ReplyManager: updated rule id={}", id);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ReplyManager: failed to update rule id={}: {}", id, e.what());
        return false;
    }
}

bool ReplyManager::deleteRule(int id) {
    auto* storage = db_.getStorage();
    if (!storage) {
        DICE_LOG_ERROR("ReplyManager: database not open");
        return false;
    }

    try {
        storage->remove_all<ReplyRuleRow>(
            orm::where(orm::c(&ReplyRuleRow::id) == id)
        );

        // Reload to keep cache consistent
        loadRules();

        DICE_LOG_INFO("ReplyManager: deleted rule id={}", id);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ReplyManager: failed to delete rule id={}: {}", id, e.what());
        return false;
    }
}

bool ReplyManager::toggleRule(int id) {
    auto* storage = db_.getStorage();
    if (!storage) {
        DICE_LOG_ERROR("ReplyManager: database not open");
        return false;
    }

    try {
        auto rows = storage->get_all<ReplyRuleRow>(
            orm::where(orm::c(&ReplyRuleRow::id) == id)
        );

        if (rows.empty()) {
            DICE_LOG_WARN("ReplyManager: rule id={} not found for toggle", id);
            return false;
        }

        ReplyRuleRow row = rows[0];
        row.enabled = !row.enabled;
        row.updatedAt = utils::nowIso8601();
        storage->update(row);

        // Reload to keep cache consistent
        loadRules();

        DICE_LOG_INFO("ReplyManager: toggled rule id={} to enabled={}", id, row.enabled);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ReplyManager: failed to toggle rule id={}: {}", id, e.what());
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════
// Matching
// ═══════════════════════════════════════════════════════════════

std::vector<ReplyRule> ReplyManager::matchMessage(const std::string& msg) const {
    std::vector<ReplyRule> matched;
    if (msg.empty()) return matched;

    for (const auto& rule : rules_) {
        if (!rule.enabled) continue;

        // Evaluate all conditions with the rule's logic (AND = every condition
        // must match; OR = any). Single-condition rules behave as before.
        bool isAnd = (rule.logic == "and");
        bool ok = isAnd;   // AND starts true (all), OR starts false (any)
        const ReplyCondition* hit = nullptr;   // the condition that drives {$N} capture groups
        for (const auto& c : rule.conditions) {
            bool m = matcher_.match(msg, c.content, c.type);
            if (isAnd) {
                if (!m) { ok = false; break; }
                // AND: all match; prefer a regex condition for capture groups, else first.
                if (!hit || (c.type == MatchType::kRegex && hit->type != MatchType::kRegex)) hit = &c;
            } else {
                if (m) { ok = true; hit = &c; break; }   // OR: the condition that triggered
            }
        }
        if (ok && !rule.conditions.empty()) {
            ReplyRule r = rule;
            // Render capture groups ({$1}…) from the condition that actually matched,
            // not always conditions[0] — fixes OR rules where a later regex triggered.
            if (hit) { r.matchContent = hit->content; r.matchType = hit->type; }
            matched.push_back(std::move(r));
        }
    }

    // Already sorted by loadRules (priority desc, id asc)
    DICE_LOG_DEBUG("ReplyManager: matched {}/{} rules for message (len={})",
        matched.size(), rules_.size(), msg.size());

    return matched;
}

// ═══════════════════════════════════════════════════════════════
// Queries
// ═══════════════════════════════════════════════════════════════

std::string ReplyManager::pickResult(const ReplyRule& rule) const {
    if (rule.results.empty()) return rule.replyContent;
    if (rule.results.size() == 1) return rule.results[0];
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, rule.results.size() - 1);
    return rule.results[dist(gen)];
}

const ReplyRule* ReplyManager::getRuleById(int id) const {
    for (const auto& rule : rules_) {
        if (rule.id == id) return &rule;
    }
    return nullptr;
}

}  // namespace dice
