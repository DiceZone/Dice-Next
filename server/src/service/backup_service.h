#pragma once

// Local full backup / restore service.  A restore is staged while the process
// is running and applied at the next startup, before any database is opened.
// This avoids overwriting live SQLite files or loaded plugins in place.

#include "../storage/database.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <algorithm>

namespace dice::backup {
namespace fs = std::filesystem;
using json = nlohmann::json;

inline std::string stamp() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char out[32]; std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &tm);
    return out;
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

inline bool copyTree(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return true;
    fs::create_directories(to, ec);
    if (ec) { error = ec.message(); return false; }
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) { error = ec.message(); return false; }
    return true;
}

inline std::string runCapture(const std::string& command, int& code);

inline fs::path archiveDirectory(bool automatic = false) {
    return automatic ? fs::path("data") / "backups" : fs::path("backups");
}

inline bool isAutomaticArchiveName(const std::string& name) {
    return name.rfind("DiceNext-auto-backup-", 0) == 0;
}

inline bool isSafeArchiveName(const std::string& name) {
    return !name.empty() && name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.rfind("DiceNext-", 0) == 0 && name.size() > 4 &&
           name.compare(name.size() - 4, 4, ".zip") == 0;
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
                          bool automatic = false) {
    std::error_code ec;
    fs::path stage = fs::temp_directory_path() / ("dicenext-backup-" + uniqueStamp());
    fs::remove_all(stage, ec); fs::create_directories(stage / "config", ec);
    if (ec) { error = ec.message(); return false; }
    if (!db.checkpoint()) { error = "无法完成 SQLite checkpoint"; fs::remove_all(stage, ec); return false; }
    fs::create_directories(stage / "data", ec);
    if (ec) { error = ec.message(); fs::remove_all(stage, ec); return false; }
    if (!copyTree("data", stage / "data", error)) { fs::remove_all(stage, ec); return false; }
    if (fs::exists(configPath, ec)) {
        fs::copy_file(configPath, stage / "config" / "default_config.json", fs::copy_options::overwrite_existing, ec);
        if (ec) { error = ec.message(); fs::remove_all(stage, ec); return false; }
    }
    json manifest{{"format", "dice-next-backup"}, {"version", 1}, {"created_at", stamp()},
                  {"includes", json::array({"data", "config/default_config.json"})}};
    { std::ofstream f(stage / "manifest.json", std::ios::binary); f << manifest.dump(2); }
    fs::path backupDir = archiveDirectory(automatic); fs::create_directories(backupDir, ec);
    if (ec) { error = ec.message(); fs::remove_all(stage, ec); return false; }
    archive = backupDir / (std::string("DiceNext-") + (automatic ? "auto-" : "") + "backup-" + uniqueStamp() + ".zip");
    std::string cmd = archiveCreateCommand(stage, archive);
    if (std::system(cmd.c_str()) != 0 || !fs::is_regular_file(archive, ec) || !validateArchiveFile(archive, error)) {
        if (error.empty()) error = "无法创建备份压缩包（需要系统 ZIP 工具）";
        fs::remove_all(stage, ec); return false;
    }
    fs::remove_all(stage, ec);
    return true;
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

inline json listArchives(bool automaticOnly = false) {
    json out = json::array();
    std::error_code ec;
    for (const bool automatic : {false, true}) {
        if (automaticOnly && !automatic) continue;
        const fs::path dir = archiveDirectory(automatic);
        if (!fs::is_directory(dir, ec)) { ec.clear(); continue; }
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec || !entry.is_regular_file(ec)) continue;
            const std::string name = entry.path().filename().string();
            if (!isSafeArchiveName(name) || isAutomaticArchiveName(name) != automatic) continue;
            const auto when = entry.last_write_time(ec);
            const auto size = entry.file_size(ec);
            if (ec) { ec.clear(); continue; }
            const auto systemWhen = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                when - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            out.push_back({{"name", name}, {"size", size},
                           {"createdAt", std::chrono::duration_cast<std::chrono::seconds>(systemWhen.time_since_epoch()).count()},
                           {"automatic", automatic}});
        }
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return a.value("createdAt", 0LL) > b.value("createdAt", 0LL);
    });
    return out;
}

inline bool deleteArchive(const std::string& name, std::string& error) {
    if (!isSafeArchiveName(name)) { error = "无效的备份文件名"; return false; }
    std::error_code ec;
    const fs::path path = archiveDirectory(isAutomaticArchiveName(name)) / name;
    if (!fs::is_regular_file(path, ec) || !fs::remove(path, ec)) {
        error = ec ? ec.message() : "备份文件不存在";
        return false;
    }
    return true;
}

inline void cleanupArchives(int keepCount) {
    if (keepCount < 1) return;
    json archives = listArchives(true);
    std::string ignored;
    for (size_t i = static_cast<size_t>(keepCount); i < archives.size(); ++i)
        deleteArchive(archives[i].value("name", std::string()), ignored);
}

inline bool allowedArchivePath(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos) return false;
    return name == "manifest.json" || name == "config/" || name == "config/default_config.json"
        || name == "data/" || name.rfind("data/", 0) == 0;
}

inline bool stageRestore(const std::string& bytes, std::string& error) {
    if (bytes.empty()) { error = "备份文件为空"; return false; }
    std::error_code ec;
    fs::path uploadDir = archiveDirectory(); fs::create_directories(uploadDir, ec);
    if (ec) { error = ec.message(); return false; }
    fs::path archive = uploadDir / ("restore-upload-" + uniqueStamp() + ".zip");
    { std::ofstream f(archive, std::ios::binary); f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
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
    try {
        std::ifstream f(stage / "manifest.json"); json manifest; f >> manifest;
        if (manifest.value("format", std::string()) != "dice-next-backup" || manifest.value("version", 0) != 1)
            throw std::runtime_error("format");
    } catch (...) { error = "备份清单格式不受支持"; fs::remove_all(stage, ec); return false; }
    return true;
}

// Called after logging is ready but before ConfigManager and Database are opened.
inline bool applyPendingRestore(std::string& notice) {
    std::error_code ec; fs::path stage = "restore-pending";
    if (!fs::is_regular_file(stage / "manifest.json", ec)) return true;
    fs::path rollback = "restore-rollbacks"; rollback /= stamp();
    fs::create_directories(rollback, ec);
    if (ec) { notice = "恢复失败：无法创建回滚目录：" + ec.message(); return false; }
    try {
        if (fs::exists("data")) fs::rename("data", rollback / "data");
        if (fs::exists("config/default_config.json")) {
            fs::create_directories(rollback / "config");
            fs::copy_file("config/default_config.json", rollback / "config" / "default_config.json", fs::copy_options::overwrite_existing);
        }
        if (fs::exists(stage / "data")) fs::rename(stage / "data", "data"); else fs::create_directories("data");
        if (fs::exists(stage / "config" / "default_config.json")) {
            fs::create_directories("config");
            fs::copy_file(stage / "config" / "default_config.json", "config/default_config.json", fs::copy_options::overwrite_existing);
        }
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
