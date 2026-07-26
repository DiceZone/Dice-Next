#pragma once

#include <string>
#include <vector>
#include <memory>

namespace dice {

class Database;
class LegacyImporter;

/**
 * @brief Schema version management and automated upgrade framework.
 *
 * The `migrations` table tracks the current schema version.
 * On startup, Migration checks the version and applies any
 * outstanding upgrade scripts sequentially.
 *
 * Current target: V4
 *   - V1: Baseline (migrations table only)
 *   - V2: Full schema (reply_rules, dice_config, decks, adapters,
 *         sessions, banlist, legacy_import_log)
 *   - V3: Causal rule engine (causal_rules, rule_counters)
 *   - V4: Persona switching system (persona_templates, persona_entries)
 */
class Migration {
public:
    /// Current target schema version.
    static constexpr int kTargetVersion = 4;

    explicit Migration(Database& db);
    ~Migration() = default;

    // ─── Public API ──────────────────────────────────────────

    /**
     * @brief Check the current schema version and apply any pending
     *        upgrades. Returns true if the database is at the target
     *        version after the call.
     */
    bool migrate();

    /**
     * @brief Get the current schema version from the database.
     */
    int getCurrentVersion();

    /**
     * @brief Check whether a legacy Dice! data directory exists at
     *        the given path and whether it appears to contain old-
     *        format data that should be imported.
     *
     * Also checks the legacy_import_log to avoid re-importing
     * files that have already been processed.
     */
    bool needsLegacyImport(const std::string& legacyPath);

private:
    Database& db_;

    // ─── Migration steps ────────────────────────────────────

    /// Initial baseline — creates the migrations tracking table.
    bool migrateToV1();

    /// V1 → V2: Create all application tables via sync_schema.
    /// This is the full schema upgrade adding reply_rules,
    /// dice_config, decks, adapters, sessions, banlist,
    /// and legacy_import_log.
    bool migrateV1toV2();

    /// V2 → V3: Add causal_rules and rule_counters tables for the
    /// causal rule engine. sync_schema creates the new tables
    /// and adds any columns missing from older schemas.
    bool migrateV2toV3();

    /// V3 → V4: Add persona_templates and persona_entries tables for
    /// the persona switching system. sync_schema creates the
    /// new tables.
    bool migrateV3toV4();

    /// Record the current version in the migrations table.
    void setVersion(int version);
};

}  // namespace dice
