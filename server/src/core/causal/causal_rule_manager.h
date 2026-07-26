#pragma once
// ─── Dice!Next v3.0.0 — Causal Rule Manager ───────────
// Low-code causal rule engine: condition matching + action execution.
// Rules are stored in the causal_rules table and loaded into memory.
// Matching is prioritized over regular ReplyManager (checked first).
//
// Condition types:
//   keyword/prefix/regex/search — text matching (reuses ReplyMatcher logic)
//   user_filter/group_filter    — whitelist/blacklist ID matching
//   cooldown                    — per-rule cooldown check
//   counter_check               — compare counter value (>=, <=, ==, !=)
//
// Action types:
//   reply                       — pick a random template and render it
//   counter_add/counter_set/counter_reset — modify a persistent counter
//   api_call                    — fetch an external API and inject the result

#include "../../storage/database.h"
#include "../../config/config_manager.h"
#include "../reply/reply_manager.h"   // for MatchType
#include "cooldown_manager.h"
#include "counter_store.h"

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

// ─── Enums ───────────────────────────────────────────────────

enum class CausalCondType {
    Keyword, Prefix, Regex, Search,
    UserFilter, GroupFilter,
    Cooldown, CounterCheck
};

enum class CausalActionType {
    Reply, CounterSet, CounterAdd, CounterReset, ApiCall
};

// ─── Data structures ─────────────────────────────────────────

struct CausalCondition {
    CausalCondType type = CausalCondType::Keyword;
    std::string content;          // match text / filter config / counter name
    std::string op;               // counter_check: ">=", "<=", "==", "!="
    int value = 0;                // counter_check: comparison value
    std::string counterName;      // counter_check: counter name
};

struct CausalAction {
    CausalActionType type = CausalActionType::Reply;
    std::vector<std::string> replies;  // reply: random-pick templates
    std::string counterName;           // counter_*: counter name
    std::string counterScope;          // counter_*: "per-user"|"per-group"|"global"
    int counterDelta = 0;              // counter_add: delta; counter_set: value
    std::string apiUrl;                // api_call: URL
    std::string apiVar;                // api_call: variable name to inject
};

struct CausalRule {
    int id = 0;
    std::string name;
    std::string scope = "global";    // "global" | "group" | "user"
    std::vector<std::string> scopeIds;
    bool enabled = true;
    int priority = 100;
    int cooldownMs = 0;
    std::string cooldownKey = "per-user";  // "per-user" | "per-group" | "global"
    std::vector<CausalCondition> conditions;
    std::string logic = "or";        // "and" | "or"
    std::vector<CausalAction> actions;
    std::string createdAt;
    std::string updatedAt;

    json toJSON() const;
    static CausalRule fromJSON(const json& j);
};

struct CounterChange {
    std::string name;
    int oldValue = 0;
    int newValue = 0;
};

struct CausalMatchResult {
    bool matched = false;
    std::string reply;
    int ruleId = 0;
    std::string ruleName;
    std::vector<CounterChange> counterChanges;
};

// ─── Manager ─────────────────────────────────────────────────

class CausalRuleManager {
public:
    CausalRuleManager(Database& db, ConfigManager& cfg,
                      CooldownManager& cooldownMgr, CounterStore& counterStore);

    /// Load all rules from the database into memory (sorted by priority desc).
    void loadRules();

    /// Add a new rule. Returns the new rule ID (or -1 on failure).
    int addRule(const CausalRule& rule);

    /// Update an existing rule. Returns true on success.
    bool updateRule(int id, const CausalRule& rule);

    /// Delete a rule (and its counters). Returns true on success.
    bool deleteRule(int id);

    /// Toggle a rule's enabled state. Returns the new state (or -1 on error).
    bool toggleRule(int id);

    /// Match a message against all enabled rules (priority desc).
    /// If matched, executes actions and returns the result.
    /// @p dryRun = true → don't trigger cooldown/counter changes (for testing).
    CausalMatchResult matchAndExecute(const std::string& msg,
                                       const std::string& userId,
                                       const std::string& groupId,
                                       const std::string& nick,
                                       bool dryRun = false);

    /// List all rules (memory cache).
    const std::vector<CausalRule>& listRules() const { return rules_; }

    /// Get a rule by ID. Returns nullptr if not found.
    const CausalRule* getRuleById(int id) const;

private:
    Database& db_;
    ConfigManager& cfg_;
    CooldownManager& cooldownMgr_;
    CounterStore& counterStore_;
    std::vector<CausalRule> rules_;

    // ─── Helpers ─────────────────────────────────────────────

    /// Check if a rule's scope applies to the given user/group.
    bool checkScope(const CausalRule& rule,
                    const std::string& userId,
                    const std::string& groupId) const;

    /// Evaluate a single condition. Returns true if the condition is met.
    bool evalCondition(const CausalCondition& cond,
                       const std::string& msg,
                       const std::string& userId,
                       const std::string& groupId,
                       int ruleId,
                       bool dryRun) const;

    /// Evaluate all conditions with AND/OR logic. Returns true if the rule fires.
    bool evalConditions(const CausalRule& rule,
                        const std::string& msg,
                        const std::string& userId,
                        const std::string& groupId,
                        bool dryRun) const;

    /// Build the cooldown key for a rule + user/group context.
    std::string buildCooldownKey(const CausalRule& rule,
                                  const std::string& userId,
                                  const std::string& groupId) const;

    /// Build the counter key for an action + user/group context.
    std::string buildCounterKey(int ruleId, const std::string& counterName,
                                 const std::string& scope,
                                 const std::string& userId,
                                 const std::string& groupId) const;

    /// Execute a rule's actions. Returns the reply text (may be empty).
    std::string executeActions(const CausalRule& rule,
                                const std::string& msg,
                                const std::string& userId,
                                const std::string& groupId,
                                const std::string& nick,
                                std::vector<CounterChange>& changes,
                                bool dryRun);

    /// Text matching (reuses ReplyMatcher logic inline).
    bool matchText(const std::string& msg, const std::string& pattern,
                   CausalCondType type) const;

    /// Parse a filter string ("whitelist:id1,id2" or "blacklist:id3").
    /// Returns (isWhitelist, list_of_ids).
    static std::pair<bool, std::vector<std::string>> parseFilter(const std::string& content);

public:
    /// Convert enum to/from string for JSON serialization.
    /// Public: CausalRule::to/fromJSON (a separate class) calls these.
    static const char* condTypeStr(CausalCondType t);
    static CausalCondType condTypeFromStr(const std::string& s);
    static const char* actionTypeStr(CausalActionType t);
    static CausalActionType actionTypeFromStr(const std::string& s);
};

}  // namespace dice
