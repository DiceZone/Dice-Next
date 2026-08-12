#pragma once

// Local full backup / restore service.  A restore is staged while the process
// is running and applied at the next startup, before any database is opened.
// This avoids overwriting live SQLite files or loaded plugins in place.

#include "../storage/database.h"
#include "../common/utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <algorithm>

namespace dice::backup {
namespace fs = std::filesystem;
using json = nlohmann::json;

// Each switch maps to one concrete kind of data.  In particular, transcript
// logs, process logs, transcript images, uploaded assets and chat-media caches
// must never be bundled under the same vague "logs/media" switch.
struct Selection {
    bool config = true;
    bool coreDatabase = true;
    bool characterCards = true;
    bool chatHistory = true;
    bool gameLogs = true;
    bool runtimeLogs = false;
    bool auditLogs = false;
    bool decks = true;
    bool rules = true;
    bool help = true;
    bool cardTemplates = true;
    bool jsPlugins = true;
    bool luaMods = true;
    bool uploadedAssets = false;
    bool resourceImages = false;
    bool gameLogImages = false;
    bool chatMedia = false;

    static Selection full() {
        Selection value;
        value.runtimeLogs = value.auditLogs = value.uploadedAssets = value.resourceImages =
            value.gameLogImages = value.chatMedia = true;
        return value;
    }

    bool complete() const {
        return config && coreDatabase && characterCards && chatHistory && gameLogs && runtimeLogs && auditLogs &&
            decks && rules && help && cardTemplates && jsPlugins && luaMods && uploadedAssets &&
            resourceImages && gameLogImages && chatMedia;
    }
    json toJson() const {
        return {{"config", config}, {"coreDatabase", coreDatabase}, {"characterCards", characterCards},
                {"chatHistory", chatHistory}, {"gameLogs", gameLogs}, {"runtimeLogs", runtimeLogs},
                {"auditLogs", auditLogs},
                {"decks", decks}, {"rules", rules}, {"help", help}, {"cardTemplates", cardTemplates},
                {"jsPlugins", jsPlugins}, {"luaMods", luaMods}, {"uploadedAssets", uploadedAssets},
                {"resourceImages", resourceImages}, {"gameLogImages", gameLogImages},
                {"chatMedia", chatMedia}, {"all", complete()}};
    }
    static Selection fromJson(const json& value) {
        Selection result;
        if (!value.is_object()) return result;

        // Version-2 settings used six broad switches.  Expand them first so
        // scheduled backups and old manifests keep their original meaning;
        // any new fine-grained key below then takes precedence.
        if (value.contains("databases") || value.contains("logs") || value.contains("resources") ||
            value.contains("plugins") || value.contains("media")) {
            const bool databases = value.value("databases", true);
            const bool logs = value.value("logs", true);
            const bool resources = value.value("resources", true);
            const bool plugins = value.value("plugins", true);
            const bool media = value.value("media", true);
            result.coreDatabase = databases;
            result.characterCards = databases;
            result.chatHistory = databases;
            result.gameLogs = databases || logs; // logs.db was included by both old categories
            result.runtimeLogs = logs;
            result.auditLogs = logs;
            result.decks = result.rules = result.help = result.cardTemplates = resources;
            result.jsPlugins = result.luaMods = plugins;
            result.uploadedAssets = result.resourceImages = result.gameLogImages = result.chatMedia = media;
        }
        result.config = value.value("config", result.config);
        result.coreDatabase = value.value("coreDatabase", result.coreDatabase);
        result.characterCards = value.value("characterCards", result.characterCards);
        result.chatHistory = value.value("chatHistory", result.chatHistory);
        result.gameLogs = value.value("gameLogs", result.gameLogs);
        result.runtimeLogs = value.value("runtimeLogs", result.runtimeLogs);
        result.auditLogs = value.value("auditLogs", result.auditLogs);
        result.decks = value.value("decks", result.decks);
        result.rules = value.value("rules", result.rules);
        result.help = value.value("help", result.help);
        result.cardTemplates = value.value("cardTemplates", result.cardTemplates);
        result.jsPlugins = value.value("jsPlugins", result.jsPlugins);
        result.luaMods = value.value("luaMods", result.luaMods);
        result.uploadedAssets = value.value("uploadedAssets", result.uploadedAssets);
        result.resourceImages = value.value("resourceImages", result.resourceImages);
        result.gameLogImages = value.value("gameLogImages", result.gameLogImages);
        result.chatMedia = value.value("chatMedia", result.chatMedia);
        return result;
    }
};

inline std::string stamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return utils::formatTimeInTimezone(now, "%Y%m%d-%H%M%S");
}

inline std::string uniqueStamp() {
    static std::atomic<unsigned long long> sequence{0};
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() % 1000;
    return stamp() + "-" + std::to_string(millis) + "-" + std::to_string(++sequence);
}

inline std::string quote(const fs::path& path) {
    std::string s = path.string();
    std::string out; out.reserve(s.size() + 2); out += '"';
    for (char c : s) { if (c == '"') out += '\\'; out += c; }
    out += '"'; return out;
}

inline bool transientLock(const std::error_code& ec) {
#ifdef _WIN32
    // ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION: the file is momentarily
    // held by another process (antivirus scan, file preview, another backup).
    return ec.value() == 32 || ec.value() == 33;
#else
    (void)ec;
    return false;
#endif
}

/// Copy one file with a few short retries.  Windows antivirus/indexers and
/// concurrent readers can hold a file for tens of milliseconds; a single
/// fs::copy_file would otherwise fail with a spurious sharing violation.
inline bool copyFileWithRetry(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) { error = "无法创建备份目录 " + to.parent_path().string() + ": " + ec.message(); return false; }
    for (int attempt = 0; attempt < 6; ++attempt) {
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (!ec) return true;
        if (!transientLock(ec) || attempt == 5) {
            error = "无法复制 " + from.string() + ": " + ec.message();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    return false;
}

/// Recursively copy a directory.  When @p skipBackups is set, the "backups"
/// store itself is excluded: archives live below data/, and copying them into
/// the staging area would both duplicate every older backup and race with a
/// backup archive that is still being written by another thread.
inline bool copyTree(const fs::path& from, const fs::path& to, std::string& error,
                     bool skipBackups = false) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return true;
    fs::create_directories(to, ec);
    if (ec) { error = "无法创建备份目录 " + to.string() + ": " + ec.message(); return false; }
    fs::recursive_directory_iterator it(from, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { error = "遍历 " + from.string() + " 失败: " + ec.message(); return false; }
        if (skipBackups && it->is_directory(ec) && it->path().filename() == "backups") {
            it.disable_recursion_pending();
            continue;
        }
        // 单实例锁文件由进程独占打开（Windows 不共享），既不是用户数据也永远
        // 复制不了，备份时必须跳过，否则 Windows 上必然报“文件被占用”。
        if (it->is_regular_file(ec) && it->path().filename() == ".instance.lock") continue;
        const auto rel = fs::relative(it->path(), from, ec);
        if (ec) { error = "解析备份路径失败: " + ec.message(); return false; }
        const auto dest = to / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(dest, ec);
            if (ec) { error = "无法创建备份目录 " + dest.string() + ": " + ec.message(); return false; }
        } else if (it->is_regular_file(ec)) {
            if (!copyFileWithRetry(it->path(), dest, error)) return false;
        } else if (ec) {
            error = "读取 " + it->path().string() + " 失败: " + ec.message();
            return false;
        }
    }
    return true;
}

inline bool copyItem(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return true;
    if (fs::is_directory(from, ec)) return copyTree(from, to, error);
    return copyFileWithRetry(from, to, error);
}

/// Copy only regular files directly inside a directory.  Used to separate the
/// exported transcript files in data/logs/ from its app/ and images/ children.
template <class Predicate>
inline bool copyFlatFiles(const fs::path& from, const fs::path& to, std::string& error, Predicate include) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return true;
    for (const auto& entry : fs::directory_iterator(from, ec)) {
        if (ec) { error = "遍历 " + from.string() + " 失败: " + ec.message(); return false; }
        if (!entry.is_regular_file(ec)) {
            if (ec) { error = "读取 " + entry.path().string() + " 失败: " + ec.message(); return false; }
            continue;
        }
        if (include(entry.path()) && !copyItem(entry.path(), to / entry.path().filename(), error)) return false;
    }
    return true;
}

inline std::string runCapture(const std::string& command, int& code);

/// Serialize manual and scheduled backups: two concurrent createArchive calls
/// would race on the same archive store (one may copy an archive the other is
/// still writing, producing exactly the Windows sharing-violation above).
inline std::atomic<bool>& backupBusyFlag() { static std::atomic<bool> value{false}; return value; }
inline bool beginBackup() { bool expected = false; return backupBusyFlag().compare_exchange_strong(expected, true); }
inline void endBackup() { backupBusyFlag().store(false); }

inline fs::path archiveDirectory(bool automatic = false) {
    // Manual and scheduled backups deliberately share one user-visible store.
    // `automatic` is retained in the API so callers can classify/clean them,
    // but it must not create a second archive location.
    (void)automatic;
    return fs::path("data") / "backups";
}

inline bool isSafeArchiveName(const std::string& name) {
    return !name.empty() && name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0 &&
           (name.rfind("dicenext_bak_", 0) == 0 || name.rfind("DiceNext-", 0) == 0);
}

inline std::string archiveListCommand(const fs::path& archive) {
#ifdef _WIN32
    return "tar -tf " + quote(archive);
#else
    return "unzip -Z1 " + quote(archive);
#endif
}

inline std::string archiveCreateCommand(const fs::path& stage, const fs::path& archive) {
#ifdef _WIN32
    return "tar -a -cf " + quote(archive) + " -C " + quote(stage) + " manifest.json config data";
#else
    return "cd " + quote(stage) + " && zip -qr " + quote(fs::absolute(archive)) + " manifest.json config data";
#endif
}

inline std::string archiveExtractCommand(const fs::path& archive, const fs::path& destination) {
#ifdef _WIN32
    return "tar -xf " + quote(archive) + " -C " + quote(destination);
#else
    return "unzip -q " + quote(archive) + " -d " + quote(destination);
#endif
}

inline std::string archiveManifestCommand(const fs::path& archive) {
#ifdef _WIN32
    return "tar -xOf " + quote(archive) + " manifest.json";
#else
    return "unzip -p " + quote(archive) + " manifest.json";
#endif
}

inline bool validateArchiveFile(const fs::path& archive, std::string& error) {
    int rc = 0;
    const std::string entries = runCapture(archiveListCommand(archive), rc);
    if (rc != 0 || entries.find("manifest.json") == std::string::npos) {
        error = "备份压缩包校验失败";
        return false;
    }
    return true;
}

inline bool createArchive(Database& db, const fs::path& configPath, fs::path& archive, std::string& error,
                          const Selection& selection, bool automatic = false) {
    if (!beginBackup()) { error = "另一个备份正在进行中，请稍后重试"; return false; }
    struct BackupGuard { ~BackupGuard() { endBackup(); } } backupGuard;
    std::error_code ec;
    fs::path stage = fs::temp_directory_path() / ("dicenext-backup-" + uniqueStamp());
    fs::remove_all(stage, ec); fs::create_directories(stage / "config", ec);
    if (ec) { error = ec.message(); return false; }
    if (!db.checkpoint()) { error = "无法完成 SQLite checkpoint"; fs::remove_all(stage, ec); return false; }
    fs::create_directories(stage / "data", ec);
    if (ec) { error = ec.message(); fs::remove_all(stage, ec); return false; }
    if (selection.complete()) {
        if (!copyTree("data", stage / "data", error, true)) { fs::remove_all(stage, ec); return false; }
    } else {
        auto copy = [&](const fs::path& item) {
            return copyItem(item, stage / item, error);
        };
        if (selection.coreDatabase && !copyFlatFiles("data", stage / "data", error, [](const fs::path& path) {
                if (path.extension() != ".db") return false;
                const std::string name = path.filename().string();
                return name != "cards.db" && name != "chat.db" && name != "logs.db" &&
                    name != "plugins.db" && name != "lua_mod.db";
            })) { fs::remove_all(stage, ec); return false; }
        if (selection.characterCards && !copy("data/cards.db")) { fs::remove_all(stage, ec); return false; }
        if (selection.chatHistory && !copy("data/chat.db")) { fs::remove_all(stage, ec); return false; }
        if (selection.gameLogs) {
            if (!copy("data/logs.db") || !copyFlatFiles("data/logs", stage / "data/logs", error,
                    [](const fs::path& path) { return path.filename().string().rfind("crash_", 0) != 0; })) {
                fs::remove_all(stage, ec); return false;
            }
        }
        if (selection.runtimeLogs) {
            if (!copy("data/logs/app") ||
                !copyFlatFiles("data/logs", stage / "data/logs", error,
                    [](const fs::path& path) { return path.filename().string().rfind("crash_", 0) == 0; })) {
                fs::remove_all(stage, ec); return false;
            }
        }
        if (selection.auditLogs && !copy("data/audit")) { fs::remove_all(stage, ec); return false; }
        if (selection.decks && !copy("data/decks")) { fs::remove_all(stage, ec); return false; }
        if (selection.rules && (!copy("data/rules") || !copy("data/rulepacks"))) { fs::remove_all(stage, ec); return false; }
        if (selection.help && (!copy("data/help") || !copy("data/helpdoc"))) { fs::remove_all(stage, ec); return false; }
        if (selection.cardTemplates && !copy("data/card-templates")) { fs::remove_all(stage, ec); return false; }
        if (selection.jsPlugins) {
            for (const auto& item : {fs::path("data/plugins"), fs::path("data/plugins.db"),
                                     fs::path("data/js_kv.json"), fs::path("data/js_kv.json.bak")})
                if (!copy(item)) { fs::remove_all(stage, ec); return false; }
        }
        if (selection.luaMods) {
            for (const auto& item : {fs::path("data/plugin"), fs::path("data/mod"), fs::path("data/lua_mod.db"),
                                     fs::path("data/self_data")})
                if (!copy(item)) { fs::remove_all(stage, ec); return false; }
        }
        if (selection.uploadedAssets && !copy("data/assets")) { fs::remove_all(stage, ec); return false; }
        if (selection.resourceImages && !copy("data/images")) { fs::remove_all(stage, ec); return false; }
        if (selection.gameLogImages && !copy("data/logs/images")) { fs::remove_all(stage, ec); return false; }
        if (selection.chatMedia && (!copy("data/chat/images") || !copy("data/chat_images"))) {
            fs::remove_all(stage, ec); return false;
        }
    }
    if (selection.config && fs::exists(configPath, ec)) {
        const fs::path configDir = fs::is_directory(configPath, ec)
            ? configPath : (configPath.parent_path().empty() ? fs::path("config") : configPath.parent_path());
        if (!copyTree(configDir, stage / "config", error)) { fs::remove_all(stage, ec); return false; }
    }
    json manifest{{"format", "dice-next-backup"}, {"version", 3}, {"created_at", stamp()}, {"automatic", automatic},
                  {"selection", selection.toJson()}, {"includes", json::array({"data", "config"})}};
    { std::ofstream f(stage / "manifest.json", std::ios::binary); f << manifest.dump(2); }
    fs::path backupDir = archiveDirectory(automatic); fs::create_directories(backupDir, ec);
    if (ec) { error = ec.message(); fs::remove_all(stage, ec); return false; }
    archive = backupDir / ("dicenext_bak_" + stamp() + ".zip");
    if (fs::exists(archive, ec)) { error = "同一秒内已有备份，请稍后重试"; fs::remove_all(stage, ec); return false; }
    std::string cmd = archiveCreateCommand(stage, archive);
    if (std::system(cmd.c_str()) != 0 || !fs::is_regular_file(archive, ec) || !validateArchiveFile(archive, error)) {
        if (error.empty()) error = "无法创建备份压缩包（需要系统 ZIP 工具）";
        fs::remove_all(stage, ec); return false;
    }
    fs::remove_all(stage, ec);
    return true;
}

inline bool createArchive(Database& db, const fs::path& configPath, fs::path& archive, std::string& error,
                          bool automatic = false) {
    return createArchive(db, configPath, archive, error, Selection::full(), automatic);
}

inline std::string runCapture(const std::string& command, int& code) {
    std::string out;
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) { code = -1; return {}; }
    char buf[4096]; while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
    code = _pclose(pipe);
#else
    code = pclose(pipe);
#endif
    return out;
}

inline bool archiveIsAutomatic(const fs::path& archive, bool fallback) {
    int rc = 0;
    try {
        const std::string content = runCapture(archiveManifestCommand(archive), rc);
        if (rc == 0) return json::parse(content).value("automatic", fallback);
    } catch (...) {}
    return fallback;
}

inline json listArchives(bool automaticOnly = false) {
    json out = json::array();
    std::error_code ec;
    const fs::path dir = archiveDirectory();
    if (!fs::is_directory(dir, ec)) { ec.clear(); return out; }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (!isSafeArchiveName(name)) continue;
        // Archives produced before the unified directory were only placed in
        // data/backups by the scheduler, so missing metadata means automatic.
        const bool automatic = archiveIsAutomatic(entry.path(), true);
        if (automaticOnly && !automatic) continue;
        const auto when = entry.last_write_time(ec);
        const auto size = entry.file_size(ec);
        if (ec) { ec.clear(); continue; }
        const auto systemWhen = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            when - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        out.push_back({{"name", name}, {"size", size},
                       {"createdAt", std::chrono::duration_cast<std::chrono::seconds>(systemWhen.time_since_epoch()).count()},
                       {"automatic", automatic}});
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a.value("createdAt", 0LL) > b.value("createdAt", 0LL);
    });
    return out;
}

inline bool deleteArchive(const std::string& name, bool automatic, std::string& error) {
    if (!isSafeArchiveName(name)) { error = "无效的备份文件名"; return false; }
    std::error_code ec;
    const fs::path path = archiveDirectory(automatic) / name;
    if (!fs::is_regular_file(path, ec) || !fs::remove(path, ec)) {
        error = ec ? ec.message() : "备份文件不存在";
        return false;
    }
    return true;
}

inline void cleanupArchives(int keepDays) {
    if (keepDays < 1) return;
    json archives = listArchives(true);
    const auto threshold = std::chrono::system_clock::now() - std::chrono::hours(24 * keepDays);
    const auto thresholdSeconds = std::chrono::duration_cast<std::chrono::seconds>(threshold.time_since_epoch()).count();
    std::string ignored;
    for (const auto& archive : archives) {
        if (archive.value("createdAt", 0LL) < thresholdSeconds)
            deleteArchive(archive.value("name", std::string()), true, ignored);
    }
}

inline bool allowedArchivePath(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos) return false;
    return name == "manifest.json" || name == "config/" || name.rfind("config/", 0) == 0
        || name == "data/" || name.rfind("data/", 0) == 0;
}

inline bool stageRestoreArchive(const fs::path& archive, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) { error = "备份文件不存在"; return false; }
    int rc = 0; std::string entries = runCapture(archiveListCommand(archive), rc);
    if (rc != 0) { error = "不是有效的 Dice!Next 备份压缩包"; return false; }
    std::istringstream lines(entries); std::string line; bool manifestSeen = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!allowedArchivePath(line)) { error = "备份包含不允许的文件路径"; return false; }
        if (line == "manifest.json") manifestSeen = true;
    }
    if (!manifestSeen) { error = "备份缺少 manifest.json"; return false; }
    fs::path stage = "restore-pending"; fs::remove_all(stage, ec); fs::create_directories(stage, ec);
    if (ec) { error = ec.message(); return false; }
    if (std::system(archiveExtractCommand(archive, stage).c_str()) != 0) {
        error = "无法解压备份文件"; fs::remove_all(stage, ec); return false;
    }
    // M4: 解压后二次校验——落盘真实路径必须位于暂存目录内（防 zip-slip 绕过字符串校验）。
    {
        std::error_code ec2;
        const fs::path stageReal = fs::canonical(stage, ec2);
        if (ec2) { error = "无法解析暂存目录"; fs::remove_all(stage, ec); return false; }
        std::string stagePrefix = stageReal.string();
        if (!stagePrefix.empty() && stagePrefix.back() != '/') stagePrefix += '/';
        for (auto& entry : fs::recursive_directory_iterator(stage, ec2)) {
            if (ec2) { error = "读取解压内容失败"; fs::remove_all(stage, ec); return false; }
            if (!entry.is_regular_file(ec2)) continue;
            const fs::path real = fs::canonical(entry.path(), ec2);
            if (ec2 || real.string().rfind(stagePrefix, 0) != 0) {
                error = "备份解压路径越界"; fs::remove_all(stage, ec); return false;
            }
        }
    }
    try {
        std::ifstream f(stage / "manifest.json"); json manifest; f >> manifest;
        const int version = manifest.value("version", 0);
        if (manifest.value("format", std::string()) != "dice-next-backup" || (version != 1 && version != 2 && version != 3))
            throw std::runtime_error("format");
    } catch (...) { error = "备份清单格式不受支持"; fs::remove_all(stage, ec); return false; }
    return true;
}

inline bool stageRestore(const std::string& bytes, std::string& error) {
    if (bytes.empty()) { error = "备份文件为空"; return false; }
    std::error_code ec;
    fs::path uploadDir = archiveDirectory(); fs::create_directories(uploadDir, ec);
    if (ec) { error = ec.message(); return false; }
    fs::path archive = uploadDir / ("restore-upload-" + uniqueStamp() + ".zip");
    { std::ofstream f(archive, std::ios::binary); f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    return stageRestoreArchive(archive, error);
}

inline bool stageStoredRestore(const std::string& name, bool automatic, std::string& error) {
    if (!isSafeArchiveName(name)) { error = "无效的备份文件名"; return false; }
    const fs::path archive = archiveDirectory(automatic) / name;
    return stageRestoreArchive(archive, error);
}

// Called after logging is ready but before ConfigManager and Database are opened.
inline bool applyPendingRestore(std::string& notice) {
    std::error_code ec; fs::path stage = "restore-pending";
    if (!fs::is_regular_file(stage / "manifest.json", ec)) return true;
    fs::path rollback = "restore-rollbacks"; rollback /= stamp();
    fs::create_directories(rollback, ec);
    if (ec) { notice = "恢复失败：无法创建回滚目录：" + ec.message(); return false; }
    try {
        json manifest;
        { std::ifstream f(stage / "manifest.json"); f >> manifest; }
        const bool partial = manifest.value("version", 1) >= 2 &&
            !Selection::fromJson(manifest.value("selection", json::object())).complete();
        if (partial) {
            // A partial archive overlays only its selected paths.  Keep a full
            // rollback copy so individual plugins/resources can be restored
            // without ever deleting unrelated current data.
            std::string copyError;
            if (!copyTree("data", rollback / "data", copyError)) throw std::runtime_error(copyError);
            if (fs::exists("config")) copyTree("config", rollback / "config", copyError);
            if (fs::exists(stage / "data") && !copyTree(stage / "data", "data", copyError)) throw std::runtime_error(copyError);
            if (fs::exists(stage / "config") && !copyTree(stage / "config", "config", copyError)) throw std::runtime_error(copyError);
            fs::remove_all(stage, ec);
            notice = "已应用局部待恢复备份；恢复前的数据保存在 " + rollback.string();
            return true;
        }
        if (fs::exists("data")) fs::rename("data", rollback / "data");
        if (fs::exists("config")) fs::rename("config", rollback / "config");
        if (fs::exists(stage / "data")) fs::rename(stage / "data", "data"); else fs::create_directories("data");
        if (fs::exists(stage / "config")) fs::rename(stage / "config", "config");
        fs::remove_all(stage, ec);
        notice = "已应用待恢复备份；恢复前的数据保存在 " + rollback.string();
        return true;
    } catch (const std::exception& e) {
        // Best effort rollback if the replacement did not complete.
        try { if (!fs::exists("data") && fs::exists(rollback / "data")) fs::rename(rollback / "data", "data"); } catch (...) {}
        notice = "恢复失败：" + std::string(e.what()); return false;
    }
}

} // namespace dice::backup
