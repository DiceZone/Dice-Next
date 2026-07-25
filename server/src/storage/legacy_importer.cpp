#include "legacy_importer.h"
#include "database.h"
#include "../common/logger.h"
#include "../common/utils.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sqlite_orm/sqlite_orm.h>

namespace dice {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─── Constructor ─────────────────────────────────────────────

LegacyImporter::LegacyImporter(Database& db, const std::string& legacyDataPath)
    : db_(db)
    , legacyDataPath_(legacyDataPath) {
}

// ─── Backup ──────────────────────────────────────────────────

void LegacyImporter::backupDatabase() {
    if (!db_.isOpen()) {
        DICE_LOG_WARN("LegacyImporter: database not open, skipping backup");
        return;
    }

    std::string backupPath = db_.path() + ".backup";
    try {
        fs::copy_file(db_.path(), backupPath,
            fs::copy_options::overwrite_existing);
        DICE_LOG_INFO("LegacyImporter: database backed up to '{}'", backupPath);
    } catch (const fs::filesystem_error& e) {
        DICE_LOG_ERROR("LegacyImporter: backup failed: {}", e.what());
        report_.warnings.push_back("Database backup failed: " + std::string(e.what()));
    }
}

// ─── Idempotency ─────────────────────────────────────────────

bool LegacyImporter::isAlreadyImported(const std::string& filePath) {
    auto* storage = db_.getStorage();
    if (!storage) return false;

    try {
        auto rows = storage->get_all<LegacyImportLogRow>(
            orm::where(orm::c(&LegacyImportLogRow::filePath) == filePath)
        );
        return !rows.empty();
    } catch (const std::exception& e) {
        DICE_LOG_DEBUG("LegacyImporter: import log check failed: {}", e.what());
        return false;
    }
}

void LegacyImporter::recordImport(const std::string& filePath, int recordCount) {
    auto* storage = db_.getStorage();
    if (!storage) return;

    try {
        LegacyImportLogRow log;
        log.filePath   = filePath;
        log.importedAt = utils::nowIso8601();
        log.recordCount = recordCount;
        storage->insert(log);
    } catch (const std::exception& e) {
        DICE_LOG_WARN("LegacyImporter: failed to record import log: {}", e.what());
        report_.warnings.push_back("Failed to record import log for: " + filePath);
    }
}

// ─── Detect Format ───────────────────────────────────────────

std::string LegacyImporter::detectOldDataFormat(const std::string& path) {
    if (!fs::exists(path)) {
        return "";
    }

    // Check for known marker files
    if (fs::exists(fs::path(path) / "reply.json")) {
        return "dice_legacy_v1";
    }
    if (fs::exists(fs::path(path) / "config.json")) {
        return "dice_legacy_v1";
    }
    if (fs::exists(fs::path(path) / "decks")) {
        return "dice_legacy_v1";
    }
    if (fs::exists(fs::path(path) / "groups")) {
        return "dice_legacy_v1";
    }

    return "";
}

// ─── Full Import Pipeline ────────────────────────────────────

MigrationReport LegacyImporter::import(const std::string& legacyPath) {
    DICE_LOG_INFO("LegacyImporter: starting import from '{}'", legacyPath);

    report_ = MigrationReport{};

    auto format = detectOldDataFormat(legacyPath);
    if (format.empty()) {
        report_.errors++;
        report_.warnings.push_back("No recognizable legacy data found at: " + legacyPath);
        DICE_LOG_WARN("LegacyImporter: no legacy data detected at '{}'", legacyPath);
        return report_;
    }

    DICE_LOG_INFO("LegacyImporter: detected format '{}'", format);

    // Step 0: Backup database before any changes
    backupDatabase();

    // Step 1: Import replies
    std::string replyFile = (fs::path(legacyPath) / "reply.json").string();
    if (fs::exists(replyFile)) {
        if (!isAlreadyImported(replyFile)) {
            int count = importReplies(replyFile);
            report_.totalImported += count;
            if (count > 0) {
                recordImport(replyFile, count);
            }
        } else {
            DICE_LOG_INFO("LegacyImporter: '{}' already imported, skipping", replyFile);
            report_.skippedItems.push_back("reply.json (already imported)");
        }
    } else {
        report_.skippedItems.push_back("reply.json (file not found)");
    }

    // Step 2: Import decks
    std::string decksDir = (fs::path(legacyPath) / "decks").string();
    if (fs::exists(decksDir)) {
        if (!isAlreadyImported(decksDir)) {
            int count = importDecks(decksDir);
            report_.totalImported += count;
            if (count > 0) {
                recordImport(decksDir, count);
            }
        } else {
            DICE_LOG_INFO("LegacyImporter: '{}' already imported, skipping", decksDir);
            report_.skippedItems.push_back("decks/ (already imported)");
        }
    } else {
        report_.skippedItems.push_back("decks/ (directory not found)");
    }

    // Step 3: Import dice rules
    std::string configFile = (fs::path(legacyPath) / "config.json").string();
    if (fs::exists(configFile)) {
        if (!isAlreadyImported(configFile)) {
            int count = importDiceRules(configFile);
            report_.totalImported += count;
            if (count > 0) {
                recordImport(configFile, count);
            }
        } else {
            DICE_LOG_INFO("LegacyImporter: '{}' already imported, skipping", configFile);
            report_.skippedItems.push_back("config.json (already imported)");
        }
    } else {
        report_.skippedItems.push_back("config.json (file not found)");
    }

    // Step 4: Import group configs
    std::string groupsDir = (fs::path(legacyPath) / "groups").string();
    if (fs::exists(groupsDir)) {
        if (!isAlreadyImported(groupsDir)) {
            int count = importGroupConfigs(groupsDir);
            report_.totalImported += count;
            if (count > 0) {
                recordImport(groupsDir, count);
            }
        } else {
            DICE_LOG_INFO("LegacyImporter: '{}' already imported, skipping", groupsDir);
            report_.skippedItems.push_back("groups/ (already imported)");
        }
    } else {
        report_.skippedItems.push_back("groups/ (directory not found)");
    }

    DICE_LOG_INFO("LegacyImporter: import complete — {} imported, {} errors, {} warnings, {} skipped",
        report_.totalImported, report_.errors,
        report_.warnings.size(), report_.skippedItems.size());

    return report_;
}

// ─── Import: Replies ─────────────────────────────────────────

int LegacyImporter::importReplies(const std::string& file) {
    DICE_LOG_INFO("LegacyImporter: importing replies from '{}'", file);

    auto* storage = db_.getStorage();
    if (!storage) {
        report_.errors++;
        report_.warnings.push_back("Database not open during reply import");
        return 0;
    }

    try {
        std::ifstream ifs(file);
        if (!ifs.is_open()) {
            report_.warnings.push_back("Cannot open reply file: " + file);
            return 0;
        }

        json legacyData = json::parse(ifs);

        int count = 0;
        auto importEntry = [&](const json& entry, int priority) -> bool {
            try {
                ReplyRuleRow row;
                row.priority = priority;

                // Map legacy fields
                if (entry.contains("match_type")) {
                    std::string mt = entry["match_type"].get<std::string>();
                    if (mt == "keyword")      row.matchType = 0;
                    else if (mt == "prefix")  row.matchType = 1;
                    else if (mt == "regex")   row.matchType = 2;
                    else if (mt == "search")  row.matchType = 3;
                    else                      row.matchType = 0;
                }

                if (entry.contains("keyword") || entry.contains("keyword")) {
                    row.matchContent = entry.value("keyword",
                        entry.value("match", entry.value("pattern", "")));
                } else if (entry.contains("match")) {
                    row.matchContent = entry["match"].get<std::string>();
                } else if (entry.contains("pattern")) {
                    row.matchContent = entry["pattern"].get<std::string>();
                }

                if (entry.contains("reply")) {
                    row.replyContent = entry["reply"].get<std::string>();
                } else if (entry.contains("answer")) {
                    row.replyContent = entry["answer"].get<std::string>();
                } else if (entry.contains("response")) {
                    row.replyContent = entry["response"].get<std::string>();
                } else {
                    // No reply content — skip this entry
                    return false;
                }

                row.enabled = entry.value("enabled", true);
                row.createdAt = utils::nowIso8601();
                row.updatedAt = row.createdAt;

                storage->insert(row);
                return true;
            } catch (const std::exception& e) {
                report_.warnings.push_back("Failed to import reply entry: " + std::string(e.what()));
                return false;
            }
        };

        if (legacyData.is_array()) {
            int priority = static_cast<int>(legacyData.size());
            for (const auto& entry : legacyData) {
                if (importEntry(entry, priority)) count++;
                priority--;
            }
        } else if (legacyData.is_object()) {
            // Legacy format where keys are match strings, values are replies
            for (auto& [key, val] : legacyData.items()) {
                ReplyRuleRow row;
                row.matchType = 0;  // keyword by default
                row.matchContent = key;
                row.replyContent = val.is_string() ? val.get<std::string>() : val.dump();
                row.enabled = true;
                row.priority = 10;  // default priority for legacy key-value format
                row.createdAt = utils::nowIso8601();
                row.updatedAt = row.createdAt;

                try {
                    storage->insert(row);
                    count++;
                } catch (const std::exception& e) {
                    report_.warnings.push_back("Failed to import legacy reply '" + key + "': " + e.what());
                }
            }
        }

        DICE_LOG_INFO("LegacyImporter: imported {} reply entries", count);
        return count;
    } catch (const json::parse_error& e) {
        report_.errors++;
        report_.warnings.push_back("Failed to parse reply file: " + std::string(e.what()));
        DICE_LOG_ERROR("LegacyImporter: parse error in '{}': {}", file, e.what());
        return 0;
    }
}

// ─── Import: Decks ───────────────────────────────────────────

int LegacyImporter::importDecks(const std::string& dir) {
    DICE_LOG_INFO("LegacyImporter: importing decks from '{}'", dir);

    auto* storage = db_.getStorage();
    if (!storage) {
        report_.errors++;
        report_.warnings.push_back("Database not open during deck import");
        return 0;
    }

    int count = 0;
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            try {
                std::ifstream ifs(entry.path());
                if (!ifs.is_open()) {
                    report_.warnings.push_back("Cannot open deck file: " + entry.path().string());
                    continue;
                }

                json deckData = json::parse(ifs);

                DeckRow row;
                row.name = entry.path().stem().string();
                row.cards = deckData.dump();
                row.createdAt = utils::nowIso8601();

                storage->insert(row);
                count++;

                DICE_LOG_DEBUG("LegacyImporter: imported deck '{}'", row.name);
            } catch (const json::parse_error& e) {
                report_.warnings.push_back("Failed to parse deck file " + entry.path().string() + ": " + e.what());
            } catch (const std::exception& e) {
                report_.warnings.push_back("Failed to import deck " + entry.path().string() + ": " + e.what());
            }
        }
    } catch (const fs::filesystem_error& e) {
        report_.errors++;
        report_.warnings.push_back("Failed to iterate decks directory: " + std::string(e.what()));
        DICE_LOG_ERROR("LegacyImporter: filesystem error in '{}': {}", dir, e.what());
        return 0;
    }

    DICE_LOG_INFO("LegacyImporter: imported {} decks", count);
    return count;
}

// ─── Import: Dice Rules ──────────────────────────────────────

int LegacyImporter::importDiceRules(const std::string& file) {
    DICE_LOG_INFO("LegacyImporter: importing dice rules from '{}'", file);

    auto* storage = db_.getStorage();
    if (!storage) {
        report_.errors++;
        report_.warnings.push_back("Database not open during dice rules import");
        return 0;
    }

    try {
        std::ifstream ifs(file);
        if (!ifs.is_open()) {
            report_.warnings.push_back("Cannot open dice rules file: " + file);
            return 0;
        }

        json legacyData = json::parse(ifs);

        int count = 0;
        auto importKey = [&](const std::string& key, const std::string& value) {
            try {
                // Check if key already exists (idempotent)
                auto existing = storage->get_all<DiceConfigRow>(
                    orm::where(orm::c(&DiceConfigRow::key) == key)
                );
                if (!existing.empty()) {
                    DICE_LOG_DEBUG("LegacyImporter: dice config '{}' already exists, skipping", key);
                    return;
                }

                DiceConfigRow row;
                row.key = key;
                row.value = value;
                storage->insert(row);
                count++;
            } catch (const std::exception& e) {
                report_.warnings.push_back("Failed to import dice config '" + key + "': " + e.what());
            }
        };

        // Look for dice rules in various legacy locations
        json* rulesSection = nullptr;

        if (legacyData.contains("dice") && legacyData["dice"].is_object()) {
            rulesSection = &legacyData["dice"];
        } else if (legacyData.contains("rules") && legacyData["rules"].is_object()) {
            rulesSection = &legacyData["rules"];
        }

        if (rulesSection) {
            for (auto& [key, val] : rulesSection->items()) {
                std::string valueStr = val.is_string() ? val.get<std::string>() : val.dump();
                importKey("legacy_dice_" + key, valueStr);
            }
        }

        // Also import the entire config as a single key for reference
        importKey("legacy_full_config", legacyData.dump());

        DICE_LOG_INFO("LegacyImporter: imported {} dice config entries", count);
        return count;
    } catch (const json::parse_error& e) {
        report_.errors++;
        report_.warnings.push_back("Failed to parse dice rules file: " + std::string(e.what()));
        DICE_LOG_ERROR("LegacyImporter: parse error in '{}': {}", file, e.what());
        return 0;
    }
}

// ─── Import: Group Configs ───────────────────────────────────

int LegacyImporter::importGroupConfigs(const std::string& dir) {
    DICE_LOG_INFO("LegacyImporter: importing group configs from '{}'", dir);

    auto* storage = db_.getStorage();
    if (!storage) {
        report_.errors++;
        report_.warnings.push_back("Database not open during group config import");
        return 0;
    }

    int count = 0;
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            try {
                std::ifstream ifs(entry.path());
                if (!ifs.is_open()) {
                    report_.warnings.push_back("Cannot open group config file: " + entry.path().string());
                    continue;
                }

                json groupData = json::parse(ifs);

                // Store group config as a dice_config entry with the group's identifier
                std::string groupId = entry.path().stem().string();

                // Check for duplicate (idempotent)
                auto existing = storage->get_all<DiceConfigRow>(
                    orm::where(orm::c(&DiceConfigRow::key) == "legacy_group_" + groupId)
                );
                if (!existing.empty()) {
                    DICE_LOG_DEBUG("LegacyImporter: group config '{}' already exists, skipping", groupId);
                    continue;
                }

                DiceConfigRow row;
                row.key = "legacy_group_" + groupId;
                row.value = groupData.dump();
                storage->insert(row);
                count++;

                DICE_LOG_DEBUG("LegacyImporter: imported group config '{}'", groupId);
            } catch (const json::parse_error& e) {
                report_.warnings.push_back("Failed to parse group config " + entry.path().string() + ": " + e.what());
            } catch (const std::exception& e) {
                report_.warnings.push_back("Failed to import group config " + entry.path().string() + ": " + e.what());
            }
        }
    } catch (const fs::filesystem_error& e) {
        report_.errors++;
        report_.warnings.push_back("Failed to iterate groups directory: " + std::string(e.what()));
        DICE_LOG_ERROR("LegacyImporter: filesystem error in '{}': {}", dir, e.what());
        return 0;
    }

    DICE_LOG_INFO("LegacyImporter: imported {} group configs", count);
    return count;
}

}  // namespace dice
