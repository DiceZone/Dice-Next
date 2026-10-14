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
#ifdef _WIN32
#include <winioctl.h>
#include <cstddef>
#include <cstring>
#endif

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

struct RestoreWorkspace {
    fs::path previous = fs::current_path();
    fs::path root = tempRoot("restore_path_guard");
    fs::path workspace = root / "workspace";
    RestoreWorkspace() {
        if (!fs::create_directory(root)) throw std::runtime_error("cannot create restore test root");
        fs::create_directory(workspace);
        fs::current_path(workspace);
    }
    ~RestoreWorkspace() {
        std::error_code ec;
        fs::current_path(previous, ec);
        if (migration_resources::safePath(root.parent_path(), root)) fs::remove_all(root, ec);
    }
};

bool createTestDirectoryLink(const fs::path& target, const fs::path& link, std::error_code& ec) {
#ifdef _WIN32
    // Directory junctions exercise reparse-point rejection without requiring
    // the symlink privilege or changing the machine's developer-mode setting.
    const std::wstring print = fs::absolute(target).native();
    const std::wstring substitute = L"\\??\\" + print;
    struct JunctionBuffer {
        DWORD tag;
        WORD dataLength, reserved;
        WORD substituteOffset, substituteLength, printOffset, printLength;
        wchar_t paths[1];
    };
    const size_t headerSize = offsetof(JunctionBuffer, paths);
    std::vector<unsigned char> bytes(headerSize + (substitute.size() + print.size() + 2) * sizeof(wchar_t), 0);
    auto* buffer = reinterpret_cast<JunctionBuffer*>(bytes.data());
    buffer->tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->dataLength = static_cast<WORD>(bytes.size() - 8);
    buffer->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    buffer->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    buffer->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(bytes.data() + headerSize, substitute.c_str(), buffer->substituteLength + sizeof(wchar_t));
    std::memcpy(bytes.data() + headerSize + buffer->printOffset, print.c_str(), buffer->printLength + sizeof(wchar_t));
    if (!fs::create_directory(link, ec)) return false;
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ec = std::error_code(GetLastError(), std::system_category());
    } else {
        DWORD returned = 0;
        const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(),
            static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr) != FALSE;
        if (!ok) ec = std::error_code(GetLastError(), std::system_category());
        CloseHandle(handle);
        if (ok) return true;
    }
    std::error_code ignored;
    fs::remove(link, ignored); // Only the newly-created, empty test directory.
    return false;
#else
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

}  // namespace

TEST(BackupRestorePaths, OnlyWhitelistedChildrenAreClearedAndActiveLogsSurvive) {
    RestoreWorkspace f;
    writeFile("data/state.txt", "replace");
    writeFile("data/logs/campaign.txt", "replace");
    writeFile("data/logs/app/current.log", "active log");
    writeFile("config/settings.json", "replace");
    writeFile("image/picture.png", "replace");
    std::string error;
    ASSERT_TRUE(backup::clearForFullRestore("data", true, error));
    ASSERT_FALSE(fs::exists("data/state.txt"));
    ASSERT_FALSE(fs::exists("data/logs/campaign.txt"));
    ASSERT_TRUE(fs::is_regular_file("data/logs/app/current.log"));
    ASSERT_TRUE(backup::clearForFullRestore("config", false, error));
    ASSERT_TRUE(backup::clearForFullRestore("image", false, error));
    ASSERT_TRUE(fs::is_empty("config"));
    ASSERT_TRUE(fs::is_empty("image"));
    ASSERT_TRUE(backup::clearForFullRestore("data", true, error));
}

TEST(BackupRestorePaths, RejectsWorkspaceParentNestedAndOutsideTargetsWithoutRemovingFiles) {
    RestoreWorkspace f;
    writeFile("data/state.txt", "keep");
    writeFile("nested/data/state.txt", "keep");
    writeFile("other/state.txt", "keep");
    writeFile(f.root / "outside/data/state.txt", "keep");
    std::string error;
    for (const auto& target : {fs::path("."), fs::path(".."), f.workspace.root_path(),
            fs::path("other"), fs::path("nested/data"), f.root / "outside/data"}) {
        ASSERT_FALSE(backup::clearForFullRestore(target, false, error));
        ASSERT_FALSE(error.empty());
    }
    ASSERT_TRUE(fs::is_regular_file("data/state.txt"));
    ASSERT_TRUE(fs::is_regular_file("nested/data/state.txt"));
    ASSERT_TRUE(fs::is_regular_file("other/state.txt"));
    ASSERT_TRUE(fs::is_regular_file(f.root / "outside/data/state.txt"));
}

TEST(BackupRestorePaths, RejectsLinksEvenWhenTraversalNormalizesBackIntoWorkspace) {
    RestoreWorkspace f;
    writeFile(f.root / "outside/state.txt", "keep");
    fs::create_directory("image");
    std::error_code ec;
    ASSERT_TRUE(createTestDirectoryLink(f.root / "outside", "data", ec));
    std::string error;
    ASSERT_FALSE(backup::clearForFullRestore("data", false, error));
    ASSERT_TRUE(fs::is_regular_file(f.root / "outside/state.txt"));
    ASSERT_TRUE(fs::remove("data")); // Remove only the test link, never its target.
    writeFile("data/state.txt", "keep");
    ASSERT_TRUE(createTestDirectoryLink(f.workspace / "image", "alias", ec));
    ASSERT_FALSE(static_cast<bool>(ec));
    ASSERT_FALSE(backup::clearForFullRestore("alias/../data", false, error));
    ASSERT_TRUE(fs::is_regular_file("data/state.txt"));
}

#ifdef _WIN32
TEST(BackupRestorePaths, WindowsShortAndLongNamesReferToTheSameWorkspace) {
    RestoreWorkspace f;
    const DWORD required = GetShortPathNameW(f.workspace.c_str(), nullptr, 0);
    ASSERT_TRUE(required > 0);
    std::wstring name(required, L'\0');
    const DWORD copied = GetShortPathNameW(f.workspace.c_str(), name.data(), required);
    ASSERT_TRUE(copied > 0 && copied < required);
    name.resize(copied);
    const fs::path alias(name);
    const auto real = fs::weakly_canonical(f.workspace);
    ASSERT_TRUE(fs::equivalent(alias, real));
    if (alias == real) {
        std::cout << "8.3 aliases unavailable on this volume; short-path check not exercised\n";
        return;
    }
    fs::current_path(alias);
    writeFile("data/state.txt", "replace");
    writeFile("data/logs/app/current.log", "keep");
    writeFile("config/settings.json", "replace");
    writeFile("image/picture.png", "replace");
    std::string error;
    ASSERT_TRUE(backup::clearForFullRestore("data", true, error));
    ASSERT_TRUE(backup::clearForFullRestore("config", false, error));
    ASSERT_TRUE(backup::clearForFullRestore("image", false, error));
    ASSERT_FALSE(fs::exists("data/state.txt"));
    ASSERT_TRUE(fs::is_regular_file("data/logs/app/current.log"));
    ASSERT_TRUE(fs::is_empty("config"));
    ASSERT_TRUE(fs::is_empty("image"));
    // Also cover an absolute short-path argument while the CWD uses long names.
    fs::current_path(real);
    writeFile("data/state.txt", "replace again");
    ASSERT_TRUE(backup::clearForFullRestore(alias / "data", true, error));
    ASSERT_FALSE(fs::exists("data/state.txt"));
    ASSERT_TRUE(fs::is_regular_file("data/logs/app/current.log"));
}
#endif

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
