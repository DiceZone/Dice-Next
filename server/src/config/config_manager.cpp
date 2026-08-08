#include "config_manager.h"
#include "../common/logger.h"
#include "../common/utils.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cctype>

namespace dice {

namespace fs = std::filesystem;

// ─── Built-in defaults ───────────────────────────────────────

static json makeDefaultConfig() {
    return json{
        {"server", {
            {"host", "0.0.0.0"},
            {"port", 18088},
            {"api_key", ""},
            {"db_path", "./data/dice.db"},
            {"log_level", "info"}
        }},
        {"dice", {
            {"command_prefix", "."},
            {"self_name", ""},                   // strSelfName: 自我介绍场合的名称（空=用登录昵称）
            {"self_call", ""},                   // strSelfCall: 回执自称，{self} 的重定向目标（空=用 self_name）
            {"message_format", "traditional"},   // 出站消息表现形式：traditional / card
            {"blacklist_quit_level", "member"},  // 黑名单退群默认等级: member=任一成员触发 / admin=仅群主级触发
            {"respond_self", false},             // 自响应：用骰娘账号自身消息自控（默认关）
            {"console_start_hidden", true},      // 启动即最小化到托盘（隐藏控制台，退出走托盘）
            {"scoped_overrides", json::object()}, // #17: adapter/account overrides; account > adapter > global
            {"rules", {
                {"coc_enabled", true},
                {"coc_critical_range", 1},
                {"coc_fumble_range", 95},
                {"dnd_enabled", true},
                {"dnd_advantage", false},
                {"fate_enabled", false},
                {"l5r_enabled", false},
                {"default_dice_sides", 100},
                {"custom_rules", json::object()}
            }}
        }},
        {"events", {
            {"auto_approve_friend", false},   // 加好友请求自动同意
            {"friend_keyword", ""},           // 仅当验证信息含此关键词才同意（空=不限）
            {"auto_approve_group", false},     // 加群/邀请请求自动同意
            {"group_keyword", ""},            // 仅当验证信息含此关键词才同意（空=不限）
            {"friend_welcome", ""},            // 被加好友后的私聊欢迎语（空=用内置文案）
            {"welcome_min_delay", 0},            // welcome delay 全局最低值（秒）
            {"welcome_min_cooldown", 0}           // welcome cooldown 全局最低值（秒）
        }},
        {"webui", {
            {"password", ""}
        }},
        {"i18n", {
            {"resource_dir", "i18n"},
            {"default_locale", "zh-Hans"}
        }},
        {"adapters", json::array()},
        {"backup", {
            {"auto_enabled", false},
            {"auto_schedule", "interval"},
            {"auto_interval_hours", 24},
            {"auto_daily_time", "04:00"},
            {"auto_keep_days", 7},
            {"auto_selection", {{"config", true}, {"databases", true}, {"logs", true},
                                {"resources", true}, {"plugins", true}, {"media", true}}},
            {"auto_last_at", 0}
        }},
        {"hot_reload", {
            {"enabled", true},
            {"debounce_ms", 500},
            {"watch_paths", json::array({"./config", "./data"})}
        }}
    };
}

static constexpr const char* kRequiredSections[] = {
    "server", "dice", "events", "webui", "i18n", "adapters", "backup", "hot_reload"
};

static bool isSafeSectionName(const std::string& name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

static fs::path configDirectory(const std::string& configPath) {
    return fs::path(configPath);
}

static bool writeJsonFile(const fs::path& path, const json& value, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) { error = ec.message(); return false; }
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "无法写入 " + path.string(); return false; }
        out << value.dump(2) << '\n';
        if (!out) { error = "写入 " + path.string() + " 失败"; return false; }
    }
    fs::remove(path, ec); ec.clear();
    fs::rename(temp, path, ec);
    if (ec) { error = ec.message(); fs::remove(temp, ec); return false; }
    return true;
}

static bool validateConfig(const json& value, std::string& error) {
    if (!value.is_object()) { error = "配置根节点必须是对象"; return false; }
    const auto server = value.value("server", json::object());
    if (!server.is_object()) { error = "server 配置必须是对象"; return false; }
    const int port = server.value("port", 18088);
    if (port < 1 || port > 65535) { error = "server.port 必须在 1-65535 之间"; return false; }
    if (value.contains("adapters") && !value["adapters"].is_array()) { error = "adapters 配置必须是数组"; return false; }
    for (const char* section : {"dice", "events", "webui", "i18n", "backup", "hot_reload"}) {
        if (value.contains(section) && !value[section].is_object()) { error = std::string(section) + " 配置必须是对象"; return false; }
    }
    if (value.contains("dice") && value["dice"].contains("scoped_overrides")
        && !value["dice"]["scoped_overrides"].is_object()) {
        error = "dice.scoped_overrides 配置必须是对象"; return false;
    }
    return true;
}

static bool writeSplitConfig(const std::string& configPath, const json& value, std::string& error) {
    const fs::path dir = configDirectory(configPath);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = ec.message(); return false; }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!isSafeSectionName(it.key())) continue;
        if (!writeJsonFile(dir / (it.key() + ".json"), it.value(), error)) return false;
    }
    // v3.0.0 briefly used this file as a split-layout manifest. The config
    // directory is now the only entry point, so remove that obsolete file.
    fs::remove(dir / "default_config.json", ec);
    return true;
}

static bool readSplitConfig(const std::string& configPath, json& value, std::string& error) {
    value = makeDefaultConfig();
    const fs::path dir = configDirectory(configPath);
    for (const char* section : kRequiredSections) {
        const std::string name = section;
        std::ifstream in(dir / (name + ".json"), std::ios::binary);
        if (!in) { error = "缺少配置文件：" + name + ".json"; return false; }
        value[name] = json::parse(in);
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) { error = ec.message(); return false; }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        const std::string name = entry.path().stem().string();
        if (!isSafeSectionName(name) || name == "webui_sessions" || name == "default_config") continue;
        bool required = false;
        for (const char* section : kRequiredSections) if (name == section) { required = true; break; }
        if (required) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) { error = "无法读取配置文件：" + entry.path().filename().string(); return false; }
        value[name] = json::parse(in);
    }
    return true;
}

// A short-lived v3 layout stored all configuration in default_config.json.
// It was never a reliable source of truth: in affected installations it can
// predate settings already persisted in the database.  Treat it as obsolete
// only when no functional split configuration is present.
static bool hasOnlyObsoleteDefaultConfig(const fs::path& dir) {
    std::error_code ec;
    const fs::path legacy = dir / "default_config.json";
    if (!fs::is_regular_file(legacy, ec) || ec) return false;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".json") continue;
        const std::string name = entry.path().stem().string();
        if (name != "default_config" && name != "webui_sessions") return false;
    }
    return true;
}

// ─── Constructor ─────────────────────────────────────────────

ConfigManager::ConfigManager(const std::string& configPath)
    : configPath_(configPath)
    , config_(makeDefaultConfig()) {
}

// ─── Lifecycle ───────────────────────────────────────────────

bool ConfigManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    createdOnLoad_ = false;
    discardedObsoleteDefaultConfig_ = false;
    try {
        if (!fs::exists(configPath_)) {
            std::string error;
            config_ = makeDefaultConfig();
            if (!writeSplitConfig(configPath_, config_, error)) {
                DICE_LOG_ERROR("ConfigManager: failed to create default configuration: {}", error);
                return false;
            }
            splitLayout_ = true;
            createdOnLoad_ = true;
            return true;
        }
        if (!fs::is_directory(configPath_)) {
            DICE_LOG_ERROR("ConfigManager: '{}' must be a configuration directory", configPath_);
            return false;
        }
        const fs::path configDir = configDirectory(configPath_);
        if (hasOnlyObsoleteDefaultConfig(configDir)) {
            std::error_code ec;
            fs::remove(configDir / "default_config.json", ec);
            if (ec) {
                DICE_LOG_WARN("ConfigManager: obsolete default_config.json was ignored but could not be removed: {}", ec.message());
            } else {
                DICE_LOG_WARN("ConfigManager: discarded obsolete default_config.json; recovering split configuration from database snapshot");
            }
            discardedObsoleteDefaultConfig_ = true;
            return false;
        }
        json loaded; std::string error;
        if (!readSplitConfig(configPath_, loaded, error) || !validateConfig(loaded, error)) {
            DICE_LOG_ERROR("ConfigManager: configuration validation failed: {}", error);
            return false;
        }
        if (loaded["server"].value("api_key", std::string()).empty()) {
            loaded["server"]["api_key"] = utils::generateUuidV4();
            DICE_LOG_INFO("ConfigManager: auto-generated API key");
        }
        config_ = std::move(loaded);
        splitLayout_ = true;
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ConfigManager: failed to load config: {}", e.what());
        return false;
    }
}

bool ConfigManager::reload() {
    if (writing_.load() > 0) { DICE_LOG_DEBUG("ConfigManager: skip reload (self-write in progress)"); return true; }
    DICE_LOG_INFO("ConfigManager: reloading from '{}'" , configPath_);

    bool ok = load();
    if (ok) {
        // The files on disk are already the source of this reload. Persist only
        // the validated snapshot; writing the same split files here would make
        // the file watcher observe our own write and enter a reload/save loop.
        json snapshot;
        std::function<void(const json&)> snapshotWriter;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = config_;
            snapshotWriter = snapshotWriter_;
        }
        if (snapshotWriter) {
            try {
                snapshotWriter(snapshot);
            } catch (const std::exception& e) {
                DICE_LOG_ERROR("ConfigManager: failed to persist database snapshot: {}", e.what());
                ok = false;
            }
        }
    }
    if (ok) {
        emitConfigChanged();
    }
    return ok;
}

bool ConfigManager::save() {
    ++writing_;  // suppress self-triggered hot reload
    bool ok = false;
    json snapshot;
    std::function<void(const json&)> snapshotWriter;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            std::string error;
            if (validateConfig(config_, error) && writeSplitConfig(configPath_, config_, error)) {
                splitLayout_ = true;
                DICE_LOG_INFO("ConfigManager: config saved to '{}'", configPath_);
                snapshot = config_;
                snapshotWriter = snapshotWriter_;
                ok = true;
            } else {
                DICE_LOG_ERROR("ConfigManager: failed to save config: {}", error);
            }
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("ConfigManager: failed to save config: {}", e.what());
        }
    }
    if (ok && snapshotWriter) {
        try { snapshotWriter(snapshot); }
        catch (const std::exception& e) { DICE_LOG_ERROR("ConfigManager: failed to persist database snapshot: {}", e.what()); }
    }
    // Delay decrement to cover the file-watcher debounce window
    std::thread([](std::atomic<int>& w) { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); --w; }, std::ref(writing_)).detach();
    return ok;
}

bool ConfigManager::restoreSnapshot(const json& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    if (!validateConfig(snapshot, error)) {
        DICE_LOG_ERROR("ConfigManager: database configuration snapshot is invalid: {}", error);
        return false;
    }
    config_ = snapshot;
    if (config_["server"].value("api_key", std::string()).empty())
        config_["server"]["api_key"] = utils::generateUuidV4();
    return true;
}

std::string ConfigManager::recoveryDatabasePath(const std::string& fallback) const {
    try {
        std::ifstream serverFile(configDirectory(configPath_) / "server.json", std::ios::binary);
        if (!serverFile) return fallback;
        const json server = json::parse(serverFile);
        if (!server.is_object() || !server.contains("db_path") || !server["db_path"].is_string()) return fallback;
        const std::string path = server["db_path"].get<std::string>();
        return path.empty() ? fallback : path;
    } catch (...) {
        return fallback;
    }
}

void ConfigManager::setSnapshotWriter(std::function<void(const json&)> writer) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshotWriter_ = std::move(writer);
}

void ConfigManager::resetDefault() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = makeDefaultConfig();
    DICE_LOG_INFO("ConfigManager: reset to defaults (not persisted)");
}

// ─── Hot-reload Callbacks ────────────────────────────────────

void ConfigManager::onConfigChanged(ConfigChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    changeCallbacks_.push_back(std::move(callback));
    DICE_LOG_DEBUG("ConfigManager: registered change callback (total: {})", changeCallbacks_.size());
}

void ConfigManager::emitConfigChanged() {
    // Copy callbacks under lock, then invoke outside lock to avoid deadlocks
    std::vector<ConfigChangeCallback> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacksCopy = changeCallbacks_;
    }

    DICE_LOG_INFO("ConfigManager: emitting config change to {} listener(s)", callbacksCopy.size());
    for (auto& cb : callbacksCopy) {
        try {
            cb();
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("ConfigManager: change callback threw: {}", e.what());
        }
    }
}

// ─── Internal: JSON Path Navigation ──────────────────────────

json* ConfigManager::navigateTo(const std::string& key, bool createMissing) {
    // Split key by "/"
    std::vector<std::string> parts;
    {
        std::istringstream iss(key);
        std::string part;
        while (std::getline(iss, part, '/')) {
            if (!part.empty()) {
                parts.push_back(part);
            }
        }
    }

    if (parts.empty()) return &config_;

    json* current = &config_;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& part = parts[i];
        if (current->is_object()) {
            if (current->contains(part)) {
                current = &(*current)[part];
            } else if (createMissing) {
                (*current)[part] = (i == parts.size() - 1) ? json() : json::object();
                current = &(*current)[part];
            } else {
                return nullptr;
            }
        } else {
            return nullptr;
        }
    }

    return current;
}

const json* ConfigManager::navigateTo(const std::string& key) const {
    std::vector<std::string> parts;
    {
        std::istringstream iss(key);
        std::string part;
        while (std::getline(iss, part, '/')) {
            if (!part.empty()) {
                parts.push_back(part);
            }
        }
    }

    if (parts.empty()) return &config_;

    const json* current = &config_;
    for (const auto& part : parts) {
        if (current->is_object() && current->contains(part)) {
            current = &(*current)[part];
        } else {
            return nullptr;
        }
    }

    return current;
}

}  // namespace dice
