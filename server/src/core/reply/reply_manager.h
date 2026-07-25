#pragma once

#include "reply_matcher.h"
#include "../../common/types.h"
#include "../../storage/database.h"
#include "../../config/config_manager.h"

#include <string>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

/**
 * @brief A single custom reply rule stored in the database.
 *
 * Maps to the `reply_rules` table in SQLite.
 */
/// One match condition (enhanced engine: a rule may have several).
struct ReplyCondition {
    MatchType type = MatchType::kKeyword;
    std::string content;
};

struct ReplyRule {
    int id = 0;
    MatchType matchType = MatchType::kKeyword;   // = conditions[0] (back-compat)
    std::string matchContent;                    // = conditions[0].content (back-compat)
    std::string replyContent;                    // = results[0] (back-compat)
    bool enabled = true;
    int priority = 0;
    std::string createdAt;   // ISO 8601
    std::string updatedAt;   // ISO 8601
    // Enhanced engine:
    std::vector<ReplyCondition> conditions;      // ≥1 condition
    std::string logic = "or";                    // "and" | "or"
    std::vector<std::string> results;            // ≥1 result (random pick on hit)

    json toJSON() const {
        json j;
        j["id"]            = id;
        j["match_type"]    = matchTypeToString(matchType);
        j["match_content"] = matchContent;
        j["reply_content"] = replyContent;
        j["enabled"]       = enabled;
        j["priority"]      = priority;
        j["created_at"]    = createdAt;
        j["updated_at"]    = updatedAt;
        j["logic"]         = logic;
        j["conditions"]    = json::array();
        for (const auto& c : conditions)
            j["conditions"].push_back({{"type", matchTypeToString(c.type)}, {"content", c.content}});
        j["results"]       = results;
        return j;
    }
};

/**
 * @brief Reply rule storage and matching manager.
 *
 * Holds an in-memory vector of ReplyRule and provides the
 * `matchMessage()` API that returns all rules (sorted by
 * priority) matching a given input message.
 *
 * Hot-reload: the constructor registers a callback on the
 * ConfigManager that reloads rules from the database.
 */
class ReplyManager {
public:
    /**
     * @brief Construct and register a hot-reload callback.
     * @param db         Database reference for loading rules.
     * @param configMgr  ConfigManager for hot-reload callback registration.
     */
    ReplyManager(Database& db, ConfigManager& configMgr);

    // ─── CRUD ────────────────────────────────────────────────

    /**
     * @brief Load all reply rules from the database into memory.
     */
    void loadRules();

    /**
     * @brief Add a new reply rule (persists to DB).
     * @param rule  The rule to add (id will be auto-generated).
     * @return The newly assigned rule id.
     */
    int addRule(const ReplyRule& rule);

    /**
     * @brief Update an existing rule by id.
     * @param id    Rule id.
     * @param rule  New rule data.
     * @return true if the rule was found and updated.
     */
    bool updateRule(int id, const ReplyRule& rule);

    /**
     * @brief Delete a rule by id.
     * @param id  Rule id.
     * @return true if the rule was found and deleted.
     */
    bool deleteRule(int id);

    /**
     * @brief Toggle a rule's enabled state.
     * @param id  Rule id.
     * @return true if the rule was found and toggled.
     */
    bool toggleRule(int id);

    // ─── Matching ────────────────────────────────────────────

    /**
     * @brief Find all reply rules that match the given message.
     *
     * Rules are sorted by priority (descending), with ties
     * broken by id (ascending). Only enabled rules are considered.
     *
     * @param msg  The user message text to match against.
     * @return Vector of matching ReplyRule (may be empty).
     */
    std::vector<ReplyRule> matchMessage(const std::string& msg) const;

    /// Pick one result template for a matched rule (random among results).
    std::string pickResult(const ReplyRule& rule) const;

    // ─── Queries ─────────────────────────────────────────────

    /**
     * @brief List all cached reply rules.
     * @return Const reference to the full rules vector.
     */
    const std::vector<ReplyRule>& listRules() const noexcept { return rules_; }

    /**
     * @brief Get a single rule by id.
     * @param id  Rule id.
     * @return Pointer to the rule, or nullptr if not found.
     */
    const ReplyRule* getRuleById(int id) const;

private:
    Database& db_;
    ConfigManager& configMgr_;
    ReplyMatcher matcher_;
    std::vector<ReplyRule> rules_;

    /**
     * @brief Helper to read a single rule row from the database.
     */
    ReplyRule readRuleFromRow(const struct ReplyRuleRow& row);
};

}  // namespace dice
