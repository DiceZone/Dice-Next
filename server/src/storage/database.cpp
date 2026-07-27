#include "database.h"
#include "../common/logger.h"

#include <sqlite_orm/sqlite_orm.h>
#include <sqlite3.h>
#include <filesystem>
#include <fstream>

namespace dice {

// ─── Database Lifecycle ──────────────────────────────────────

Database::Database() = default;

Database::~Database() {
    close();
}

bool Database::open(const std::string& path) {
    if (isOpen()) {
        DICE_LOG_WARN("Database: already open, closing first");
        close();
    }

    dbPath_ = path;

    try {
        // Ensure the parent directory exists
        std::filesystem::path dbDir = std::filesystem::path(path).parent_path();
        if (!dbDir.empty() && !std::filesystem::exists(dbDir)) {
            std::filesystem::create_directories(dbDir);
            DICE_LOG_INFO("Database: created directory '{}'", dbDir.string());
        }

        // Construct storage with all registered tables
        storage_ = std::make_unique<Storage>(
            orm::make_storage(
                path,
                // ── migrations ──
                orm::make_table("migrations",
                    orm::make_column("version", &MigrationRecord::version),
                    orm::make_column("applied_at", &MigrationRecord::appliedAt)
                ),
                // ── reply_rules ──
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
                // ── dice_config ──
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
                // ── decks ──
                orm::make_table("decks",
                    orm::make_column("id", &DeckRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("name", &DeckRow::name),
                    orm::make_column("cards", &DeckRow::cards),
                    orm::make_column("created_at", &DeckRow::createdAt)
                ),
                // ── adapters ──
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
                // ── (game_logs / game_log_messages moved to logs.db) ──
                // ── banlist ──
                orm::make_table("banlist",
                    orm::make_column("id", &BanlistRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("target_type", &BanlistRow::targetType),
                    orm::make_column("target_id", &BanlistRow::targetId),
                    orm::make_column("list_type", &BanlistRow::listType),
                    orm::make_column("reason", &BanlistRow::reason),
                    orm::make_column("created_at", &BanlistRow::createdAt)
                ),
                // ── legacy_import_log ──
                orm::make_table("legacy_import_log",
                    orm::make_column("id", &LegacyImportLogRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("file_path", &LegacyImportLogRow::filePath),
                    orm::make_column("imported_at", &LegacyImportLogRow::importedAt),
                    orm::make_column("record_count", &LegacyImportLogRow::recordCount)
                ),
                // ── locale_settings (i18n overrides) ──
                orm::make_table("locale_settings",
                    orm::make_column("id", &LocaleSettingRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("scope", &LocaleSettingRow::scope),
                    orm::make_column("scope_key", &LocaleSettingRow::scopeKey),
                    orm::make_column("locale", &LocaleSettingRow::locale)
                ),
                // ── character_cards ──
                orm::make_table("character_cards",
                    orm::make_column("id", &CharacterCardRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("user_id", &CharacterCardRow::userId),
                    orm::make_column("group_id", &CharacterCardRow::groupId),
                    orm::make_column("name", &CharacterCardRow::name),
                    orm::make_column("attrs", &CharacterCardRow::attrs),
                    orm::make_column("updated_at", &CharacterCardRow::updatedAt)
                ),
                // ── user_settings ──
                orm::make_table("user_settings",
                    orm::make_column("id", &UserSettingRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("user_id", &UserSettingRow::userId),
                    orm::make_column("group_id", &UserSettingRow::groupId),
                    orm::make_column("key", &UserSettingRow::key),
                    orm::make_column("value", &UserSettingRow::value)
                ),
                // ── group_settings ──
                orm::make_table("group_settings",
                    orm::make_column("id", &GroupSettingRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("platform", &GroupSettingRow::platform),
                    orm::make_column("group_id", &GroupSettingRow::groupId),
                    orm::make_column("key", &GroupSettingRow::key),
                    orm::make_column("value", &GroupSettingRow::value)
                ),
                // ── i18n_overrides ──
                orm::make_table("i18n_overrides",
                    orm::make_column("id", &I18nOverrideRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("locale", &I18nOverrideRow::locale),
                    orm::make_column("key", &I18nOverrideRow::key),
                    orm::make_column("value", &I18nOverrideRow::value)
                ),
                // ── player_profiles ──
                orm::make_table("player_profiles",
                    orm::make_column("id", &PlayerProfileRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("platform", &PlayerProfileRow::platform),
                    orm::make_column("user_id", &PlayerProfileRow::userId),
                    orm::make_column("nickname", &PlayerProfileRow::nickname),
                    orm::make_column("trust_level", &PlayerProfileRow::trustLevel),
                    orm::make_column("cmd_count", &PlayerProfileRow::cmdCount),
                    orm::make_column("favor", &PlayerProfileRow::favor),
                    orm::make_column("last_cmd_at", &PlayerProfileRow::lastCmdAt),
                    orm::make_column("created_at", &PlayerProfileRow::createdAt)
                ),
                // ── roll_stats (.hiy 骰点统计) ──
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
                orm::make_table("scheduled_tasks",
                    orm::make_column("id", &ScheduledTaskRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("name", &ScheduledTaskRow::name),
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
                // low-code causal rule engine
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
                    orm::make_column("value", &PersonaEntryRow::value)
                )
            )
        );

        // Sync schema — create tables if they don't exist
        storage_->sync_schema();
        storage_->open_forever();  // keep connection alive with WAL mode

        DICE_LOG_INFO("Database: opened '{}' ({} tables)", path,
            storage_->table_names().size());

        // ── Separate transcript-log database (logs.db, same directory) ──
        // Lives in its own file so it can grow large and be deleted on its own
        // to clear log data without touching the main DB.
        logDbPath_ = (std::filesystem::path(path).parent_path() / "logs.db").string();
        logStorage_ = std::make_unique<LogStorage>(
            orm::make_storage(
                logDbPath_,
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
                    orm::make_column("sender", &GameLogMessageRow::sender),
                    orm::make_column("user_id", &GameLogMessageRow::userId),
                    orm::make_column("content", &GameLogMessageRow::content),
                    orm::make_column("created_at", &GameLogMessageRow::createdAt),
                    orm::make_column("images", &GameLogMessageRow::images)
                )
            )
        );
        logStorage_->sync_schema();
        logStorage_->open_forever();
        DICE_LOG_INFO("Database: opened log store '{}' ({} tables)", logDbPath_,
            logStorage_->table_names().size());

        // ── separate chat-history database (chat.db, same directory) ──
        chatDbPath_ = (std::filesystem::path(path).parent_path() / "chat.db").string();
        chatStorage_ = std::make_unique<ChatStorage>(
            orm::make_storage(
                chatDbPath_,
                orm::make_table("chat_messages",
                    orm::make_column("id", &ChatMsgRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("platform", &ChatMsgRow::platform),
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
            )
        );
        chatStorage_->sync_schema();
        chatStorage_->open_forever();
        DICE_LOG_INFO("Database: opened chat store '{}' ({} tables)", chatDbPath_,
            chatStorage_->table_names().size());

        // ── Separate character-card database (cards.db, same directory) ──
        cardDbPath_ = (std::filesystem::path(path).parent_path() / "cards.db").string();
        cardStorage_ = std::make_unique<CardStorage>(
            orm::make_storage(
                cardDbPath_,
                orm::make_table("character_cards",
                    orm::make_column("id", &CharacterCardRow::id,
                        orm::primary_key().autoincrement()),
                    orm::make_column("user_id", &CharacterCardRow::userId),
                    orm::make_column("group_id", &CharacterCardRow::groupId),
                    orm::make_column("name", &CharacterCardRow::name),
                    orm::make_column("attrs", &CharacterCardRow::attrs),
                    orm::make_column("updated_at", &CharacterCardRow::updatedAt)
                )
            )
        );
        cardStorage_->sync_schema();
        cardStorage_->open_forever();

        // One-time migration: if cards.db is empty but the legacy character_cards
        // table in the main DB has rows, move them over (then clear from main).
        try {
            if (cardStorage_->count<CharacterCardRow>() == 0) {
                auto legacy = storage_->get_all<CharacterCardRow>();
                if (!legacy.empty()) {
                    for (auto row : legacy) { row.id = 0; cardStorage_->insert(row); }
                    storage_->remove_all<CharacterCardRow>();
                    DICE_LOG_INFO("Database: migrated {} character cards → cards.db", legacy.size());
                }
            }
        } catch (const std::exception& e) {
            DICE_LOG_WARN("Database: card migration skipped: {}", e.what());
        }
        DICE_LOG_INFO("Database: opened card store '{}' ({} tables)", cardDbPath_,
            cardStorage_->table_names().size());
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Database: failed to open '{}': {}", path, e.what());
        storage_.reset();
        return false;
    }
}

void Database::close() {
    if (chatStorage_) {
        try { chatStorage_.reset(); } catch (...) {}
    }
    if (cardStorage_) {
        try { cardStorage_.reset(); } catch (...) {}
    }
    if (logStorage_) {
        try { logStorage_.reset(); } catch (...) {}
    }
    if (storage_) {
        try {
            storage_.reset();
            DICE_LOG_INFO("Database: closed '{}'", dbPath_);
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("Database: error closing: {}", e.what());
        }
        storage_.reset();
    }
}

bool Database::execute(const std::string& sql) {
    if (!isOpen()) {
        DICE_LOG_ERROR("Database: execute called but not open");
        return false;
    }

    try {
        sqlite3* rawDb = nullptr;
        int rc = sqlite3_open(dbPath_.c_str(), &rawDb);
        if (rc != SQLITE_OK) {
            DICE_LOG_ERROR("Database: execute — cannot open raw connection: {}", 
                rawDb ? sqlite3_errmsg(rawDb) : "unknown");
            if (rawDb) sqlite3_close(rawDb);
            return false;
        }
        char* errMsg = nullptr;
        rc = sqlite3_exec(rawDb, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            DICE_LOG_ERROR("Database: execute failed: {}", errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
        }
        sqlite3_close(rawDb);
        return rc == SQLITE_OK;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Database: execute failed: {}", e.what());
        return false;
    }
}

}  // namespace dice
