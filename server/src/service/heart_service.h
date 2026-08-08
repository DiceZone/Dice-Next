#pragma once
// Dice!Next — heart.dice.zone 心跳上报客户端。
// 每个适配器持有账号中心为该骰娘签发的独立 API Key；同一进程中的多个
// 适配器分别上报，不能再用一个全局 Key 猜测整套实例代表哪只骰娘。

#include "../common/logger.h"
#include "../common/version.h"
#include "../config/config_manager.h"
#include "../adapter/adapter_interface.h"
#include "../adapter/adapter_manager.h"
#include "../adapter/kook_adapter.h"
#include "../adapter/qq_official_adapter.h"
#include "ai_gateway.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dice::heart {

using json = nlohmann::json;

inline constexpr const char* kOfficialHeartUrl = "https://heart.dice.zone";

inline std::string nowUtcIso() {
    std::time_t t = std::time(nullptr);
    std::tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return buf;
}

class HeartService {
public:
    static HeartService& instance() { static HeartService s; return s; }

    void init(ConfigManager* cfg, AdapterManager* adapters,
              std::function<long long()> commandCountProvider = {}) {
        cfg_ = cfg;
        adapters_ = adapters;
        commandCountProvider_ = std::move(commandCountProvider);
        ensureInstanceId();
        loadAutoCredentials();
    }

    bool enabled() const { return cfg_ && cfg_->get<bool>("dice/heart_enabled", false); }
    std::string url() const {
        std::string u = cfg_ ? cfg_->get<std::string>("dice/heart_url", std::string(kOfficialHeartUrl))
                             : std::string(kOfficialHeartUrl);
        if (u.empty()) u = kOfficialHeartUrl;
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
    bool publicShow() const { return cfg_ ? cfg_->get<bool>("dice/heart_public_show", true) : true; }
    int interval() const {
        int v = cfg_ ? cfg_->get<int>("dice/heart_interval", 240) : 240;
        if (v < 180) v = 180;
        if (v > 480) v = 480;
        return v;
    }

    void tick() {
        if (!cfg_ || !adapters_) return;
        auto autoWork = pendingAutoReports(false);
        auto work = enabled() ? pendingReports(false) : std::vector<WorkItem>{};
        if ((work.empty() && autoWork.empty()) || busy_.exchange(true)) return;
        std::thread([this, work = std::move(work), autoWork = std::move(autoWork)]() {
            for (const auto& item : autoWork) doAutoReport(item.target.adapter, item.status, false, 15);
            for (const auto& item : work) doReport(item.target, item.status, false, 15);
            busy_.store(false);
        }).detach();
    }

    /// WebUI 立即测试所有已配置 Key 的适配器。
    std::pair<int, std::string> testReport() {
        auto targets = reportTargets();
        if (targets.empty()) {
            return {0, "没有适配器配置骰娘 API Key，请在适配器页面填写"};
        }
        json results = json::array();
        int firstError = 0;
        for (const auto& target : targets) {
            const std::string status = target.adapter->isConnected() ? "online" : "offline";
            auto result = doReport(target, status, true, 15);
            results.push_back({
                {"adapter_id", target.adapter->id()},
                {"adapter_name", target.adapter->name()},
                {"http", result.first},
                {"body", result.second},
            });
            if (result.first != 200 && firstError == 0) firstError = result.first == 0 ? 500 : result.first;
        }
        return {firstError == 0 ? 200 : firstError, json{{"results", results}}.dump()};
    }

    void shutdownReport() {
        if (!cfg_ || !adapters_) return;
        if (enabled()) for (const auto& target : reportTargets()) {
            bool wasOnline = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = states_.find(target.adapter->id());
                wasOnline = it != states_.end() && it->second.lastStatus == "online";
            }
            if (wasOnline) doReport(target, "offline", true, 5);
        }
        for (const auto& adapter : adapters_->allAdapters()) {
            if (!adapter) continue;
            bool wasOnline = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = autoStates_.find(adapter->id());
                wasOnline = it != autoStates_.end() && it->second.lastStatus == "online";
            }
            if (wasOnline) doAutoReport(adapter, "offline", true, 5);
        }
    }

    json lastState() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string status = "unknown";
        std::string latestAt;
        std::vector<std::string> errors;
        bool anyOnline = false;
        bool anyOffline = false;
        for (const auto& [adapterId, state] : states_) {
            anyOnline = anyOnline || state.lastStatus == "online";
            anyOffline = anyOffline || state.lastStatus == "offline";
            if (state.lastReportIso > latestAt) latestAt = state.lastReportIso;
            if (!state.lastError.empty()) errors.push_back(adapterId + ": " + state.lastError);
        }
        if (anyOnline) status = "online";
        else if (anyOffline) status = "offline";
        std::string error;
        for (const auto& item : errors) {
            if (!error.empty()) error += "\n";
            error += item;
        }
        return {
            {"last_status", status},
            {"last_report_at", latestAt},
            {"last_error", error},
            {"reported_adapters", states_.size()},
        };
    }

    std::size_t configuredAdapterCount() const {
        return configuredKeys().size();
    }

private:
    HeartService() = default;

    struct Target {
        AdapterPtr adapter;
        std::string apiKey;
    };

    struct WorkItem {
        Target target;
        std::string status;
    };

    struct TargetState {
        std::string lastStatus = "unknown";
        long long lastReportAt = 0;
        std::string lastReportIso;
        long long lastSwitchAt = 0;
        long long penaltyUntil = 0;
        bool permanentlyBlocked = false;
        std::string lastLoginIso;
        std::string lastError;
        bool warned401 = false;
    };

    struct AutoCredential {
        std::string registrationSecret;
        std::string heartbeatKey;
        json endpoint = json::object();
    };

    static long long epoch() {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    std::string instanceId() const {
        return cfg_ ? cfg_->get<std::string>("dice/heart_instance_id", std::string()) : std::string();
    }

    std::string adapterInstanceId(const std::string& adapterId) const {
        std::string suffix = "-adapter-" + adapterId;
        if (suffix.size() >= 64) {
            // Keep identifiers deterministic across restarts without trusting platform
            // adapter IDs to fit the BDC column length.
            unsigned long long hash = 1469598103934665603ULL;
            for (const unsigned char ch : adapterId) {
                hash ^= ch;
                hash *= 1099511628211ULL;
            }
            std::ostringstream encoded;
            encoded << "-adapter-" << std::hex << std::setw(16) << std::setfill('0') << hash;
            suffix = encoded.str();
        }
        std::string base = instanceId();
        if (base.size() + suffix.size() > 64) base.resize(64 - suffix.size());
        return base + suffix;
    }

    void ensureInstanceId() {
        if (!cfg_ || !instanceId().empty()) return;
        std::random_device rd;
        std::ostringstream out;
        out << "dn-";
        for (int i = 0; i < 16; ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << (rd() & 0xff);
        try {
            cfg_->set<std::string>("dice/heart_instance_id", out.str());
            cfg_->save();
        } catch (...) {}
    }

    static std::string randomHex(std::size_t bytes) {
        std::random_device rd;
        std::ostringstream out;
        for (std::size_t i = 0; i < bytes; ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << (rd() & 0xff);
        return out.str();
    }

    void loadAutoCredentials() {
        if (!cfg_) return;
        try {
            json stored = cfg_->get<json>("dice/bdc_auto_credentials", json::object());
            if (!stored.is_object()) return;
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [adapterId, value] : stored.items()) {
                if (!value.is_object()) continue;
                autoCredentials_[adapterId] = {
                    value.value("registration_secret", std::string()),
                    value.value("heartbeat_key", std::string()),
                    value.value("endpoint", json::object()),
                };
            }
        } catch (...) {}
    }

    void saveAutoCredentials() {
        if (!cfg_) return;
        json stored = json::object();
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [adapterId, credential] : autoCredentials_)
                stored[adapterId] = {
                    {"registration_secret", credential.registrationSecret},
                    {"heartbeat_key", credential.heartbeatKey},
                    {"endpoint", credential.endpoint},
                };
        }
        try {
            cfg_->set<json>("dice/bdc_auto_credentials", stored);
            cfg_->save();
        } catch (...) {}
    }

    std::unordered_map<std::string, std::string> configuredKeys() const {
        std::unordered_map<std::string, std::string> result;
        if (!cfg_) return result;
        try {
            json all = cfg_->getAll();
            if (!all.contains("adapters") || !all["adapters"].is_array()) return result;
            for (const auto& item : all["adapters"]) {
                if (!item.is_object()) continue;
                std::string id;
                if (item.contains("id") && item["id"].is_string()) id = item["id"].get<std::string>();
                else if (item.contains("id") && item["id"].is_number_integer()) id = std::to_string(item["id"].get<int>());
                const std::string key = item.value("heart_api_key", item.value("heartApiKey", std::string()));
                if (!id.empty() && !key.empty()) result[id] = key;
            }
        } catch (...) {}
        return result;
    }

    std::vector<Target> reportTargets() const {
        std::vector<Target> result;
        if (!adapters_) return result;
        const auto keys = configuredKeys();
        for (const auto& adapter : adapters_->allAdapters()) {
            if (!adapter) continue;
            auto it = keys.find(adapter->id());
            if (it != keys.end()) result.push_back({adapter, it->second});
        }
        return result;
    }


    std::vector<WorkItem> pendingAutoReports(bool bypassThrottle) {
        std::vector<WorkItem> result;
        const long long now = epoch();
        const auto verifiedKeys = configuredKeys();
        for (const auto& adapter : adapters_->allAdapters()) {
            if (!adapter) continue;
            bool shouldReport = bypassThrottle;
            std::string desired;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto& state = autoStates_[adapter->id()];
                if (verifiedKeys.count(adapter->id())) {
                    // A formally verified API key takes over this adapter. Close the
                    // isolated record once, then stop all automatic reporting for it.
                    if (state.lastStatus != "online") continue;
                    desired = "offline";
                    shouldReport = true;
                } else {
                    // Do not create a record until this adapter has logged in once.
                    if (adapter->getLoginId().empty() && state.lastStatus == "unknown") continue;
                    desired = adapter->isConnected() ? "online" : "offline";
                    if (desired == "offline" && state.lastStatus == "unknown") continue;
                    if (!bypassThrottle)
                        shouldReport = state.lastStatus != desired ||
                            (desired == "online" && now - state.lastReportAt >= interval());
                }
            }
            if (shouldReport) result.push_back({Target{adapter, ""}, desired});
        }
        return result;
    }

    std::vector<WorkItem> pendingReports(bool bypassThrottle) {
        std::vector<WorkItem> result;
        const long long now = epoch();
        for (const auto& target : reportTargets()) {
            const std::string id = target.adapter->id();
            const std::string desired = target.adapter->isConnected() ? "online" : "offline";
            bool shouldReport = bypassThrottle;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto& state = states_[id];
                if (state.permanentlyBlocked || now < state.penaltyUntil) continue;
                if (!bypassThrottle) {
                    if (state.lastStatus != "online" && desired == "offline") continue;
                    const bool switching = desired != state.lastStatus;
                    shouldReport = switching ? (now - state.lastSwitchAt >= 60)
                                             : (now - state.lastReportAt >= interval());
                }
            }
            if (shouldReport) result.push_back({target, desired});
        }
        return result;
    }

    json endpointFor(const AdapterPtr& adapter, std::string& displayIdentity) const {
        const std::string platform = adapter->platform();
        const std::string nativeId = adapter->getLoginId();
        std::string accountId = nativeId;
        std::string displayId;
        std::string shareUrl;
        if (platform == "onebot_v11") {
            displayId = nativeId;
        } else if (platform == "qq_official") {
            if (auto official = std::dynamic_pointer_cast<QQOfficialAdapter>(adapter)) {
                accountId = official->appId();
                displayId = official->displayQQ();
                shareUrl = official->shareUrl();
            }
        } else if (platform == "discord") {
            // Discord 官方的 client_id-only 安装链接使用开发者后台的默认安装设置。
            shareUrl = "https://discord.com/oauth2/authorize?client_id=" + nativeId;
        } else if (platform == "kook") {
            if (auto kook = std::dynamic_pointer_cast<KookAdapter>(adapter)) {
                const std::string clientId = kook->clientId();
                shareUrl = clientId.empty()
                    ? "https://www.kookapp.cn/app/bot/" + nativeId
                    : "https://www.kookapp.cn/app/oauth2/authorize?client_id=" + clientId
                        + "&id=" + nativeId + "&scope=bot";
            }
        }
        displayIdentity = !displayId.empty() ? displayId : accountId;
        if (accountId.empty()) return json();
        return {
            {"platform", platform},
            {"account_id", accountId},
            {"native_id", nativeId},
            {"display_id", displayId},
            {"share_url", shareUrl},
            {"nickname", adapter->getLoginName()},
            {"protocol", adapter->version()},
            {"connected", adapter->isConnected()},
            {"adapter_id", adapter->id()},
        };
    }

    std::string firstMaster() const {
        if (!cfg_) return "";
        try {
            json all = cfg_->getAll();
            if (all.contains("dice") && all["dice"].contains("masters") && all["dice"]["masters"].is_array()) {
                for (const auto& m : all["dice"]["masters"]) {
                    std::string platform, id;
                    if (m.is_string()) { id = m.get<std::string>(); platform.clear(); }
                    else if (m.is_object()) {
                        platform = m.value("platform", std::string());
                        id = m.value("id", std::string());
                    }
                    if (id.empty()) continue;
                    // 心跳展示只认 OneBot（真实 QQ 号）骰主：QQ 官方 OpenID、
                    // Discord/KOOK ID 对展示无意义，且 OpenID 会超服务端 master_id
                    // 列长导致上报 500。空平台视为旧版 QQ 号条目，兼容老数据。
                    if (platform.empty() || platform == "onebot_v11") return id;
                }
            }
        } catch (...) {}
        return "";
    }

    std::pair<int, std::string> doReport(
        const Target& target,
        const std::string& status,
        bool bypassThrottle,
        int timeoutSec
    ) {
        const std::string adapterId = target.adapter->id();
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& state = states_[adapterId];
            if (state.permanentlyBlocked) return {0, "该适配器已被服务端永久封禁（403）"};
            if (!bypassThrottle && epoch() < state.penaltyUntil) return {0, "该适配器处于切换惩罚期"};
        }

        std::string selfId;
        json endpoint = endpointFor(target.adapter, selfId);
        if (endpoint.is_null() || endpoint.empty()) return {0, "适配器尚未取得平台账号 ID"};

        std::string bn = std::to_string(buildNumber());
        while (bn.size() < 3) bn = "0" + bn;
        json body{
            {"access_token", target.apiKey},
            {"instance_id", adapterInstanceId(adapterId)},
            {"adapters", json::array({endpoint})},
            {"status", status},
            {"dice_info", {
                {"dice_id", selfId},
                {"dice_nickname", target.adapter->getLoginName()},
                {"master_id", firstMaster()},
                {"master_nickname", ""},
            }},
            {"dice_type", "dicenext"},
            {"dice_version", versionString() + "(" + bn + ")"},
            {"frame", target.adapter->version()},
            {"plugin_version", versionString()},
            {"command_count", commandCountProvider_ ? commandCountProvider_() : 0},
            {"public_show", publicShow()},
        };
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& state = states_[adapterId];
            if (status == "online" && state.lastLoginIso.empty()) state.lastLoginIso = nowUtcIso();
            if (!state.lastLoginIso.empty()) body["login_time"] = state.lastLoginIso;
            if (status == "offline") body["offline_time"] = nowUtcIso();
        }

        int httpStatus = 0;
        const std::string response = dice::ai::httpPostJson(
            url() + "/api/heart", "", body.dump(), timeoutSec, httpStatus);
        handleResponse(adapterId, target.adapter->name(), status, httpStatus, response);
        return {httpStatus, response};
    }

    std::pair<int, std::string> doAutoReport(
        const AdapterPtr& adapter,
        const std::string& status,
        bool bypassThrottle,
        int timeoutSec
    ) {
        if (!adapter) return {0, "适配器不存在"};
        const std::string adapterId = adapter->id();
        std::string identity;
        json endpoint = endpointFor(adapter, identity);

        AutoCredential credential;
        bool credentialsChanged = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& saved = autoCredentials_[adapterId];
            if (saved.registrationSecret.empty()) {
                saved.registrationSecret = randomHex(32);
                credentialsChanged = true;
            }
            if (!endpoint.is_null() && !endpoint.empty() && saved.endpoint != endpoint) {
                saved.endpoint = endpoint;
                credentialsChanged = true;
            }
            credential = saved;
        }
        if (endpoint.is_null() || endpoint.empty()) endpoint = credential.endpoint;
        if (endpoint.is_null() || endpoint.empty()) return {0, "适配器尚未取得平台账号 ID"};
        endpoint["connected"] = status == "online";
        // Persist the continuity secret before registration. If the server accepts the
        // request but the response is lost, a restart can still reclaim the same record.
        if (credentialsChanged) saveAutoCredentials();
        if (credential.heartbeatKey.empty()) {
            std::string bn = std::to_string(buildNumber());
            while (bn.size() < 3) bn = "0" + bn;
            json registration{
                {"client_adapter_id", adapterInstanceId(adapterId)},
                {"registration_secret", credential.registrationSecret},
                {"adapter_name", adapter->name()}, {"endpoint", endpoint},
                {"dice_type", "dicenext"}, {"dice_version", versionString() + "(" + bn + ")"},
                {"plugin_version", versionString()}, {"frame", adapter->version()},
            };
            int registerStatus = 0;
            const std::string registerResponse = dice::ai::httpPostJson(
                std::string(kOfficialHeartUrl) + "/api/unverified/register", "",
                registration.dump(), timeoutSec, registerStatus);
            if (registerStatus != 200) {
                if (!bypassThrottle)
                    DICE_LOG_WARN("[BDC 自动发现:{}] 注册失败：HTTP {}（{}）", adapter->name(), registerStatus, registerResponse);
                return {registerStatus, registerResponse};
            }
            try {
                const json parsed = json::parse(registerResponse);
                credential.heartbeatKey = parsed.value("heartbeat_key", std::string());
            } catch (...) {}
            if (credential.heartbeatKey.empty()) return {0, "BDC 注册响应缺少 heartbeat_key"};
            {
                std::lock_guard<std::mutex> lock(mu_);
                autoCredentials_[adapterId] = credential;
            }
            saveAutoCredentials();
            DICE_LOG_INFO("[BDC 自动发现:{}] 已取得独立未验证心跳凭据", adapter->name());
        }

        std::string bn = std::to_string(buildNumber());
        while (bn.size() < 3) bn = "0" + bn;
        json body{
            {"heartbeat_key", credential.heartbeatKey}, {"status", status},
            {"endpoint", endpoint}, {"adapter_name", adapter->name()},
            {"dice_type", "dicenext"}, {"dice_version", versionString() + "(" + bn + ")"},
            {"plugin_version", versionString()}, {"frame", adapter->version()},
            {"command_count", commandCountProvider_ ? commandCountProvider_() : 0},
        };
        int httpStatus = 0;
        const std::string response = dice::ai::httpPostJson(
            std::string(kOfficialHeartUrl) + "/api/unverified/heart", "",
            body.dump(), timeoutSec, httpStatus);
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& state = autoStates_[adapterId];
            if (httpStatus == 200) {
                state.lastStatus = status;
                state.lastReportAt = epoch();
                state.lastReportIso = nowUtcIso();
                state.lastError.clear();
            } else {
                state.lastError = httpStatus == 0 ? "网络请求失败" : "HTTP " + std::to_string(httpStatus);
                if (httpStatus == 401) autoCredentials_[adapterId].heartbeatKey.clear();
            }
        }
        if (httpStatus == 401) saveAutoCredentials();
        else if (httpStatus != 200 && !bypassThrottle)
            DICE_LOG_WARN("[BDC 自动发现:{}] 心跳失败：HTTP {}（{}）", adapter->name(), httpStatus, response);
        return {httpStatus, response};
    }

    void handleResponse(
        const std::string& adapterId,
        const std::string& adapterName,
        const std::string& reported,
        int httpStatus,
        const std::string& response
    ) {
        const long long now = epoch();
        std::lock_guard<std::mutex> lock(mu_);
        auto& state = states_[adapterId];
        if (httpStatus == 200) {
            if (state.lastStatus != reported) state.lastSwitchAt = now;
            state.lastStatus = reported;
            state.lastReportAt = now;
            state.lastReportIso = nowUtcIso();
            state.lastError.clear();
            state.warned401 = false;
            if (reported == "offline") state.lastLoginIso.clear();
        } else if (httpStatus == 401) {
            state.lastError = response.find("不匹配") != std::string::npos
                ? "API Key 不属于该适配器当前登录的骰娘账号"
                : "骰娘 API Key 无效，请到账号中心重新生成";
            if (!state.warned401) {
                state.warned401 = true;
                DICE_LOG_WARN("[心跳:{}] {}", adapterName, state.lastError);
            }
        } else if (httpStatus == 403) {
            state.permanentlyBlocked = true;
            state.lastError = "本机 IP 已被心跳服务端永久封禁（403）";
            DICE_LOG_WARN("[心跳:{}] {}", adapterName, state.lastError);
        } else if (httpStatus == 429) {
            const long long seconds = parsePenaltySeconds(response);
            state.penaltyUntil = now + seconds;
            state.lastError = "触发切换惩罚，暂停上报 " + std::to_string(seconds) + " 秒";
            DICE_LOG_WARN("[心跳:{}] {}", adapterName, state.lastError);
        } else {
            state.lastError = httpStatus == 0 ? "网络请求失败（curl 无响应）"
                                              : ("HTTP " + std::to_string(httpStatus));
            DICE_LOG_WARN("[心跳:{}] 上报失败：{}（服务端响应：{}）", adapterName, state.lastError, response);
        }
    }

    static long long parsePenaltySeconds(const std::string& response) {
        auto pick = [](const json& object) -> long long {
            for (const char* key : {"retry_after", "penalty", "penalty_seconds", "seconds"})
                if (object.contains(key) && object[key].is_number())
                    return (std::max)(static_cast<long long>(object[key].get<double>()), 1LL);
            return 0;
        };
        try {
            json value = json::parse(response);
            long long seconds = pick(value);
            if (seconds) return seconds;
            if (value.contains("detail") && value["detail"].is_object()) {
                seconds = pick(value["detail"]);
                if (seconds) return seconds;
            }
        } catch (...) {}
        return 600;
    }

    ConfigManager* cfg_ = nullptr;
    AdapterManager* adapters_ = nullptr;
    std::function<long long()> commandCountProvider_;
    mutable std::mutex mu_;
    std::atomic<bool> busy_{false};
    std::unordered_map<std::string, TargetState> states_;
    std::unordered_map<std::string, TargetState> autoStates_;
    std::unordered_map<std::string, AutoCredential> autoCredentials_;
};

}  // namespace dice::heart
