#include "locale_resolver.h"
#include "../storage/database.h"
#include "../config/config_manager.h"
#include "../common/logger.h"

#include <sqlite_orm/sqlite_orm.h>

namespace dice {

namespace orm = sqlite_orm;

LocaleResolver::LocaleResolver(Database& db, ConfigManager& cfg)
    : db_(db), cfg_(cfg) {}

// ═══════════════════════════════════════════════════════════════
// Resolution
// ═══════════════════════════════════════════════════════════════

Locale LocaleResolver::resolve(const Message& msg) const {
    const std::string& platform = msg.platform;

    // 1. Most-specific scope override.
    if (msg.type == MessageType::kPrivate) {
        if (auto loc = getOverride("user", platform + ":" + msg.senderId))
            return *loc;
    } else {
        if (auto loc = getOverride("group", platform + ":" + msg.targetId))
            return *loc;
    }

    // 2. Platform default.
    if (auto loc = platformDefault(platform))
        return *loc;

    // 3. Global default.
    return globalDefault();
}

// ═══════════════════════════════════════════════════════════════
// Defaults (from config)
// ═══════════════════════════════════════════════════════════════

Locale LocaleResolver::globalDefault() const {
    std::string code = cfg_.get<std::string>("i18n/default_locale", std::string("zh-Hans"));
    return localeFromString(code);
}

std::optional<Locale> LocaleResolver::platformDefault(const std::string& platform) const {
    if (platform.empty()) return std::nullopt;
    try {
        // config: { "i18n": { "platform_defaults": { "onebot_v11": "zh-Hans", ... } } }
        json all = cfg_.getAll();
        if (all.contains("i18n") &&
            all["i18n"].contains("platform_defaults") &&
            all["i18n"]["platform_defaults"].contains(platform)) {
            return localeFromString(
                all["i18n"]["platform_defaults"][platform].get<std::string>());
        }
    } catch (const std::exception& e) {
        DICE_LOG_DEBUG("LocaleResolver: platformDefault lookup failed: {}", e.what());
    }
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// Override storage (DB)
// ═══════════════════════════════════════════════════════════════

std::optional<Locale> LocaleResolver::getOverride(const std::string& scope,
                                                  const std::string& scopeKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return std::nullopt;
    try {
        auto rows = st->get_all<LocaleSettingRow>(
            orm::where(orm::c(&LocaleSettingRow::scope) == scope
                       and orm::c(&LocaleSettingRow::scopeKey) == scopeKey));
        if (!rows.empty()) {
            return localeFromString(rows.front().locale);
        }
    } catch (const std::exception& e) {
        DICE_LOG_WARN("LocaleResolver: getOverride failed: {}", e.what());
    }
    return std::nullopt;
}

bool LocaleResolver::setOverride(const std::string& scope,
                                 const std::string& scopeKey,
                                 Locale locale) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return false;
    try {
        auto rows = st->get_all<LocaleSettingRow>(
            orm::where(orm::c(&LocaleSettingRow::scope) == scope
                       and orm::c(&LocaleSettingRow::scopeKey) == scopeKey));
        if (rows.empty()) {
            LocaleSettingRow row;
            row.scope = scope;
            row.scopeKey = scopeKey;
            row.locale = localeToString(locale);
            st->insert(row);
        } else {
            LocaleSettingRow row = rows.front();
            row.locale = localeToString(locale);
            st->update(row);
        }
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("LocaleResolver: setOverride failed: {}", e.what());
        return false;
    }
}

bool LocaleResolver::clearOverride(const std::string& scope, const std::string& scopeKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return false;
    try {
        st->remove_all<LocaleSettingRow>(
            orm::where(orm::c(&LocaleSettingRow::scope) == scope
                       and orm::c(&LocaleSettingRow::scopeKey) == scopeKey));
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("LocaleResolver: clearOverride failed: {}", e.what());
        return false;
    }
}

}  // namespace dice
