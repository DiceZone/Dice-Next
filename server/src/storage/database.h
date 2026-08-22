#pragma once

#include <string>
#include <memory>
#include <sqlite_orm/sqlite_orm.h>

namespace dice {

namespace orm = sqlite_orm;

// ═══════════════════════════════════════════════════════════════
// Row structs — one per table (required by sqlite_orm)
// ═══════════════════════════════════════════════════════════════

/// Schema migration tracking table.
struct MigrationRecord {
    int version = 0;
    std::string appliedAt;
};

/// Custom reply rules table.
struct ReplyRuleRow {
    int id = 0;
    int matchType = 0;        // Cast from MatchType (legacy / first condition)
    std::string matchContent; // legacy / first condition content
    std::string replyContent; // legacy / first result
    bool enabled = true;
    int priority = 0;
    std::string createdAt;
    std::string updatedAt;
    // Enhanced engine (back-compat: empty → use the legacy single fields above):
    std::string conditions;   // JSON array of {"type":"keyword|prefix|regex|search","content":"…"}
    std::string logic;        // "and" | "or" (how to combine conditions; default "or")
    std::string results;      // JSON array of reply strings (random pick on hit)
    // 触发限制（原版 DiceTriggerLimit 的常用子集，以前只有因果规则才有）：
    int prob = 100;           // 触发概率 0-100（100=必回）
    int cooldownSec = 0;      // 冷却秒数（按 规则×群/私聊按人；0=无冷却）
    std::string scopeMode;    // ""=不限 | "allow"=仅列表内群 | "deny"=排除列表内群
    std::string scopeIds;     // 逗号分隔群号（配合 scopeMode）
    std::string cooldownNotice;   // 冷却中回这句（原版 cd@echo；空=沉默）
    int dayLimit = 0;             // 每日触发上限（按 规则×窗口；原版 today 默认 Chat 维度；0=不限）
    std::string dayLimitNotice;   // 达到日限回这句（原版 daylimit_notice；空=沉默）
    std::string scopeUsersMode;   // ""=不限 | "allow"=仅列表内用户 | "deny"=排除列表内用户
    std::string scopeUsers;       // 逗号分隔用户ID（原版 user_id 白/黑名单）
};

/// Key-value dice configuration (extensible rule storage).
struct DiceConfigRow {
    int id = 0;
    std::string key;
    std::string value;         // JSON-encoded
};

/// A stable logical QQ identity. publicId is always a numeric QQ/群号: a real
/// number when known, otherwise a reserved virtual number for QQ Official.
struct IdentityRow {
    int id = 0;
    std::string kind;          // "user" | "group"
    std::string publicId;      // real QQ number or reserved virtual number
    bool isVirtual = true;
    std::string createdAt;
};

/// A platform endpoint attached to an IdentityRow.  Official OpenIDs are kept
/// in endpointId and never mixed into the public QQ/群号 field.
struct IdentityEndpointRow {
    int id = 0;
    int identityId = 0;
    std::string kind;          // "user" | "group"
    std::string adapterType;   // e.g. onebot_v11 / qq_official
    std::string adapterAccount;// QQ Official AppID; OneBot adapter id
    std::string endpointId;    // raw QQ number or official OpenID
    std::string createdAt;
};

/// Deck / card collection table.
struct DeckRow {
    int id = 0;
    std::string name;
    std::string cards;          // JSON-encoded card list
    std::string createdAt;
};

/// Adapter configuration table.
struct AdapterRow {
    int id = 0;
    std::string name;
    int type = 0;               // Cast from AdapterType
    int connectionMode = 0;     // Cast from ConnectionMode
    std::string endpoint;
    std::string accessToken;
    bool enabled = true;
    std::string config;         // JSON-encoded
};

/// Game recording log table.
struct GameLogRow {
    int id = 0;
    std::string groupId;
    std::string gmId;
    std::string name;           // log name (.log new <name>); "" → "log<id>"
    std::string players;        // JSON-encoded
    std::string customRules;    // JSON-encoded
    int status = 0;             // Cast from GameLogStatus
    std::string createdAt;
};

/// Ban / block list table.
struct BanlistRow {
    int id = 0;
    int targetType = 0;         // 0=user, 1=group, ...
    std::string targetId;
    int listType = 0;           // 0=blacklist, 1=whitelist
    std::string reason;
    std::string createdAt;
};

/// Log of previously imported legacy files (prevents re-import).
struct LegacyImportLogRow {
    int id = 0;
    std::string filePath;
    std::string importedAt;     // ISO 8601
    int recordCount = 0;
};

/// Per-scope locale (i18n) override table.
/// Resolution order is "platform default + overridable": a row here
/// overrides the configured default for a specific scope.
///   scope    — "global" | "platform" | "group" | "user"
///   scopeKey — depends on scope:
///                global   → "" (single row)
///                platform → platform name, e.g. "onebot_v11" / "discord"
///                group    → "<platform>:<groupOrChannelId>"
///                user     → "<platform>:<userId>"
///   locale   — BCP-47 code, e.g. "zh-Hant" / "zh-Hans" / "en"
struct LocaleSettingRow {
    int id = 0;
    std::string scope;
    std::string scopeKey;
    std::string locale;
};

/// Character card. Cards are named and global to a user (identified by
/// userId + name); the empty name "" is the default unnamed card. Which card
/// is "active" in a given group/scope is a per-(user, scope) binding stored in
/// user_settings (key "cardBind"). Attributes are a JSON object {name: number}.
/// groupId is retained for legacy rows but no longer part of the card key.
struct CharacterCardRow {
    int id = 0;
    std::string userId;
    std::string groupId;     // legacy / informational (scope where last touched)
    std::string name;        // card name; "" = default unnamed card
    std::string attrs;       // JSON object, e.g. {"力量":50,"理智":65}
    std::string updatedAt;
};

/// Per-(user, group) string settings: nickname (.nn), default dice (.set), ...
struct UserSettingRow {
    int id = 0;
    std::string userId;
    std::string groupId;
    std::string key;         // "nick" | "defaultDice" | ...
    std::string value;
};

/// A single recorded line in a game log transcript (.log recording).
struct GameLogMessageRow {
    int id = 0;
    int logId = 0;           // → game_logs.id
    std::string messageId;   // platform source message id; empty for bot/web lines
    std::string sender;      // display name of the speaker (or bot name)
    std::string userId;      // speaker's platform id (bot's selfId for replies)
    std::string content;
    std::string createdAt;   // ISO 8601
    std::string images;      // JSON 数组，本条消息的图片引用（URL 或本地落地文件名）
};

/// persisted group-chat message (chat.db). Mirrors the simulated-chat
/// window: incoming group messages + bot replies + web sends. Retained N days
/// (config chat/retention_days); `recalled` marks撤回 but content is kept.
struct ChatMsgRow {
    int64_t id = 0;
    std::string platform;
    std::string adapterId;   // concrete adapter account; empty for historical rows
    std::string groupId;
    std::string msgId;       // platform message id (empty for outgoing/web sends)
    std::string userId;      // speaker id (bot selfId for replies)
    std::string sender;      // display name
    std::string content;     // display text (CQ 原文，前端渲染图片)
    int self = 0;            // 1 = sent by the bot
    int64_t time = 0;        // unix epoch seconds
    int recalled = 0;        // 1 = 已撤回（仍保留内容展示）
};

/// AI 记忆（chat.db）。阶段 B：滚动对话摘要（kind="summary"，每群一行）；
/// 阶段 C 复用同表存持久事实（kind="fact"，带 embedding 向量做检索）。
struct AiMemoryRow {
    int64_t id = 0;
    std::string scope;       // "group" | "user" | ...
    std::string scopeId;     // 例如 "onebot_v11:123456"
    std::string kind;        // "summary" | "fact"
    std::string content;     // 摘要/事实文本
    int64_t refId = 0;       // summary：已折叠进摘要的最新 chat_messages.id（水位线）
    int importance = 0;      // 阶段 C 排序用
    std::string embedding;   // 阶段 C：JSON 浮点数组，阶段 B 留空
    int64_t createdAt = 0;   // unix 秒
    int64_t updatedAt = 0;   // unix 秒
    int hits = 0;            // 被检索命中次数
};

/// User override for a localized template (editable reply text). The bundle
/// default always remains in the JSON files; this just layers on top.
struct I18nOverrideRow {
    int id = 0;
    std::string locale;   // BCP-47 code, e.g. "zh-Hant"
    std::string key;      // dotted i18n key, e.g. "dice.roll.result"
    std::string value;
    std::string format = "plain"; // plain | markdown; legacy rows stay literal
};

/// Per-(platform, group) settings: bot enabled flag (.bot on/off), and future
/// group-management fields (bot card name, group remark, ...). Managed from chat
/// and, later, the web admin panel.
struct GroupSettingRow {
    int id = 0;
    std::string platform;
    std::string groupId;
    std::string key;         // "enabled" | "card" | "remark" | ...
    std::string value;
};

/// Settings that belong to one concrete adapter account in a shared group.
/// GroupSettingRow remains the shared/public-group layer (for example name and
/// remark); switches and runtime state live here so two bots in the same group
/// cannot overwrite each other.
struct GroupAccountSettingRow {
    int id = 0;
    std::string adapterId;   // stable AdapterRow id
    std::string platform;
    std::string groupId;     // public/shared group id
    std::string endpointId;  // native id used by this adapter
    std::string key;
    std::string value;
};

/// Player profile — auto-created the first time a user triggers the bot (even a
/// single .r). Powers the web 玩家管理 page.
struct PlayerProfileRow {
    int id = 0;
    std::string platform;
    std::string userId;
    std::string nickname;     // last-seen display name
    int trustLevel = 0;       // 信任等级 (0=普通, higher=trusted; matches banlist whitelist idea)
    int cmdCount = 0;         // 总指令数
    int groupCmdCount = 0;    // 群聊中成功触发的指令数（好友申请审批使用）
    int favor = 0;            // 好感度 (DiceFavor)
    std::string lastCmdAt;    // 上次指令时间 (ISO 8601)
    std::string createdAt;    // 首次建档时间 (ISO 8601)
};

/// Per-(platform, user) skill-check roll statistics (.hiy 骰点统计). One row per
/// user; counters accumulate over every COC check (.ra/.rb/.rp).
struct RollStatRow {
    int id = 0;
    std::string platform;
    std::string userId;
    std::string skill;   // canonical skill name; "" = legacy/overall row
    int total = 0;       // total checks counted
    int crit = 0;        // 大成功
    int extreme = 0;     // 极难成功
    int hard = 0;        // 困难成功
    int regular = 0;     // 成功
    int fail = 0;        // 失败
    int fumble = 0;      // 大失败
};

/// Hourly aggregate of commands and individual dice samples. This keeps the
/// statistics page useful without retaining message text.
struct UsageHourRow {
    int id = 0;
    std::string day;     // local date, YYYY-MM-DD
    int hour = 0;        // local hour, 0-23
    long long commandCount = 0;
    long long rollCount = 0;
};

/// Distribution of raw faces from simple NdM rolls.
struct DiceFaceStatRow {
    int id = 0;
    int sides = 0;
    int face = 0;
    long long count = 0;
};

/// Periodic adapter availability sample for the local statistics page.
struct OnlineSampleRow {
    int id = 0;
    std::string sampledAt; // UTC ISO 8601
    int onlineCount = 0;
    int totalCount = 0;
};

/// 定时任务 (#48): a message pushed to a target group/user on a daily schedule.
struct ScheduledTaskRow {
    int id = 0;
    std::string name;        // 任务名（展示用）
    std::string adapterId;   // 指定适配器帐号（空=按 platform 任选该平台已连接适配器）
    std::string platform;    // 目标平台 (onebot_v11/…)
    std::string targetType;  // "group" | "private"
    std::string targetId;    // 群号 / QQ
    std::string cronTime;    // 触发时刻 "HH:MM"（每日）
    std::string days;        // 星期过滤，逗号分隔 0-6(0=周日)；空=每天
    std::string content;     // 发送内容（支持 {self}{group}{date}{roll:..}{draw:..} 等变量/函数）
    int enabled = 1;
    std::string lastRun;     // daily: 上次触发日期 "YYYY-MM-DD"；interval/once: "YYYY-MM-DD HH:MM"
    std::string createdAt;
    std::string action;      // 因果动作："send"(默认,发内容) | "leave"(退群,内容作告别语)
    std::string condition;   // 因果条件（空=无条件）。如 "inactive>=7"：本群 ≥7 天无指令才触发
    std::string triggerType; // "daily"(默认,每日 cronTime+days) | "interval"(每 N 分钟) | "once"(onceDate+cronTime 执行一次后自动停用)
    int intervalMin = 0;     // interval: 间隔分钟
    std::string onceDate;    // once: 执行日期 "YYYY-MM-DD"
};

/// Causal rule row — a rule with conditions, actions, cooldown, and scope.
struct CausalRuleRow {
    int id = 0;
    std::string name;
    std::string scope;           // 'global' | 'group' | 'user'
    std::string scopeIds;        // JSON array of group/user IDs
    bool enabled = true;
    int priority = 100;
    int cooldownMs = 0;
    std::string cooldownKey;     // 'per-user' | 'per-group' | 'global'
    std::string conditions;      // JSON array of CausalCondition
    std::string logic;           // 'and' | 'or'
    std::string actions;         // JSON array of CausalAction
    std::string createdAt;
    std::string updatedAt;
};

/// Rule counter row — persistent counter for causal rules.
struct RuleCounterRow {
    int id = 0;
    std::string key;             // '{ruleId}:{counterName}:{scope}:{scopeId}'
    int value = 0;
    std::string updatedAt;
};

/// Persona template row — a named set of i18n overrides.
struct PersonaTemplateRow {
    int id = 0;
    std::string name;            // unique persona name (e.g. "傲娇", "严肃")
    std::string description;     // human-readable description
    bool isBuiltin = false;      // true = built-in (cannot delete)
    std::string createdAt;
    std::string updatedAt;
};

/// Persona entry row — a single (locale, key, value) override.
struct PersonaEntryRow {
    int id = 0;
    int personaId = 0;           // → persona_templates.id
    std::string locale;          // BCP-47 code, e.g. "zh-Hans"
    std::string key;             // dotted i18n key, e.g. "dice.roll.result"
    std::string value;           // override text
    std::string format = "plain"; // plain | markdown
};

// ═══════════════════════════════════════════════════════════════
// Database — SQLite connection manager
// ═══════════════════════════════════════════════════════════════

class Database {
public:
    Database();
    ~Database();

    // ─── Lifecycle ───────────────────────────────────────────

    /// Open (or create) the SQLite database at the given path. The transcript log
    /// store (game_logs / game_log_messages) is opened as a SEPARATE file
    /// "logs.db" in the same directory, so it can be deleted independently to
    /// clear log data. Creates all tables via sync_schema. Returns true on success.
    bool open(const std::string& path);

    /// Close the database connection gracefully.
    void close();

    /// Whether the database is currently open.
    bool isOpen() const noexcept { return storage_ != nullptr; }

    /// Get the underlying sqlite_orm storage (main DB).
    /// Returns nullptr if not open.
    auto* getStorage() { return storage_.get(); }

    /// Get the transcript-log storage (separate logs.db). Returns nullptr if not open.
    auto* getLogStorage() { return logStorage_.get(); }

    /// Get the character-card storage (separate cards.db). Returns nullptr if not open.
    auto* getCardStorage() { return cardStorage_.get(); }

    /// get the chat-history storage (separate chat.db). Returns nullptr if not open.
    auto* getChatStorage() { return chatStorage_.get(); }

    /// Path of the separate transcript-log database file.
    const std::string& logPath() const noexcept { return logDbPath_; }
    /// Path of the separate character-card database file.
    const std::string& cardPath() const noexcept { return cardDbPath_; }
    /// Path of the separate chat-history database file.
    const std::string& chatPath() const noexcept { return chatDbPath_; }

    /// Flush WAL files for every store before a filesystem backup is made.
    bool checkpoint();

    /// Execute a raw SQL statement. Returns true on success.
    bool execute(const std::string& sql);

    /// Get the database file path.
    const std::string& path() const noexcept { return dbPath_; }

private:
    std::string dbPath_;

    // ─── ORM Storage type (all tables registered here) ───────

    using Storage = decltype(orm::make_storage(
        "",
        orm::make_table("migrations",
            orm::make_column("version", &MigrationRecord::version),
            orm::make_column("applied_at", &MigrationRecord::appliedAt)
        ),
        orm::make_table("reply_rules",
            orm::make_column("id", &ReplyRuleRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("match_type", &ReplyRuleRow::matchType),
            orm::make_column("match_content", &ReplyRuleRow::matchContent),
            orm::make_column("reply_content", &ReplyRuleRow::replyContent),
            orm::make_column("enabled", &ReplyRuleRow::enabled),
            orm::make_column("priority", &ReplyRuleRow::priority),
            orm::make_column("created_at", &ReplyRuleRow::createdAt),
            orm::make_column("updated_at", &ReplyRuleRow::updatedAt),
            orm::make_column("conditions", &ReplyRuleRow::conditions),
            orm::make_column("logic", &ReplyRuleRow::logic),
            orm::make_column("results", &ReplyRuleRow::results),
            orm::make_column("prob", &ReplyRuleRow::prob, orm::default_value(100)),
            orm::make_column("cooldown_sec", &ReplyRuleRow::cooldownSec, orm::default_value(0)),
            orm::make_column("scope_mode", &ReplyRuleRow::scopeMode, orm::default_value("")),
            orm::make_column("scope_ids", &ReplyRuleRow::scopeIds, orm::default_value("")),
            orm::make_column("cooldown_notice", &ReplyRuleRow::cooldownNotice, orm::default_value("")),
            orm::make_column("day_limit", &ReplyRuleRow::dayLimit, orm::default_value(0)),
            orm::make_column("day_limit_notice", &ReplyRuleRow::dayLimitNotice, orm::default_value("")),
            orm::make_column("scope_users_mode", &ReplyRuleRow::scopeUsersMode, orm::default_value("")),
            orm::make_column("scope_users", &ReplyRuleRow::scopeUsers, orm::default_value(""))
        ),
        orm::make_table("dice_config",
            orm::make_column("id", &DiceConfigRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("key", &DiceConfigRow::key,
                orm::unique()),
            orm::make_column("value", &DiceConfigRow::value)
        ),
        orm::make_table("identities",
            orm::make_column("id", &IdentityRow::id, orm::primary_key().autoincrement()),
            orm::make_column("kind", &IdentityRow::kind),
            orm::make_column("public_id", &IdentityRow::publicId),
            orm::make_column("is_virtual", &IdentityRow::isVirtual),
            orm::make_column("created_at", &IdentityRow::createdAt)
        ),
        orm::make_table("identity_endpoints",
            orm::make_column("id", &IdentityEndpointRow::id, orm::primary_key().autoincrement()),
            orm::make_column("identity_id", &IdentityEndpointRow::identityId),
            orm::make_column("kind", &IdentityEndpointRow::kind),
            orm::make_column("adapter_type", &IdentityEndpointRow::adapterType),
            orm::make_column("adapter_account", &IdentityEndpointRow::adapterAccount),
            orm::make_column("endpoint_id", &IdentityEndpointRow::endpointId),
            orm::make_column("created_at", &IdentityEndpointRow::createdAt)
        ),
        orm::make_table("decks",
            orm::make_column("id", &DeckRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("name", &DeckRow::name),
            orm::make_column("cards", &DeckRow::cards),
            orm::make_column("created_at", &DeckRow::createdAt)
        ),
        orm::make_table("adapters",
            orm::make_column("id", &AdapterRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("name", &AdapterRow::name),
            orm::make_column("type", &AdapterRow::type),
            orm::make_column("connection_mode", &AdapterRow::connectionMode),
            orm::make_column("endpoint", &AdapterRow::endpoint),
            orm::make_column("access_token", &AdapterRow::accessToken),
            orm::make_column("enabled", &AdapterRow::enabled),
            orm::make_column("config", &AdapterRow::config)
        ),
        // NOTE: game_logs / game_log_messages live in a SEPARATE database
        // (LogStorage / logs.db) so the (potentially huge) transcript store can be
        // deleted on its own to clear log data. See LogStorage below.
        orm::make_table("banlist",
            orm::make_column("id", &BanlistRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("target_type", &BanlistRow::targetType),
            orm::make_column("target_id", &BanlistRow::targetId),
            orm::make_column("list_type", &BanlistRow::listType),
            orm::make_column("reason", &BanlistRow::reason),
            orm::make_column("created_at", &BanlistRow::createdAt)
        ),
        orm::make_table("legacy_import_log",
            orm::make_column("id", &LegacyImportLogRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("file_path", &LegacyImportLogRow::filePath),
            orm::make_column("imported_at", &LegacyImportLogRow::importedAt),
            orm::make_column("record_count", &LegacyImportLogRow::recordCount)
        ),
        orm::make_table("locale_settings",
            orm::make_column("id", &LocaleSettingRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("scope", &LocaleSettingRow::scope),
            orm::make_column("scope_key", &LocaleSettingRow::scopeKey),
            orm::make_column("locale", &LocaleSettingRow::locale)
        ),
        orm::make_table("character_cards",
            orm::make_column("id", &CharacterCardRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("user_id", &CharacterCardRow::userId),
            orm::make_column("group_id", &CharacterCardRow::groupId),
            orm::make_column("name", &CharacterCardRow::name),
            orm::make_column("attrs", &CharacterCardRow::attrs),
            orm::make_column("updated_at", &CharacterCardRow::updatedAt)
        ),
        orm::make_table("user_settings",
            orm::make_column("id", &UserSettingRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("user_id", &UserSettingRow::userId),
            orm::make_column("group_id", &UserSettingRow::groupId),
            orm::make_column("key", &UserSettingRow::key),
            orm::make_column("value", &UserSettingRow::value)
        ),
        orm::make_table("group_settings",
            orm::make_column("id", &GroupSettingRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("platform", &GroupSettingRow::platform),
            orm::make_column("group_id", &GroupSettingRow::groupId),
            orm::make_column("key", &GroupSettingRow::key),
            orm::make_column("value", &GroupSettingRow::value)
        ),
        orm::make_table("group_account_settings",
            orm::make_column("id", &GroupAccountSettingRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("adapter_id", &GroupAccountSettingRow::adapterId),
            orm::make_column("platform", &GroupAccountSettingRow::platform),
            orm::make_column("group_id", &GroupAccountSettingRow::groupId),
            orm::make_column("endpoint_id", &GroupAccountSettingRow::endpointId),
            orm::make_column("key", &GroupAccountSettingRow::key),
            orm::make_column("value", &GroupAccountSettingRow::value)
        ),
        orm::make_table("i18n_overrides",
            orm::make_column("id", &I18nOverrideRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("locale", &I18nOverrideRow::locale),
            orm::make_column("key", &I18nOverrideRow::key),
            orm::make_column("value", &I18nOverrideRow::value),
            orm::make_column("format", &I18nOverrideRow::format, orm::default_value("plain"))
        ),
        orm::make_table("player_profiles",
            orm::make_column("id", &PlayerProfileRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("platform", &PlayerProfileRow::platform),
            orm::make_column("user_id", &PlayerProfileRow::userId),
            orm::make_column("nickname", &PlayerProfileRow::nickname),
            orm::make_column("trust_level", &PlayerProfileRow::trustLevel),
            orm::make_column("cmd_count", &PlayerProfileRow::cmdCount),
            orm::make_column("group_cmd_count", &PlayerProfileRow::groupCmdCount, orm::default_value(0)),
            orm::make_column("favor", &PlayerProfileRow::favor),
            orm::make_column("last_cmd_at", &PlayerProfileRow::lastCmdAt),
            orm::make_column("created_at", &PlayerProfileRow::createdAt)
        ),
        orm::make_table("roll_stats",
            orm::make_column("id", &RollStatRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("platform", &RollStatRow::platform),
            orm::make_column("user_id", &RollStatRow::userId),
            orm::make_column("skill", &RollStatRow::skill),
            orm::make_column("total", &RollStatRow::total),
            orm::make_column("crit", &RollStatRow::crit),
            orm::make_column("extreme", &RollStatRow::extreme),
            orm::make_column("hard", &RollStatRow::hard),
            orm::make_column("regular", &RollStatRow::regular),
            orm::make_column("fail", &RollStatRow::fail),
            orm::make_column("fumble", &RollStatRow::fumble)
        ),
        orm::make_table("usage_hours",
            orm::make_column("id", &UsageHourRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("day", &UsageHourRow::day),
            orm::make_column("hour", &UsageHourRow::hour),
            orm::make_column("command_count", &UsageHourRow::commandCount),
            orm::make_column("roll_count", &UsageHourRow::rollCount)
        ),
        orm::make_table("dice_face_stats",
            orm::make_column("id", &DiceFaceStatRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("sides", &DiceFaceStatRow::sides),
            orm::make_column("face", &DiceFaceStatRow::face),
            orm::make_column("count", &DiceFaceStatRow::count)
        ),
        orm::make_table("online_samples",
            orm::make_column("id", &OnlineSampleRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("sampled_at", &OnlineSampleRow::sampledAt),
            orm::make_column("online_count", &OnlineSampleRow::onlineCount),
            orm::make_column("total_count", &OnlineSampleRow::totalCount)
        ),
        orm::make_table("scheduled_tasks",
            orm::make_column("id", &ScheduledTaskRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("name", &ScheduledTaskRow::name),
            orm::make_column("adapter_id", &ScheduledTaskRow::adapterId),
            orm::make_column("platform", &ScheduledTaskRow::platform),
            orm::make_column("target_type", &ScheduledTaskRow::targetType),
            orm::make_column("target_id", &ScheduledTaskRow::targetId),
            orm::make_column("cron_time", &ScheduledTaskRow::cronTime),
            orm::make_column("days", &ScheduledTaskRow::days),
            orm::make_column("content", &ScheduledTaskRow::content),
            orm::make_column("enabled", &ScheduledTaskRow::enabled),
            orm::make_column("last_run", &ScheduledTaskRow::lastRun),
            orm::make_column("created_at", &ScheduledTaskRow::createdAt),
            orm::make_column("action", &ScheduledTaskRow::action),
            orm::make_column("cond", &ScheduledTaskRow::condition),
            orm::make_column("trigger_type", &ScheduledTaskRow::triggerType, orm::default_value("")),
            orm::make_column("interval_min", &ScheduledTaskRow::intervalMin, orm::default_value(0)),
            orm::make_column("once_date", &ScheduledTaskRow::onceDate, orm::default_value(""))
        ),
        // Causal rule engine tables
        orm::make_table("causal_rules",
            orm::make_column("id", &CausalRuleRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("name", &CausalRuleRow::name),
            orm::make_column("scope", &CausalRuleRow::scope),
            orm::make_column("scope_ids", &CausalRuleRow::scopeIds),
            orm::make_column("enabled", &CausalRuleRow::enabled),
            orm::make_column("priority", &CausalRuleRow::priority),
            orm::make_column("cooldown_ms", &CausalRuleRow::cooldownMs),
            orm::make_column("cooldown_key", &CausalRuleRow::cooldownKey),
            orm::make_column("conditions", &CausalRuleRow::conditions),
            orm::make_column("logic", &CausalRuleRow::logic),
            orm::make_column("actions", &CausalRuleRow::actions),
            orm::make_column("created_at", &CausalRuleRow::createdAt),
            orm::make_column("updated_at", &CausalRuleRow::updatedAt)
        ),
        orm::make_table("rule_counters",
            orm::make_column("id", &RuleCounterRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("key", &RuleCounterRow::key,
                orm::unique()),
            orm::make_column("value", &RuleCounterRow::value),
            orm::make_column("updated_at", &RuleCounterRow::updatedAt)
        ),
        // Persona switching system tables
        orm::make_table("persona_templates",
            orm::make_column("id", &PersonaTemplateRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("name", &PersonaTemplateRow::name,
                orm::unique()),
            orm::make_column("description", &PersonaTemplateRow::description),
            orm::make_column("is_builtin", &PersonaTemplateRow::isBuiltin),
            orm::make_column("created_at", &PersonaTemplateRow::createdAt),
            orm::make_column("updated_at", &PersonaTemplateRow::updatedAt)
        ),
        orm::make_table("persona_entries",
            orm::make_column("id", &PersonaEntryRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("persona_id", &PersonaEntryRow::personaId),
            orm::make_column("locale", &PersonaEntryRow::locale),
            orm::make_column("key", &PersonaEntryRow::key),
            orm::make_column("value", &PersonaEntryRow::value),
            orm::make_column("format", &PersonaEntryRow::format, orm::default_value("plain"))
        )
    ));

    // ─── Separate transcript-log storage (logs.db) ───────────
    // Kept in its own file so it can grow large and be deleted on its own to
    // wipe log data without touching the main database.
    using LogStorage = decltype(orm::make_storage(
        "",
        orm::make_table("game_logs",
            orm::make_column("id", &GameLogRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("group_id", &GameLogRow::groupId),
            orm::make_column("gm_id", &GameLogRow::gmId),
            orm::make_column("name", &GameLogRow::name),
            orm::make_column("players", &GameLogRow::players),
            orm::make_column("custom_rules", &GameLogRow::customRules),
            orm::make_column("status", &GameLogRow::status),
            orm::make_column("created_at", &GameLogRow::createdAt)
        ),
        orm::make_table("game_log_messages",
                    orm::make_column("id", &GameLogMessageRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("log_id", &GameLogMessageRow::logId),
                    orm::make_column("message_id", &GameLogMessageRow::messageId),
                    orm::make_column("sender", &GameLogMessageRow::sender),
            orm::make_column("user_id", &GameLogMessageRow::userId),
            orm::make_column("content", &GameLogMessageRow::content),
            orm::make_column("created_at", &GameLogMessageRow::createdAt),
            orm::make_column("images", &GameLogMessageRow::images)
        )
    ));

    // ─── separate chat-history storage (chat.db) ────────
    // Group-chat persistence for the simulated-chat window; retained N days.
    using ChatStorage = decltype(orm::make_storage(
        "",
        orm::make_table("chat_messages",
            orm::make_column("id", &ChatMsgRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("platform", &ChatMsgRow::platform),
            orm::make_column("adapter_id", &ChatMsgRow::adapterId),
            orm::make_column("group_id", &ChatMsgRow::groupId),
            orm::make_column("msg_id", &ChatMsgRow::msgId),
            orm::make_column("user_id", &ChatMsgRow::userId),
            orm::make_column("sender", &ChatMsgRow::sender),
            orm::make_column("content", &ChatMsgRow::content),
            orm::make_column("self", &ChatMsgRow::self),
            orm::make_column("time", &ChatMsgRow::time),
            orm::make_column("recalled", &ChatMsgRow::recalled)
        ),
        orm::make_table("ai_memory",
            orm::make_column("id", &AiMemoryRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("scope", &AiMemoryRow::scope),
            orm::make_column("scope_id", &AiMemoryRow::scopeId),
            orm::make_column("kind", &AiMemoryRow::kind),
            orm::make_column("content", &AiMemoryRow::content),
            orm::make_column("ref_id", &AiMemoryRow::refId),
            orm::make_column("importance", &AiMemoryRow::importance),
            orm::make_column("embedding", &AiMemoryRow::embedding),
            orm::make_column("created_at", &AiMemoryRow::createdAt),
            orm::make_column("updated_at", &AiMemoryRow::updatedAt),
            orm::make_column("hits", &AiMemoryRow::hits)
        )
    ));

    // ─── Separate character-card storage (cards.db) ──────────
    // Cards live in their own file so they can be backed up / wiped separately
    // from the main DB. character_cards is ALSO mapped in the main Storage above
    // only so a one-time migration can read legacy rows out of dice.db.
    using CardStorage = decltype(orm::make_storage(
        "",
        orm::make_table("character_cards",
            orm::make_column("id", &CharacterCardRow::id,
                orm::primary_key().autoincrement()),
            orm::make_column("user_id", &CharacterCardRow::userId),
            orm::make_column("group_id", &CharacterCardRow::groupId),
            orm::make_column("name", &CharacterCardRow::name),
            orm::make_column("attrs", &CharacterCardRow::attrs),
            orm::make_column("updated_at", &CharacterCardRow::updatedAt)
        )
    ));

    std::unique_ptr<Storage> storage_;
    std::unique_ptr<LogStorage> logStorage_;
    std::string logDbPath_;
    std::unique_ptr<CardStorage> cardStorage_;
    std::string cardDbPath_;
    std::unique_ptr<ChatStorage> chatStorage_;   // 
    std::string chatDbPath_;
};

}  // namespace dice
