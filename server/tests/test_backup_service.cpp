// ─── Issue #10: Backup sharing-violation regression tests ────────────
// "automatic backup failed: The process cannot access the file because it is
// being used by another process" came from copying live files while another
// process (or a concurrent backup) held them.  The backup staging now skips
// the data/backups store itself, retries transient Windows locks, and
// serializes manual/automatic backup runs.

#include "test_framework.h"
#include "../src/service/backup_service.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

using namespace dice;
namespace fs = std::filesystem;

namespace {

fs::path tempRoot(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (std::string("dice_next_") + name + "_" + std::to_string(nonce));
}

void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

}  // namespace

TEST(BackupSelection, DefaultsKeepImportantDataButSkipBulkyMedia) {
    const backup::Selection selection;
    ASSERT_TRUE(selection.config);
    ASSERT_TRUE(selection.coreDatabase);
    ASSERT_TRUE(selection.characterCards);
    ASSERT_TRUE(selection.chatHistory);
    ASSERT_TRUE(selection.gameLogs);
    ASSERT_FALSE(selection.runtimeLogs);
    ASSERT_FALSE(selection.auditLogs);
    ASSERT_FALSE(selection.uploadedAssets);
    ASSERT_FALSE(selection.resourceImages);
    ASSERT_FALSE(selection.gameLogImages);
    ASSERT_FALSE(selection.chatMedia);
    ASSERT_FALSE(selection.complete());
    ASSERT_TRUE(backup::Selection::full().complete());
}

TEST(BackupSelection, ExpandsLegacyBroadCategories) {
    const auto selection = backup::Selection::fromJson({
        {"config", true}, {"databases", true}, {"logs", false},
        {"resources", false}, {"plugins", true}, {"media", false},
    });
    ASSERT_TRUE(selection.coreDatabase);
    ASSERT_TRUE(selection.characterCards);
    ASSERT_TRUE(selection.chatHistory);
    ASSERT_TRUE(selection.gameLogs);       // old databases copied every *.db, including logs.db
    ASSERT_FALSE(selection.runtimeLogs);
    ASSERT_FALSE(selection.auditLogs);
    ASSERT_FALSE(selection.decks);
    ASSERT_TRUE(selection.jsPlugins);
    ASSERT_TRUE(selection.luaMods);
    ASSERT_FALSE(selection.gameLogImages);
    ASSERT_FALSE(selection.chatMedia);
}

TEST(BackupSelection, FineGrainedKeysOverrideLegacyValues) {
    const auto selection = backup::Selection::fromJson({
        {"databases", true}, {"logs", true}, {"media", true},
        {"gameLogs", false}, {"runtimeLogs", false}, {"auditLogs", false}, {"gameLogImages", false},
    });
    ASSERT_FALSE(selection.gameLogs);
    ASSERT_FALSE(selection.runtimeLogs);
    ASSERT_FALSE(selection.auditLogs);
    ASSERT_FALSE(selection.gameLogImages);
    ASSERT_TRUE(selection.chatMedia);
}

TEST(BackupCopy, FullTreeSkipsBackupsStore) {
    const fs::path root = tempRoot("backup_skip");
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path data = root / "data", stage = root / "stage";
    writeFile(data / "keep.txt", "keep");
    writeFile(data / "backups" / "old.zip", "zip");
    writeFile(data / "backups" / "nested" / "x", "x");
    writeFile(data / "logs" / "app.log", "log");

    std::string error;
    ASSERT_TRUE(backup::copyTree(data, stage, error, true));
    ASSERT_TRUE(fs::exists(stage / "keep.txt"));
    ASSERT_TRUE(fs::exists(stage / "logs" / "app.log"));
    ASSERT_FALSE(fs::exists(stage / "backups"));
    fs::remove_all(root, ec);
}

TEST(BackupCopy, NonSkippingTreeKeepsBackups) {
    const fs::path root = tempRoot("backup_keep");
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path data = root / "data", stage = root / "stage";
    writeFile(data / "keep.txt", "keep");
    writeFile(data / "backups" / "old.zip", "zip");

    std::string error;
    ASSERT_TRUE(backup::copyTree(data, stage, error, false));
    ASSERT_TRUE(fs::exists(stage / "keep.txt"));
    ASSERT_TRUE(fs::exists(stage / "backups" / "old.zip"));
    fs::remove_all(root, ec);
}

TEST(BackupCopy, SingleFileCopy) {
    const fs::path root = tempRoot("backup_file");
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path src = root / "src" / "a.bin", dst = root / "dst" / "a.bin";
    writeFile(src, "hello");
    std::string error;
    ASSERT_TRUE(backup::copyItem(src, dst, error));
    ASSERT_TRUE(fs::exists(dst));
    fs::remove_all(root, ec);
}

TEST(BackupCopy, FlatFileFilterSeparatesTranscriptAndCrashLogs) {
    const fs::path root = tempRoot("backup_log_filter");
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path logs = root / "logs";
    writeFile(logs / "q_123_campaign.txt", "transcript");
    writeFile(logs / "crash_20260101.txt", "crash");
    writeFile(logs / "app" / "dice.log", "runtime");

    std::string error;
    ASSERT_TRUE(backup::copyFlatFiles(logs, root / "transcripts", error,
        [](const fs::path& path) { return path.filename().string().rfind("crash_", 0) != 0; }));
    ASSERT_TRUE(backup::copyFlatFiles(logs, root / "runtime", error,
        [](const fs::path& path) { return path.filename().string().rfind("crash_", 0) == 0; }));
    ASSERT_TRUE(fs::exists(root / "transcripts" / "q_123_campaign.txt"));
    ASSERT_FALSE(fs::exists(root / "transcripts" / "crash_20260101.txt"));
    ASSERT_TRUE(fs::exists(root / "runtime" / "crash_20260101.txt"));
    ASSERT_FALSE(fs::exists(root / "runtime" / "q_123_campaign.txt"));
    ASSERT_FALSE(fs::exists(root / "transcripts" / "app"));
    fs::remove_all(root, ec);
}

TEST(BackupCopy, SkipsInstanceLockFile) {
    const fs::path root = tempRoot("backup_lockfile");
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path data = root / "data", stage = root / "stage";
    writeFile(data / ".instance.lock", "pid");
    writeFile(data / "keep.txt", "keep");

    std::string error;
    ASSERT_TRUE(backup::copyTree(data, stage, error, true));
    ASSERT_TRUE(fs::exists(stage / "keep.txt"));
    ASSERT_FALSE(fs::exists(stage / ".instance.lock"));
    fs::remove_all(root, ec);
}

TEST(BackupLock, SerializesBackupRuns) {
    ASSERT_TRUE(backup::beginBackup());
    ASSERT_FALSE(backup::beginBackup());   // a second concurrent run is rejected
    backup::endBackup();
    ASSERT_TRUE(backup::beginBackup());
    backup::endBackup();
}

TEST(BackupLock, TransientLockCodesAreDetected) {
#ifdef _WIN32
    std::error_code ec(32, std::generic_category());
    ASSERT_TRUE(backup::transientLock(ec));
    std::error_code ec2(33, std::generic_category());
    ASSERT_TRUE(backup::transientLock(ec2));
    std::error_code ec3(2, std::generic_category());
    ASSERT_FALSE(backup::transientLock(ec3));
#else
    std::error_code ec(32, std::generic_category());
    ASSERT_FALSE(backup::transientLock(ec));
#endif
}
