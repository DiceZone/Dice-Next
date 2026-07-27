#pragma once

#include "reply_matcher.h"
#include "../../common/types.h"
#include "../../storage/database.h"
#include "../../config/config_manager.h"

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
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
    // 触发限制（原版 DiceTriggerLimit 常用子集）：
    int prob = 100;              // 触发概率 0-100（100=必回）
    int cooldownSec = 0;         // 冷却秒（同一规则×同一群/私聊按人；0=无冷却）
    std::string scopeMode;       // ""=不限 | "allow"=仅列表内群 | "deny"=排除列表内群
    std::string scopeIds;        // 逗号分隔群号
    std::string cooldownNotice;  // 冷却中回这句（原版 cd@echo；空=沉默跳过）
    int dayLimit = 0;            // 每日触发上限（按 规则×窗口；0=不限）
    std::string dayLimitNotice;  // 达到日限回这句（空=沉默跳过）
    std::string scopeUsersMode;  // ""=不限 | "allow"=仅列表内用户 | "deny"=排除列表内用户
    std::string scopeUsers;      // 逗号分隔用户ID

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
        j["prob"]          = prob;
        j["cooldown_sec"]  = cooldownSec;
        j["scope_mode"]    = scopeMode;
        j["scope_ids"]     = scopeIds;
        j["cooldown_notice"]  = cooldownNotice;
        j["day_limit"]        = dayLimit;
        j["day_limit_notice"] = dayLimitNotice;
        j["scope_users_mode"] = scopeUsersMode;
        j["scope_users"]      = scopeUsers;
        return j;
    }
};

/// 触发上下文（scope/冷却需要知道消息来自哪）。私聊 groupId 为空。
struct ReplyCtx {
    std::string platform;
    std::string groupId;
    std::string userId;
};

/// pickReply 的结果：选中的规则（拷贝，matchContent/matchType 已换成实际命中的
/// 条件，{$N} 捕获组按它取）+ 被限制条件跳过的规则（供网页测试面板展示）。
/// notice 非空 = 规则被冷却/日限拦下但设置了提示语（原版 cd@echo/限额回复）——
/// 调用方应把 notice 当回复发出，此时 rule 为空。
struct ReplyPick {
    struct Skip { int id; std::string reason; };   // "scope" | "prob" | "cooldown" | "daylimit"
    std::optional<ReplyRule> rule;
    std::vector<ReplyPick::Skip> skipped;
    std::string notice;
    int noticeRuleId = 0;
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

    // ─── Matching ────────────────────────────────────────────

    /**
     * @brief Find all reply rules that match the given message.
     *
     * Rules are sorted by priority (descending), ties broken by
     * match-mode specificity (keyword > prefix > search > regex,
     * 对齐原版 Match→Prefix→Search→Regex 的隐式次序), then id.
     * Only enabled rules are considered. 纯文本匹配——不看
     * scope/概率/冷却；完整触发管线请用 pickReply。
     *
     * @param msg  The user message text to match against.
     * @return Vector of matching ReplyRule (may be empty).
     */
    std::vector<ReplyRule> matchMessage(const std::string& msg) const;

    /**
     * @brief 完整触发管线：文本匹配 → 生效范围(scope) → 冷却 → 概率，
     *        返回第一条全部通过的规则（被限制跳过的规则继续尝试下一条）。
     * @param msg     消息文本。
     * @param ctx     触发上下文（平台/群/用户）。
     * @param commit  true=真实触发（记录冷却时间戳、掷概率）；
     *                false=网页测试（冷却只检查不消耗、概率不掷，只报告）。
     */
    ReplyPick pickReply(const std::string& msg, const ReplyCtx& ctx, bool commit = true);

    /// Pick one result template for a matched rule (random among results).
    std::string pickResult(const ReplyRule& rule) const;

    // ─── Queries ─────────────────────────────────────────────

    /**
     * @brief List all cached reply rules（不可变快照——热重载随时换列表，
     *        引用会悬空；调用方持有 shared_ptr 即安全）。
     */
    std::shared_ptr<const std::vector<ReplyRule>> listRules() const { return snapshot(); }

private:
    Database& db_;
    ConfigManager& configMgr_;
    ReplyMatcher matcher_;
    // 规则快照：读者拿 shared_ptr 遍历，loadRules 整体换指针。
    // 以前是裸 vector——网页保存规则重载时消息线程可能正在遍历，数据竞争。
    std::shared_ptr<const std::vector<ReplyRule>> rules_ =
        std::make_shared<const std::vector<ReplyRule>>();
    mutable std::mutex rulesMutex_;
    // 冷却状态（内存态，重启清零）：key = "<ruleId>|g<群号>" 或 "<ruleId>|u<用户>"。
    std::map<std::string, int64_t> cooldownAt_;
    // 每日触发计数（内存态）：key 同上，跨天整表清空。
    std::map<std::string, int> dayCount_;
    std::string dayCountDate_;   // 计数所属日期 "YYYY-MM-DD"
    std::mutex cooldownMutex_;   // 保护 cooldownAt_ + dayCount_

    std::shared_ptr<const std::vector<ReplyRule>> snapshot() const {
        std::lock_guard<std::mutex> lock(rulesMutex_);
        return rules_;
    }

    /// 逗号分隔名单里是否含 id（忽略两端空白）。
    static bool csvHas(const std::string& csv, const std::string& id);
    /// 规则是否对该会话生效（群 allow/deny 名单；私聊仅受 allow 名单排斥）。
    static bool scopeAllows(const ReplyRule& rule, const ReplyCtx& ctx);
    /// 规则是否对该用户生效（用户 allow/deny 名单）。
    static bool userAllows(const ReplyRule& rule, const ReplyCtx& ctx);

    /**
     * @brief Helper to read a single rule row from the database.
     */
    ReplyRule readRuleFromRow(const struct ReplyRuleRow& row);
};

}  // namespace dice
