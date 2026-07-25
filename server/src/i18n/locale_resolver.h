#pragma once
// ─── Dice!Next v3.0.0 — Locale Resolver ──────────────────────
// Decides which language to use for a given incoming message,
// implementing the "platform default + overridable" policy:
//
//   1. Scope override (DB locale_settings)
//        - group/channel message → "group" scope, key "<platform>:<targetId>"
//        - private message       → "user"  scope, key "<platform>:<senderId>"
//   2. Platform default  (config i18n.platform_defaults.<platform>)
//   3. Global default    (config i18n.default_locale, else zh-Hans)
//
// Overrides live in the database so they hot-reload and can be edited
// from the web panel later, without restarting the bot.

#include "../common/types.h"
#include "../adapter/adapter_interface.h"

#include <string>
#include <optional>
#include <mutex>
#include <vector>

namespace dice {

class Database;
class ConfigManager;

class LocaleResolver {
public:
    LocaleResolver(Database& db, ConfigManager& cfg);

    /// Resolve the locale to use for replying to @p msg.
    Locale resolve(const Message& msg) const;

    // ─── Override management (used by the REST API / web panel) ──

    /// Upsert an override row. @p scope ∈ {global, platform, group, user}.
    bool setOverride(const std::string& scope,
                     const std::string& scopeKey,
                     Locale locale);

    /// Remove an override. Returns true if a row was deleted.
    bool clearOverride(const std::string& scope, const std::string& scopeKey);

    /// Read a single override if present.
    std::optional<Locale> getOverride(const std::string& scope,
                                      const std::string& scopeKey) const;

private:
    Locale globalDefault() const;
    std::optional<Locale> platformDefault(const std::string& platform) const;

    Database& db_;
    ConfigManager& cfg_;
    mutable std::mutex mutex_;
};

}  // namespace dice
