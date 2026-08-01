#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <vector>
#include <atomic>
#include <nlohmann/json.hpp>

namespace dice {

// ─── Type alias ──────────────────────────────────────────────

using json = nlohmann::json;
using ConfigChangeCallback = std::function<void()>;

// ─── ConfigManager ───────────────────────────────────────────
// Thread-safe JSON configuration-directory manager with hot-reload support.
//
// Usage:
//   ConfigManager mgr("config");
//   mgr.load();
//   int port = mgr.get<int>("server/port");
//   mgr.set<std::string>("server/host", "127.0.0.1");
//   mgr.save();
//   mgr.onConfigChanged([] { std::cout << "reloaded!\n"; });

class ConfigManager {
public:
    explicit ConfigManager(const std::string& configPath);
    ~ConfigManager() = default;

    // ─── Lifecycle ───────────────────────────────────────────

    /// Load configuration from the configuration directory. If the directory
    /// does not exist, creates all functional JSON files from built-in defaults.
    /// Returns true on success.
    bool load();

    /// Reload configuration from disk (hot-reload entry point).
    /// Preserves the file path; emits change callbacks on success.
    /// Returns true on success.
    bool reload();

    /// Persist current in-memory configuration back to its functional files.
    /// Returns true on success.
    bool save();

    /// Restore a validated full configuration snapshot, normally recovered from
    /// the database after an invalid on-disk edit.
    bool restoreSnapshot(const json& snapshot);

    /// When normal validation fails, read only a string db_path from server.json
    /// so startup can locate its last snapshot without loading the config.
    std::string recoveryDatabasePath(const std::string& fallback) const;

    /// Reset to factory defaults (does NOT write to disk).
    void resetDefault();

    // ─── Typed Accessors ─────────────────────────────────────
    // Keys use "/" as path separator for nested JSON objects.
    // E.g., "server/port" accesses config["server"]["port"].

    template <typename T>
    T get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return getImpl<T>(key);
    }

    template <typename T>
    T get(const std::string& key, const T& defaultValue) const {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            return getImpl<T>(key);
        } catch (...) {
            return defaultValue;
        }
    }

    template <typename T>
    void set(const std::string& key, const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        setImpl(key, value);
    }

    /// Retrieve the entire configuration tree as a JSON object.
    json getAll() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    // ─── Hot-reload Callbacks ────────────────────────────────

    /// Register a callback to be invoked whenever the
    /// configuration is reloaded from disk.
    void onConfigChanged(ConfigChangeCallback callback);

    /// Manually trigger all registered change callbacks.
    void emitConfigChanged();

    /// Installs the durable snapshot sink after the database is open. Every
    /// successful save writes the full validated configuration to this sink.
    void setSnapshotWriter(std::function<void(const json&)> writer);

    // ─── Path ────────────────────────────────────────────────

    const std::string& configPath() const noexcept { return configPath_; }

    /// True after the configuration has been loaded from the split directory.
    bool usingSplitLayout() const noexcept { return splitLayout_; }

    /// True only for the startup that had to create a fresh config directory.
    /// The caller can use this to export existing runtime state once.
    bool createdOnLoad() const noexcept { return createdOnLoad_; }

    /// True when startup found only the obsolete default_config.json.  That
    /// file is deliberately discarded and startup must recover from the
    /// database snapshot instead of persisting factory defaults over it.
    bool discardedObsoleteDefaultConfig() const noexcept { return discardedObsoleteDefaultConfig_; }

private:
    std::string configPath_;
    json config_;
    mutable std::mutex mutex_;
    std::vector<ConfigChangeCallback> changeCallbacks_;
    std::function<void(const json&)> snapshotWriter_;
    bool splitLayout_ = false;
    bool createdOnLoad_ = false;
    bool discardedObsoleteDefaultConfig_ = false;

    std::atomic<int> writing_{0};  // >0 when save() is writing (suppress self-triggered reload)
    // ─── Internal helpers ────────────────────────────────────

    json* navigateTo(const std::string& key, bool createMissing = false);
    const json* navigateTo(const std::string& key) const;

    template <typename T>
    T getImpl(const std::string& key) const {
        const json* node = navigateTo(key);
        if (!node) {
            throw std::runtime_error("ConfigManager: key not found: " + key);
        }
        return node->get<T>();
    }

    template <typename T>
    void setImpl(const std::string& key, const T& value) {
        json* node = navigateTo(key, true);
        if (node) {
            *node = value;
        }
    }
};

}  // namespace dice
