#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dice {

class Database;

/**
 * @brief Result structure returned after executing a legacy data import.
 *
 * Conforms to 8.5 cross-file conventions.
 */
struct MigrationReport {
    int totalImported = 0;
    int errors = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> skippedItems;

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["total_imported"] = totalImported;
        j["errors"] = errors;
        j["warnings"] = warnings;
        j["skipped_items"] = skippedItems;
        return j;
    }
};

/**
 * @brief Reads old Dice! (CoolQ DLL) JSON / custom-format data and
 *        writes it into the new SQLite schema.
 *
 * Features:
 *   - Idempotent: checks `legacy_import_log` table and skips
 *     files that have already been imported.
 *   - Safe: creates a `.db.backup` before any destructive writes.
 *   - Complete: imports replies, decks, dice rules, and group
 *     configs in a single pass.
 */
class LegacyImporter {
public:
    LegacyImporter(Database& db, const std::string& legacyDataPath);
    ~LegacyImporter() = default;

    // ─── Public API ──────────────────────────────────────────

    /// Detect the format of old data at the given path.
    /// Returns a format identifier string, or empty if unknown.
    std::string detectOldDataFormat(const std::string& path);

    /// Run the full import pipeline for all four data types.
    /// Returns a consolidated MigrationReport.
    MigrationReport import(const std::string& legacyPath);

    /// Import replies from legacy reply.json file.
    /// Returns the number of records imported.
    int importReplies(const std::string& file);

    /// Import decks from a legacy decks/ directory.
    /// Returns the number of decks imported.
    int importDecks(const std::string& dir);

    /// Import dice rules from a legacy config file.
    /// Returns the number of rule groups imported.
    int importDiceRules(const std::string& file);

    /// Import group configurations from legacy groups/ directory.
    /// Returns the number of group configs imported.
    int importGroupConfigs(const std::string& file);

    /// Get the latest import report (cumulative).
    const MigrationReport& getReport() const noexcept { return report_; }

private:
    Database& db_;
    std::string legacyDataPath_;
    MigrationReport report_;

    // ─── Safety & Idempotency ────────────────────────────────

    /// Create a backup of the current database before importing.
    void backupDatabase();

    /// Check the legacy_import_log to see if a file was already imported.
    bool isAlreadyImported(const std::string& filePath);

    /// Record a successful import in the legacy_import_log table.
    void recordImport(const std::string& filePath, int recordCount);
};

}  // namespace dice
