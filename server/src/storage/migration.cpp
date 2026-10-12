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
    DICE_LOG_DEBUG("检查数据库结构版本，目标版本 V{}", kTargetVersion);

    int current = getCurrentVersion();

    if (current >= kTargetVersion) {
        DICE_LOG_DEBUG("数据库结构已是最新版本 V{}，无需迁移", current);
        return true;
    }

    DICE_LOG_INFO("开始升级数据库结构：V{} → V{}", current, kTargetVersion);

    // Apply migrations sequentially
    if (current < 1) {
        if (!migrateToV1()) {
            DICE_LOG_ERROR("初始化数据库 V1 结构失败");
            return false;
        }
        current = 1;
    }

    if (current < 2) {
        if (!migrateV1toV2()) {
            DICE_LOG_ERROR("数据库结构 V1 → V2 升级失败");
            return false;
        }
        current = 2;
    }

    if (current < 3) {
        if (!migrateV2toV3()) {
            DICE_LOG_ERROR("数据库结构 V2 → V3 升级失败");
            return false;
        }
        current = 3;
    }

    if (current < 4) {
        if (!migrateV3toV4()) {
            DICE_LOG_ERROR("数据库结构 V3 → V4 升级失败");
            return false;
        }
        current = 4;
    }

    setVersion(current);
    DICE_LOG_SUCCESS("数据库结构升级完成，当前版本 V{}", current);
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
        DICE_LOG_DEBUG("未读取到数据库结构版本，新数据库可能尚未创建版本表：{}", e.what());
        return 0;
    }
}

bool Migration::needsLegacyImport(const std::string& legacyPath) {
    if (!fs::exists(legacyPath)) {
        DICE_LOG_DEBUG("旧版数据目录不存在：{}", legacyPath);
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
            DICE_LOG_INFO("发现旧版数据文件：{}", file);
            hasLegacyFiles = true;

            // Check if already imported
            auto* storage = db_.getStorage();
            if (storage) {
                try {
                    auto imported = storage->get_all<LegacyImportLogRow>(
                        orm::where(orm::c(&LegacyImportLogRow::filePath) == fullPath.string())
                    );
                    if (!imported.empty()) {
                        DICE_LOG_INFO("旧版数据 {} 已于 {} 导入，跳过重复导入",
                            file, imported.front().importedAt);
                        continue;
                    }
                } catch (const std::exception& e) {
                    DICE_LOG_DEBUG("未读取到旧版导入记录，新数据库可能尚未创建记录表：{}", e.what());
                }
            }
            return true;
        }
    }

    for (const auto& dir : kLegacyDirs) {
        auto fullPath = fs::path(legacyPath) / dir;
        if (fs::exists(fullPath) && fs::is_directory(fullPath)) {
            DICE_LOG_INFO("发现旧版数据目录：{}", dir);

            // Check if already imported
            auto* storage = db_.getStorage();
            if (storage) {
                try {
                    auto imported = storage->get_all<LegacyImportLogRow>(
                        orm::where(orm::c(&LegacyImportLogRow::filePath) == fullPath.string())
                    );
                    if (!imported.empty()) {
                        DICE_LOG_INFO("旧版数据 {} 已于 {} 导入，跳过重复导入",
                            dir, imported.front().importedAt);
                        continue;
                    }
                } catch (const std::exception& e) {
                    DICE_LOG_DEBUG("查询旧版导入记录失败：{}", e.what());
                }
            }
            return true;
        }
    }

    return false;
}

// ─── Private: Migration Steps ────────────────────────────────

bool Migration::migrateToV1() {
    DICE_LOG_DEBUG("初始化数据库 V1 基础结构");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("无法升级数据库结构：数据库尚未打开");
            return false;
        }

        // sync_schema creates tables that are registered in the storage
        storage->sync_schema();

        DICE_LOG_DEBUG("数据库 V1 基础结构已创建");

        // Seed with initial version record
        MigrationRecord seed;
        seed.version = 1;
        seed.appliedAt = utils::nowIso8601();
        storage->insert(seed);

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("数据库 V1 初始化失败：{}", e.what());
        return false;
    }
}

bool Migration::migrateV1toV2() {
    DICE_LOG_DEBUG("升级数据库结构 V1 → V2：创建应用数据表");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("无法升级数据库结构：数据库尚未打开");
            return false;
        }

        // sync_schema will create any tables that were added to the Storage type
        // since V1 (reply_rules, dice_config, decks, adapters, sessions,
        // banlist, legacy_import_log)
        storage->sync_schema();

        DICE_LOG_DEBUG("数据库结构 V1 → V2 升级完成，应用数据表已创建");

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("数据库结构 V1 → V2 升级失败：{}", e.what());
        return false;
    }
}

bool Migration::migrateV2toV3() {
    DICE_LOG_DEBUG("升级数据库结构 V2 → V3：创建因果规则及计数表");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("无法升级数据库结构：数据库尚未打开");
            return false;
        }

        // sync_schema will create causal_rules and rule_counters tables
        // (they were added to the Storage type definition in database.h).
        // For existing databases, sync_schema also adds missing columns.
        storage->sync_schema();

        DICE_LOG_DEBUG("数据库结构 V2 → V3 升级完成，因果规则及计数表已创建");

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("数据库结构 V2 → V3 升级失败：{}", e.what());
        return false;
    }
}

bool Migration::migrateV3toV4() {
    DICE_LOG_DEBUG("升级数据库结构 V3 → V4：创建人格模板及条目表");

    try {
        auto* storage = db_.getStorage();
        if (!storage) {
            DICE_LOG_ERROR("无法升级数据库结构：数据库尚未打开");
            return false;
        }

        // sync_schema will create persona_templates and persona_entries tables
        // (they were added to the Storage type definition in database.h).
        storage->sync_schema();

        DICE_LOG_DEBUG("数据库结构 V3 → V4 升级完成，人格模板及条目表已创建");

        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("数据库结构 V3 → V4 升级失败：{}", e.what());
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
        DICE_LOG_ERROR("保存数据库结构版本失败：{}", e.what());
    }
}

}  // namespace dice
