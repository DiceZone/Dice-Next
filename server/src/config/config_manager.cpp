#include "config_manager.h"
#include "../common/logger.h"
#include "../common/utils.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>

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
            {"blacklist_quit_level", "member"},  // 黑名单退群默认等级: member=任一成员触发 / admin=仅群主级触发
            {"respond_self", false},             // 自响应：用骰娘账号自身消息自控（默认关）
            {"console_start_hidden", true},      // 启动即最小化到托盘（隐藏控制台，退出走托盘）
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

// ─── Constructor ─────────────────────────────────────────────

ConfigManager::ConfigManager(const std::string& configPath)
    : configPath_(configPath)
    , config_(makeDefaultConfig()) {
}

// ─── Lifecycle ───────────────────────────────────────────────

bool ConfigManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fs::exists(configPath_)) {
        DICE_LOG_WARN("ConfigManager: config file not found at '{}', using defaults", configPath_);
        // Write defaults to disk
        try {
            fs::create_directories(fs::path(configPath_).parent_path());
            std::ofstream ofs(configPath_);
            ofs << config_.dump(2);
            DICE_LOG_INFO("ConfigManager: default config written to '{}'", configPath_);
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("ConfigManager: failed to write default config: {}", e.what());
            return false;
        }
        return true;
    }

    try {
        std::ifstream ifs(configPath_);
        if (!ifs.is_open()) {
            DICE_LOG_ERROR("ConfigManager: failed to open '{}'", configPath_);
            return false;
        }

        json loaded = json::parse(ifs);

        // Merge with defaults to ensure all keys exist
        // nlohmann::json::update() recursively merges loaded over defaults
        config_ = makeDefaultConfig();
        config_.update(loaded);

        // Auto-generate API key if empty
        if (config_["server"]["api_key"].get<std::string>().empty()) {
            config_["server"]["api_key"] = utils::generateUuidV4();
            DICE_LOG_INFO("ConfigManager: auto-generated API key");
        }

        DICE_LOG_INFO("ConfigManager: config loaded from '{}'", configPath_);
        return true;
    } catch (const json::parse_error& e) {
        DICE_LOG_ERROR("ConfigManager: JSON parse error in '{}': {}", configPath_, e.what());
        // Fall back to defaults
        config_ = makeDefaultConfig();
        return false;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("ConfigManager: failed to load config: {}", e.what());
        config_ = makeDefaultConfig();
        return false;
    }
}

bool ConfigManager::reload() {
    if (writing_.load() > 0) { DICE_LOG_DEBUG("ConfigManager: skip reload (self-write in progress)"); return true; }
    DICE_LOG_INFO("ConfigManager: reloading from '{}'" , configPath_);

    bool ok = load();
    if (ok) {
        emitConfigChanged();
    }
    return ok;
}

bool ConfigManager::save() {
    ++writing_;  // suppress self-triggered hot reload
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            fs::create_directories(fs::path(configPath_).parent_path());
            std::ofstream ofs(configPath_);
            if (!ofs.is_open()) {
                DICE_LOG_ERROR("ConfigManager: failed to open '{}' for writing", configPath_);
            } else {
                ofs << config_.dump(2);
                DICE_LOG_INFO("ConfigManager: config saved to '{}'", configPath_);
                ok = true;
            }
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("ConfigManager: failed to save config: {}", e.what());
        }
    }
    // Delay decrement to cover the file-watcher debounce window
    std::thread([](std::atomic<int>& w) { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); --w; }, std::ref(writing_)).detach();
    return ok;
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
