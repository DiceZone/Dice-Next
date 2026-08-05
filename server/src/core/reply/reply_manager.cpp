#include "reply_manager.h"
#include "../../common/logger.h"
#include "../../common/utils.h"

#include <algorithm>
#include <ctime>
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

    DICE_LOG_INFO("ReplyManager: initialized with {} rules", snapshot()->size());
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

        // 在本地建好完整列表再整体换指针——正在遍历旧快照的消息线程不受影响。
        auto next = std::make_shared<std::vector<ReplyRule>>();
        next->reserve(rows.size());

        for (const auto& row : rows) {
            ReplyRule rule;
            rule.id             = row.id;
            rule.matchType      = static_cast<MatchType>(row.matchType);
            rule.matchContent   = row.matchContent;
            rule.replyContent   = row.replyContent;
            rule.enabled        = row.enabled;
            rule.priority       = row.priority;
            rule.createdAt      = row.createdAt;
            rule.updatedAt      = row.updatedAt;
            rule.prob           = row.prob;
            rule.cooldownSec    = row.cooldownSec;
            rule.scopeMode      = row.scopeMode;
            rule.scopeIds       = row.scopeIds;
            rule.cooldownNotice = row.cooldownNotice;
            rule.dayLimit       = row.dayLimit;
            rule.dayLimitNotice = row.dayLimitNotice;
            rule.scopeUsersMode = row.scopeUsersMode;
            rule.scopeUsers     = row.scopeUsers;
            normalizeRule(rule, row.conditions, row.logic, row.results);
            next->push_back(std::move(rule));
        }

        // 匹配模式特异度（同优先级时精确的先赢，对齐原版 Match→Prefix→Search→Regex
        // 的隐式次序；否则一条高分的「包含」规则会盖过明明写了完全匹配的规则）。
        auto specRank = [](const ReplyRule& r) {
            auto rank = [](MatchType t) {
                switch (t) {
                    case MatchType::kKeyword: return 0;
                    case MatchType::kPrefix:  return 1;
                    case MatchType::kSearch:  return 2;
                    case MatchType::kRegex:   return 3;
                }
                return 3;
            };
            int best = 3;
            for (const auto& c : r.conditions) best = (std::min)(best, rank(c.type));
            return best;
        };
        // Sort by priority desc, then specificity, then id asc
        std::sort(next->begin(), next->end(),
            [&specRank](const ReplyRule& a, const ReplyRule& b) {
                if (a.priority != b.priority) return a.priority > b.priority;
                int sa = specRank(a), sb = specRank(b);
                if (sa != sb) return sa < sb;
                return a.id < b.id;
            });

        DICE_LOG_INFO("ReplyManager: loaded {} rules", next->size());
        std::lock_guard<std::mutex> lock(rulesMutex_);
        rules_ = std::move(next);
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
        row.prob         = rule.prob;
        row.cooldownSec  = rule.cooldownSec;
        row.scopeMode    = rule.scopeMode;
        row.scopeIds     = rule.scopeIds;
        row.cooldownNotice = rule.cooldownNotice;
        row.dayLimit       = rule.dayLimit;
        row.dayLimitNotice = rule.dayLimitNotice;
        row.scopeUsersMode = rule.scopeUsersMode;
        row.scopeUsers     = rule.scopeUsers;
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
        row.prob         = rule.prob;
        row.cooldownSec  = rule.cooldownSec;
        row.scopeMode    = rule.scopeMode;
        row.scopeIds     = rule.scopeIds;
        row.cooldownNotice = rule.cooldownNotice;
        row.dayLimit       = rule.dayLimit;
        row.dayLimitNotice = rule.dayLimitNotice;
        row.scopeUsersMode = rule.scopeUsersMode;
        row.scopeUsers     = rule.scopeUsers;
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

// ═══════════════════════════════════════════════════════════════
// Matching
// ═══════════════════════════════════════════════════════════════

bool ReplyManager::csvHas(const std::string& csv, const std::string& id) {
    if (csv.empty() || id.empty()) return false;
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t comma = csv.find(',', pos);
        std::string item = csv.substr(pos,
            comma == std::string::npos ? std::string::npos : comma - pos);
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b != std::string::npos && item.substr(b, e - b + 1) == id) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

bool ReplyManager::scopeAllows(const ReplyRule& rule, const ReplyCtx& ctx) {
    if (rule.scopeMode.empty() || rule.scopeIds.empty()) return true;
    // 私聊不在任何群名单里：allow 名单 → 不触发；deny 名单 → 放行。
    const bool inList = !ctx.groupId.empty() && csvHas(rule.scopeIds, ctx.groupId);
    if (rule.scopeMode == "allow") return inList;
    if (rule.scopeMode == "deny")  return !inList;
    return true;
}

bool ReplyManager::userAllows(const ReplyRule& rule, const ReplyCtx& ctx) {
    if (rule.scopeUsersMode.empty() || rule.scopeUsers.empty()) return true;
    const bool inList = csvHas(rule.scopeUsers, ctx.userId);
    if (rule.scopeUsersMode == "allow") return inList;
    if (rule.scopeUsersMode == "deny")  return !inList;
    return true;
}

ReplyPick ReplyManager::pickReply(const std::string& msg, const ReplyCtx& ctx, bool commit) {
    ReplyPick pick;
    auto matches = matchMessage(msg);
    if (matches.empty()) return pick;

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    static thread_local std::mt19937 probGen(std::random_device{}());
    // 日计数所属日期（配置时区）；跨天清空整表。
    const std::string ymd = utils::formatTimeInTimezone(std::time(nullptr), "%Y-%m-%d");

    for (auto& r : matches) {
        if (!scopeAllows(r, ctx)) { pick.skipped.push_back({r.id, "scope"}); continue; }
        if (!userAllows(r, ctx))  { pick.skipped.push_back({r.id, "scope"}); continue; }
        const std::string key = std::to_string(r.id) + "|"
            + (ctx.groupId.empty() ? ("u" + ctx.userId) : ("g" + ctx.groupId));
        // 冷却：冷却中 → 有提示语则回提示语（原版 cd@echo），否则沉默让下条接话。
        if (r.cooldownSec > 0) {
            std::lock_guard<std::mutex> lock(cooldownMutex_);
            auto it = cooldownAt_.find(key);
            if (it != cooldownAt_.end() && now - it->second < r.cooldownSec) {
                pick.skipped.push_back({r.id, "cooldown"});
                if (!r.cooldownNotice.empty()) { pick.notice = r.cooldownNotice; pick.noticeRuleId = r.id; return pick; }
                continue;
            }
        }
        // 每日上限（按 规则×窗口，原版 today 默认 Chat 维度）：同冷却的提示语语义。
        if (r.dayLimit > 0) {
            std::lock_guard<std::mutex> lock(cooldownMutex_);
            if (dayCountDate_ != ymd) { dayCount_.clear(); dayCountDate_ = ymd; }
            auto it = dayCount_.find(key);
            if (it != dayCount_.end() && it->second >= r.dayLimit) {
                pick.skipped.push_back({r.id, "daylimit"});
                if (!r.dayLimitNotice.empty()) { pick.notice = r.dayLimitNotice; pick.noticeRuleId = r.id; return pick; }
                continue;
            }
        }
        if (commit && r.prob < 100) {
            // 概率不命中 → 本条不回，也不落到低优先级规则上（抽签失败就是
            // 这条规则「这次不说话」，而不是换别的规则说）。冷却/日限不消耗。
            if (std::uniform_int_distribution<int>(0, 99)(probGen) >= (std::max)(0, r.prob)) {
                pick.skipped.push_back({r.id, "prob"});
                return pick;
            }
        }
        if (commit) {
            std::lock_guard<std::mutex> lock(cooldownMutex_);
            if (r.cooldownSec > 0) cooldownAt_[key] = now;   // 真正要回了才开始计冷却
            if (r.dayLimit > 0) {
                if (dayCountDate_ != ymd) { dayCount_.clear(); dayCountDate_ = ymd; }
                ++dayCount_[key];
            }
        }
        pick.rule = std::move(r);
        return pick;
    }
    return pick;
}

std::vector<ReplyRule> ReplyManager::matchMessage(const std::string& msg) const {
    std::vector<ReplyRule> matched;
    if (msg.empty()) return matched;

    auto snap = snapshot();
    for (const auto& rule : *snap) {
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
        matched.size(), snap->size(), msg.size());

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

}  // namespace dice
