#include "migration.h"
#include "database.h"
#include "legacy_importer.h"
#include "../common/logger.h"
#include "../common/utils.h"

#include <filesystem>
#include <sqlite_orm/sqlite_orm.h>

namespace dice {

namespace fs = std::filesystem;

// ─── Constructor ─────────────────────────────────────────────

Migration::Migration(Database& db) : db_(db) {}

// ─── Public API ──────────────────────────────────────────────

bool Migration::migrate() {
    DICE_LOG_INFO("Migration: checking schema version (target: {})", kTargetVersion);

    int current = getCurrentVersion();

    if (current >= kTargetVersion) {
        DICE_LOG_INFO("Migration: schema is current (V{})", current);
        return true;
    }

    DICE_LOG_INFO("Migration: upgrading from V{} to V{}", current, kTargetVersion);

    // Apply migrations sequentially
    if (current < 1) {
        if (!migrateToV1()) {
            DICE_LOG_ERROR("Migration: V1 migration failed");
            return false;
        }
        current = 1;
    }

    if (current < 2) {
        if (!migrateV1toV2()) {
            DICE_LOG_ERROR("Migration: V1→V2 migration failed");
            return false;
        }
        current = 2;
    }

    if (current < 3) {
        if (!migrateV2toV3()) {
            DICE_LOG_ERROR("Migration: V2→V3 migration failed");
            return false;
        }
        current = 3;
    }

    if (current < 4) {
        if (!migrateV3toV4()) {
            DICE_LOG_ERROR("Migration: V3→V4 migration failed");
            return false;
        }
        current = 4;
    }

    setVersion(current);
    DICE_LOG_INFO("Migration: complete — schema is now V{}", current);
    return true;
}

int Migration::getCurrentVersion() {
    try {
        auto* storage = db_.getStorage();
        if (!storage) return 0;

        auto records = storage->get_all<MigrationRecord>();
        if (records.empty()) {
            return 0;
        }
        // Return the highest version recorded
        int maxVersion = 0;
        for (const auto& r : records) {
            if (r.version > maxVersion) maxVersion = r.version;
        }
        return maxVersion;
    } catch (const std::exception& e) {
        DICE_LOG_WARN("Migration: failed to query version (table may not exist yet): {}", e.what());
        return 0;
    }
}

bool Migration::needsLegacyImport(const std::string& legacyPath) {
    if (!fs::exists(legacyPath)) {
        DICE_LOG_DEBUG("Migration: legacy path '{}' does not exist", legacyPath);
        return false;
    }

    // Check for known legacy files
    static const std::vector<std::string> kLegacyFiles = {
        "reply.json",
        "config.json",
    };

    // Check for legacy directories
    static const std::vector<std::string> kLegacyDirs = {
        "decks",
        "groups",
    };

    bool hasLegacyFiles = false;

    for (const auto& file : kLegacyFiles) {
        auto fullPath = fs::path(legacyPath) / file;
        if (fs::exists(fullPath)) {
            DICE_LOG_INFO("Migration: legacy file detected: {}", file);
            hasLegacyFiles = true;

            // Check if already imported
            auto* storage = db_.getStorage();
            if (storage) {
                try {
                    auto imported = storage->get_all<LegacyImportLogRow>(
                        orm::where(orm::c(&LegacyImportLogRow::filePath) == fullPath.string())
                    );
                    if (!imported.empty()) {
                        DICE_LOG_INFO("Migration: '{}' already imported at {}, skipping",
                            file, imported.front().importedAt);
                        continue;
                    }
                } catch (const std::exception& e) {
                    DICE_LOG_DEBUG("Migration: legacy_import_log query failed (table may not exist): {}", e.what());
                }
            }
            return true;
        }
    }

    for (const auto& dir : kLegacyDirs) {
        auto fullPath = fs::path(legacyPath) / dir;
        if (fs::exists(fullPath) && fs::is_directory(fullPath)) {
            DICE_LOG_INFO("Migration: legacy directory detected: {}", dir);

            // Check if already imported
            auto* storage = db_.getStorage();
            if (storage) {
                try {
                    auto imported = storage->get_all<LegacyImportLogRow>(
                        orm::where(orm::c(&LegacyImportLogRow::filePath) == fullPath.string())
                    );
                    if (!imported.empty()) {
                        DICE_LOG_INFO("Migration: '{}' already imported at {}, skipping",
                            dir, imported.front().importedAt);
                        continue;
                    }
                } catch (const std::exception& e) {
                    DICE_LOG_DEBUG("Migration: legacy_import_log query failed: {}", e.what());
                }
            }
            return true;
        }
    }

    return false;
}

// ─── Private: Migration Steps ────────────────────────────────

bool Migration::migrateToV1() {
    DICE_LOG_INFO("Migration: applying V1 baseline schema");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("Migration: database not open");
            return false;
        }

        // sync_schema creates tables that are registered in the storage
        storage->sync_schema();

        DICE_LOG_INFO("Migration: V1 baseline applied");

        // Seed with initial version record
        MigrationRecord seed;
        seed.version = 1;
        seed.appliedAt = utils::nowIso8601();
        storage->insert(seed);

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Migration: V1 failed: {}", e.what());
        return false;
    }
}

bool Migration::migrateV1toV2() {
    DICE_LOG_INFO("Migration: applying V1→V2 upgrade (full application schema)");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("Migration: database not open");
            return false;
        }

        // sync_schema will create any tables that were added to the Storage type
        // since V1 (reply_rules, dice_config, decks, adapters, sessions,
        // banlist, legacy_import_log)
        storage->sync_schema();

        DICE_LOG_INFO("Migration: V1→V2 upgrade complete — all tables created");

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Migration: V1→V2 failed: {}", e.what());
        return false;
    }
}

bool Migration::migrateV2toV3() {
    DICE_LOG_INFO("Migration: applying V2→V3 upgrade (causal rule engine tables)");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("Migration: database not open");
            return false;
        }

        // sync_schema will create causal_rules and rule_counters tables
        // (they were added to the Storage type definition in database.h).
        // For existing databases, sync_schema also adds missing columns.
        storage->sync_schema();

        DICE_LOG_INFO("Migration: V2→V3 upgrade complete — causal_rules and rule_counters created");

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Migration: V2→V3 failed: {}", e.what());
        return false;
    }
}

bool Migration::migrateV3toV4() {
    DICE_LOG_INFO("Migration: applying V3→V4 upgrade (persona switching system tables)");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("Migration: database not open");
            return false;
        }

        // sync_schema will create persona_templates and persona_entries tables
        // (they were added to the Storage type definition in database.h).
        storage->sync_schema();

        DICE_LOG_INFO("Migration: V3→V4 upgrade complete — persona_templates and persona_entries created");

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Migration: V3→V4 failed: {}", e.what());
        return false;
    }
}

void Migration::setVersion(int version) {
    try {
        auto* storage = db_.getStorage();
        if (!storage) return;

        // Remove old version records and insert the current one
        storage->remove_all<MigrationRecord>();

        MigrationRecord record;
        record.version = version;
        record.appliedAt = utils::nowIso8601();
        storage->insert(record);
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("Migration: failed to set version: {}", e.what());
    }
}

}  // namespace dice
