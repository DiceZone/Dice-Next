#pragma once
// Dependency-free (apart from header-only JSON) POSIX package transactions.
// The manager owns these paths only while no core process is running.
#ifndef _WIN32
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace dice::update::posix {
namespace fs = std::filesystem;
inline constexpr int kRestartExitCode = 42;

inline fs::path executablePath() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return fs::canonical(buffer.data());
#else
    std::vector<char> buffer(65536);
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    return size > 0 && static_cast<size_t>(size) < buffer.size()
        ? fs::path(std::string(buffer.data(), size)) : fs::path();
#endif
}

inline bool relativePathSafe(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) {
        const auto value = part.string();
        if (value == ".." || value == ".") return false;
        for (unsigned char ch : value) if (ch < 32 || ch == 127 || ch == '\\') return false;
    }
    return true;
}

inline bool pathExists(const fs::path& path) {
    return fs::symlink_status(path).type() != fs::file_type::not_found;
}

// Never follow a local symlink while creating, replacing or restoring files.
inline void safeParents(const fs::path& root, const fs::path& relative, bool create) {
    if (!relativePathSafe(relative)) throw std::runtime_error("unsafe update path");
    auto current = root;
    for (const auto& part : relative.parent_path()) {
        current /= part;
        if (!pathExists(current)) {
            if (create) fs::create_directory(current);
        } else if (fs::symlink_status(current).type() != fs::file_type::directory) {
            throw std::runtime_error("update parent is not a real directory: " + current.string());
        }
    }
}

inline void writeJson(const fs::path& path, const nlohmann::json& value) {
    const auto temporary = path.string() + ".tmp";
    const std::string text = value.dump(2) + '\n';
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("cannot write update journal");
    size_t offset = 0;
    while (offset < text.size()) {
        const auto written = ::write(fd, text.data() + offset, text.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ::close(fd); throw std::runtime_error("cannot persist update journal"); }
        offset += static_cast<size_t>(written);
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) throw std::runtime_error("cannot sync update journal");
    fs::rename(temporary, path);
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) { ::fsync(directory); ::close(directory); }
}

inline nlohmann::json readJson(const fs::path& path) {
    if (fs::symlink_status(path).type() != fs::file_type::regular || fs::file_size(path) > 1024 * 1024)
        throw std::runtime_error("invalid update metadata");
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}

inline bool managedLaunch(const fs::path& root) {
    const char* managed = std::getenv("DICENEXT_MANAGED");
    const char* parent = std::getenv("DICENEXT_MANAGER_PID");
    if (!managed || std::string(managed) != "1" || !parent) return false;
    try {
        if (std::stoll(parent) != ::getppid()) return false;
        return fs::symlink_status(root / "dice-next").type() == fs::file_type::regular &&
            fs::symlink_status(root / "dice-next-server").type() == fs::file_type::regular &&
            ::access((root / "dice-next").c_str(), X_OK) == 0 &&
            ::access((root / "dice-next-server").c_str(), X_OK) == 0;
    } catch (...) { return false; }
}

inline bool destinationAllowed(const fs::path& relative) {
    const auto path = relative.generic_string();
    const std::set<std::string> fixed{
        "dice-next", "dice-next-server", "start.sh", "README.txt", "update-mirrors.json",
        "lib", "i18n", "web", "docs", "decks", "card-templates"};
    return relativePathSafe(relative) && (fixed.contains(path) || path.rfind("data/plugins/", 0) == 0 ||
                                         path.rfind("data/helpdoc/", 0) == 0);
}

inline void validatePackage(const fs::path& stage) {
    for (const auto* file : {"dice-next", "dice-next-server", "start.sh", "web/dist/index.html", "docs/roadmap.md"}) {
        safeParents(stage, file, false);
        if (fs::symlink_status(stage / file).type() != fs::file_type::regular)
            throw std::runtime_error(std::string("incomplete POSIX package: ") + file);
    }
    for (const auto* file : {"dice-next", "dice-next-server", "start.sh"})
        if (::access((stage / file).c_str(), X_OK) != 0)
            throw std::runtime_error(std::string("package executable is not executable: ") + file);
    if (fs::symlink_status(stage / "i18n").type() != fs::file_type::directory)
        throw std::runtime_error("incomplete POSIX package: i18n");
    for (const auto& entry : fs::recursive_directory_iterator(stage)) {
        const auto type = entry.symlink_status().type();
        if (type == fs::file_type::directory || type == fs::file_type::regular) continue;
        const auto relative = entry.path().lexically_relative(stage);
        if (type != fs::file_type::symlink || relative.generic_string().rfind("lib/", 0) != 0)
            throw std::runtime_error("unsupported special file in update package");
        const auto target = fs::read_symlink(entry.path());
        const auto resolved = (relative.parent_path() / target).lexically_normal();
        if (target.is_absolute() || !relativePathSafe(resolved) || resolved.generic_string().rfind("lib/", 0) != 0)
            throw std::runtime_error("library symlink escapes package");
        const auto canonical = fs::canonical(entry.path()); // Also rejects dangling/cyclic links.
        const auto canonicalRelative = canonical.lexically_relative(fs::canonical(stage / "lib"));
        if (!relativePathSafe(canonicalRelative) || !fs::is_regular_file(canonical))
            throw std::runtime_error("library symlink escapes lib directory");
    }
}

class Transaction {
public:
    explicit Transaction(fs::path root) : root_(fs::canonical(root)), stage_(root_ / "updates/pending"),
        journal_(root_ / "updates/transaction.json") {
        safeParents(root_, "updates/transaction.json", true);
        safeParents(root_, "updates/pending/update.json", false);
    }
    bool active() const { return pathExists(journal_); }

    void begin() {
        if (active()) throw std::runtime_error("an interrupted update needs recovery");
        validatePackage(stage_);
        metadata_ = readJson(stage_ / "update.json");
        const std::string id = std::to_string(::getpid()) + "-" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        backup_ = fs::path("updates/rollbacks") / id;
        state_ = {{"schema", 1}, {"backup", backup_.generic_string()}, {"metadata", metadata_},
                  {"moves", nlohmann::json::array()}, {"committed", false}};
        std::vector<fs::path> paths;
        for (const auto* item : {"lib", "i18n", "web", "docs", "decks", "card-templates",
                                 "dice-next-server", "start.sh", "README.txt",
                                 "update-mirrors.json", "dice-next"}) {
            if (pathExists(stage_ / item)) paths.emplace_back(item);
        }
        for (const auto* directory : {"data/plugins", "data/helpdoc"})
            if (pathExists(stage_ / directory))
                for (const auto& entry : fs::recursive_directory_iterator(stage_ / directory))
                    if (entry.is_regular_file()) paths.push_back(entry.path().lexically_relative(stage_));
        // Check every destination before moving anything. Databases, config and
        // user decks are never on this allowlist; plugins and help merge by file.
        for (const auto& relative : paths) {
            if (!destinationAllowed(relative)) throw std::runtime_error("unsupported update destination");
            safeParents(root_, relative, false);
        }
        writeJson(journal_, state_);
        for (const auto& relative : paths) {
            safeParents(root_, relative, true);
            safeParents(root_, backup_ / relative, true);
            state_["moves"].push_back({{"path", relative.generic_string()}, {"existing", pathExists(root_ / relative)}});
            // Persist intent BEFORE either rename, so an interrupted process
            // between backup and replacement can recover on the next launch.
            writeJson(journal_, state_);
            if (pathExists(root_ / relative)) fs::rename(root_ / relative, root_ / backup_ / relative);
            fs::rename(stage_ / relative, root_ / relative);
        }
        state_["prepared"] = true;
        writeJson(journal_, state_);
    }

    void resume() {
        state_ = readJson(journal_);
        if (!state_.value("prepared", false) || state_.value("committed", false))
            throw std::runtime_error("invalid prepared update transaction");
        metadata_ = state_.at("metadata");
        backup_ = fs::path(state_.at("backup").get<std::string>());
        if (!relativePathSafe(backup_) || backup_.parent_path() != fs::path("updates/rollbacks"))
            throw std::runtime_error("invalid prepared rollback path");
        for (const auto& move : state_.at("moves"))
            if (!destinationAllowed(fs::path(move.at("path").get<std::string>())))
                throw std::runtime_error("invalid prepared destination");
    }

    void result(bool success, const std::string& message) {
        writeJson(root_ / "updates/last-result.json",
            {{"schema", 1}, {"success", success}, {"metadata", metadata_}, {"message", message}});
    }

    void commit() {
        state_["committed"] = true;
        writeJson(journal_, state_);
        result(true, "更新文件已应用，新核心已启动");
        fs::remove_all(stage_);
        fs::remove(journal_);
    }

    void recover(const std::string& reason = "检测到中断的更新，已回滚程序文件") {
        if (!active()) return;
        state_ = readJson(journal_);
        metadata_ = state_.at("metadata");
        backup_ = fs::path(state_.at("backup").get<std::string>());
        if (!relativePathSafe(backup_) || backup_.parent_path() != fs::path("updates/rollbacks"))
            throw std::runtime_error("invalid rollback path");
        if (state_.value("committed", false)) {
            result(true, "更新文件已应用");
            fs::remove_all(stage_);
            fs::remove(journal_);
            return;
        }
        for (const auto& move : state_.at("moves")) {
            const fs::path relative(move.at("path").get<std::string>());
            if (!destinationAllowed(relative)) throw std::runtime_error("invalid rollback destination");
            safeParents(root_, relative, false);
            safeParents(root_, backup_ / relative, false);
        }
        const auto& moves = state_.at("moves");
        for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
            const fs::path relative(it->at("path").get<std::string>());
            const auto destination = root_ / relative;
            const auto saved = root_ / backup_ / relative;
            if (pathExists(saved)) {
                if (pathExists(destination)) fs::remove_all(destination);
                fs::rename(saved, destination);
            } else if (!it->at("existing").get<bool>() && !pathExists(stage_ / relative)) {
                if (pathExists(destination)) fs::remove_all(destination);
            }
        }
        result(false, reason);
        // Keep the failed package for inspection, but never retry it at every start.
        if (pathExists(stage_)) fs::rename(stage_, root_ / "updates" / ("failed-" + backup_.filename().string()));
        fs::remove(journal_);
    }

private:
    fs::path root_, stage_, journal_, backup_;
    nlohmann::json state_, metadata_;
};
} // namespace dice::update::posix
#endif
