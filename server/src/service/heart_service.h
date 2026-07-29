#pragma once
// ─── Dice!Next — heart.dice.zone 心跳上报客户端 ────────────────
// 协议见 Better-Dice-Control/docs/HEART_API.md：
//   POST {url}/api/heart，token 放在 JSON body（access_token），非 HTTP 头。
//   频控：同状态 180s 内重报被忽略（200 rate_limited，不算错误）；
//   惩罚：60s 内切换 ≥3 次 → 429（响应含惩罚秒数）；24h 被罚 ≥3 次 → 该 IP 永久 403。
// 客户端策略：
//   · main.cpp runEvery(60s) 驱动 tick()，内部按 dice/heart_interval 自行节流；
//   · 状态切换立即报，但加 60s 最小切换间隔防抖（网络抖动时单向收敛，避免触发惩罚）；
//   · 从未上过线（本进程内没报过 online）就不报 offline——服务端超 600s 无心跳
//     会自动判离线，冷启动时报 offline 毫无意义还可能凑出切换惩罚；
//   · 一切出站请求走 curl 分离线程（复用 ai_gateway 的配置文件防注入模式），
//     绝不阻塞 Drogon 事件循环；仅退出时的 shutdownReport() 同步发送（max-time 5s）。

#include "../common/logger.h"
#include "../common/version.h"
#include "../config/config_manager.h"
#include "../adapter/adapter_interface.h"
#include "../adapter/adapter_manager.h"
#include "../adapter/qq_official_adapter.h"
#include "ai_gateway.h"   // dice::ai::httpPostJson —— curl 配置文件出站模板

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>
#include <string>
#include <thread>
#include <utility>

namespace dice::heart {

using json = nlohmann::json;

inline constexpr const char* kOfficialHeartUrl = "https://heart.dice.zone";

/// 当前 UTC 时间 → "YYYY-MM-DDTHH:MM:SSZ"（协议示例的秒级 ISO8601）。
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

    void init(ConfigManager* cfg, AdapterManager* adapters) {
        cfg_ = cfg;
        adapters_ = adapters;
        ensureInstanceId();
    }

    // ── 配置读取（每次现读 → 热更即生效）────────────────────
    bool enabled() const { return cfg_ && cfg_->get<bool>("dice/heart_enabled", false); }
    std::string url() const {
        std::string u = cfg_ ? cfg_->get<std::string>("dice/heart_url", std::string(kOfficialHeartUrl))
                             : std::string(kOfficialHeartUrl);
        if (u.empty()) u = kOfficialHeartUrl;
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
    std::string token() const { return cfg_ ? cfg_->get<std::string>("dice/heart_token", std::string()) : std::string(); }
    bool publicShow() const { return cfg_ ? cfg_->get<bool>("dice/heart_public_show", true) : true; }
    int interval() const {
        int v = cfg_ ? cfg_->get<int>("dice/heart_interval", 240) : 240;
        if (v < 180) v = 180;
        if (v > 480) v = 480;   // 服务端 600s 无心跳判离线，上限留巡检余量防在线状态抖动
        return v;
    }

    /// 周期驱动（main.cpp runEvery 60s / runAfter 首跳）。内部自行节流，直接返回极快。
    void tick() {
        if (!cfg_ || !adapters_) return;
        std::string tk = token();
        if (!enabled() || tk.empty()) return;
        long long now = epoch();
        std::string desired = anyConnected() ? "online" : "offline";
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (permanentBlocked_) return;
            if (now < penaltyUntil_) return;
            // 本进程内从未报过 online → 不报 offline（服务端会自动判离线）。
            if (lastStatus_ != "online" && desired == "offline") return;
            bool switching = (desired != lastStatus_);
            if (switching) {
                if (now - lastSwitchAt_ < 60) return;   // 切换防抖：最小 60s
            } else {
                if (now - lastReportAt_ < interval()) return;   // 同状态按 interval 保活
            }
        }
        if (busy_.exchange(true)) return;   // 上一次请求还在路上 → 跳过本跳
        std::thread([this, desired]() {
            doReport(desired, false, 15);
            busy_.store(false);
        }).detach();
    }

    /// WebUI「测试」：立即同步发一次当前状态心跳（在调用方的分离线程里跑）。
    /// 返回 {HTTP 状态码, 响应体}（0 = curl 失败）。
    std::pair<int, std::string> testReport() {
        std::string desired = anyConnected() ? "online" : "offline";
        // 从未上线过就别报 offline：否则服务端会用遗留 login_time 反复重算并虚增在线时长/会话数
        if (desired == "offline") {
            std::lock_guard<std::mutex> lk(mu_);
            if (lastStatus_ != "online")
                return {200, std::string("{\"status\":\"skipped\",\"reason\":\"never online\"}")};
        }
        return doReport(desired, true, 15);
    }

    /// 进程正常退出时同步报一次 offline（max-time 5s）。app.run() 之后调用。
    void shutdownReport() {
        if (!cfg_ || !adapters_) return;
        if (!enabled() || token().empty()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (permanentBlocked_) return;
            if (lastStatus_ != "online") return;   // 没上过线就没有可结算的时长
        }
        doReport("offline", true, 5);
    }

    /// 供 /api/system/heartbeat 展示的最近状态。
    json lastState() const {
        std::lock_guard<std::mutex> lk(mu_);
        return json{
            {"last_status", lastStatus_},
            {"last_report_at", lastReportIso_},
            {"last_error", lastError_},
        };
    }

private:
    HeartService() = default;

    static long long epoch() {
        return (long long)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool anyConnected() const {
        if (!adapters_) return false;
        for (auto& a : adapters_->allAdapters())
            if (a && a->isConnected()) return true;
        return false;
    }

    std::string instanceId() const {
        return cfg_ ? cfg_->get<std::string>("dice/heart_instance_id", std::string()) : std::string();
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

    json adapterEndpoints(std::string& preferredId, std::string& preferredNick,
                          std::string& aggregateFrame) const {
        json result = json::array();
        if (!adapters_) return result;
        bool choseOneBot = false;
        for (const auto& adapter : adapters_->allAdapters()) {
            if (!adapter) continue;
            const std::string platform = adapter->platform();
            const std::string nativeId = adapter->getLoginId();
            std::string accountId = nativeId;
            std::string displayId;
            if (platform == "onebot_v11") {
                displayId = nativeId;
            } else if (platform == "qq_official") {
                if (auto official = std::dynamic_pointer_cast<QQOfficialAdapter>(adapter)) {
                    accountId = official->appId();
                    displayId = official->displayQQ();
                }
            }
            // 未连接的适配器可能还拿不到平台账号；QQ 官方使用 AppID，
            // 其余平台等首次连上取得原生 Bot ID 后再纳入心跳端点。
            if (accountId.empty()) continue;
            result.push_back(json{
                {"platform", platform},
                {"account_id", accountId},
                {"native_id", nativeId},
                {"display_id", displayId},
                {"nickname", adapter->getLoginName()},
                {"protocol", adapter->version()},
                {"connected", adapter->isConnected()},
                {"adapter_id", adapter->id()},
            });
            if (adapter->isConnected() && (!choseOneBot || platform == "onebot_v11")) {
                preferredId = !displayId.empty() ? displayId : accountId;
                preferredNick = adapter->getLoginName();
                choseOneBot = platform == "onebot_v11";
            }
        }
        if (result.size() == 1) aggregateFrame = result.front().value("protocol", std::string());
        else if (!result.empty()) aggregateFrame = "multi-adapter";
        return result;
    }

    /// 骰主 QQ：config dice/masters 第一条（兼容裸字符串与 {platform,id} 两种形态）。
    std::string firstMaster() const {
        if (!cfg_) return "";
        try {
            json all = cfg_->getAll();
            if (all.contains("dice") && all["dice"].contains("masters") && all["dice"]["masters"].is_array()) {
                for (const auto& m : all["dice"]["masters"]) {
                    if (m.is_string() && !m.get<std::string>().empty()) return m.get<std::string>();
                    if (m.is_object()) {
                        std::string id = m.value("id", std::string());
                        if (!id.empty()) return id;
                    }
                }
            }
        } catch (...) {}
        return "";
    }

    /// 组包并发送一次心跳；返回 {HTTP 状态码, 响应体}。响应处理更新内部状态机。
    /// @p bypassThrottle 为 true 时（测试/退出）不做节流判断，但仍遵守永久封禁。
    std::pair<int, std::string> doReport(const std::string& status, bool bypassThrottle, int timeoutSec) {
        std::string tk = token();
        if (tk.empty()) return {0, "token 未配置"};
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (permanentBlocked_) return {0, "已被服务端永久封禁（403），本次运行不再上报"};
            if (!bypassThrottle && epoch() < penaltyUntil_) return {0, "处于切换惩罚期"};
        }

        std::string selfId, selfNick, frame;
        json endpoints = adapterEndpoints(selfId, selfNick, frame);
        if (selfId.empty() && cfg_) selfId = cfg_->get<std::string>("dice/self_qq", std::string());
        std::string bn = std::to_string(buildNumber());
        while (bn.size() < 3) bn = "0" + bn;

        json body{
            {"access_token", tk},
            {"instance_id", instanceId()},
            {"adapters", std::move(endpoints)},
            {"status", status},
            {"dice_info", json{
                {"dice_id", selfId},
                {"dice_nickname", selfNick},
                {"master_id", firstMaster()},
                {"master_nickname", ""},
            }},
            {"dice_type", "dicenext"},
            {"dice_version", versionString() + "(" + bn + ")"},
            {"frame", frame.empty() ? std::string("dicenext") : frame},
            {"plugin_version", versionString()},
            {"public_show", publicShow()},
        };
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (status == "online" && lastLoginIso_.empty()) lastLoginIso_ = nowUtcIso();
            if (!lastLoginIso_.empty()) body["login_time"] = lastLoginIso_;
            if (status == "offline") body["offline_time"] = nowUtcIso();
        }

        int httpStatus = 0;
        std::string resp = dice::ai::httpPostJson(url() + "/api/heart", "", body.dump(), timeoutSec, httpStatus);
        handleResponse(status, httpStatus, resp);
        return {httpStatus, resp};
    }

    void handleResponse(const std::string& reported, int httpStatus, const std::string& resp) {
        long long now = epoch();
        std::lock_guard<std::mutex> lk(mu_);
        if (httpStatus == 200) {
            // ok 与 rate_limited（同状态 180s 内重报被忽略）都算正常。
            if (lastStatus_ != reported) lastSwitchAt_ = now;
            lastStatus_ = reported;
            lastReportAt_ = now;
            lastReportIso_ = nowUtcIso();
            lastError_.clear();
            warned401_ = false;
            if (reported == "offline") lastLoginIso_.clear();   // 下次上线取新的 login_time
        } else if (httpStatus == 401) {
            lastError_ = "access_token 无效（401），请到 heart.dice.zone 重新绑定获取";
            if (!warned401_) {   // 同因仅告警一次，避免每周期刷屏
                warned401_ = true;
                DICE_LOG_WARN("[心跳] {}", lastError_);
            }
        } else if (httpStatus == 403) {
            permanentBlocked_ = true;
            lastError_ = "本机 IP 已被心跳服务端永久封禁（403），本次运行不再上报";
            DICE_LOG_WARN("[心跳] {}", lastError_);
        } else if (httpStatus == 429) {
            long long sec = parsePenaltySeconds(resp);
            penaltyUntil_ = now + sec;
            lastError_ = "触发切换惩罚（429），暂停上报 " + std::to_string(sec) + " 秒";
            DICE_LOG_WARN("[心跳] {}", lastError_);
        } else {
            // 网络失败/其它错误 → 静默，下周期重试（只记状态供 WebUI 展示）。
            lastError_ = httpStatus == 0 ? "网络请求失败（curl 无响应）"
                                         : ("HTTP " + std::to_string(httpStatus));
            DICE_LOG_DEBUG("[心跳] 上报失败：{}（下周期重试）", lastError_);
        }
    }

    /// 从 429 响应体里解析惩罚秒数（retry_after / penalty / seconds），拿不到默认 600。
    /// FastAPI 会把 HTTPException 的 detail 包一层，故顶层与 detail 子对象都查。
    static long long parsePenaltySeconds(const std::string& resp) {
        auto pick = [](const json& o) -> long long {
            for (const char* k : {"retry_after", "penalty", "penalty_seconds", "seconds"})
                if (o.contains(k) && o[k].is_number()) return (std::max)((long long)o[k].get<double>(), 1LL);
            return 0;
        };
        try {
            json j = json::parse(resp);
            long long v = pick(j);
            if (v) return v;
            if (j.contains("detail") && j["detail"].is_object()) {
                v = pick(j["detail"]);
                if (v) return v;
            }
        } catch (...) {}
        return 600;
    }

    ConfigManager* cfg_ = nullptr;
    AdapterManager* adapters_ = nullptr;

    mutable std::mutex mu_;
    std::atomic<bool> busy_{false};        // 出站请求进行中（防止分离线程堆积）
    std::string lastStatus_ = "unknown";   // online / offline / unknown
    long long lastReportAt_ = 0;           // 上次成功上报（epoch 秒）
    std::string lastReportIso_;            // 同上，ISO 展示用
    long long lastSwitchAt_ = 0;           // 上次状态切换成功上报时刻（防抖基准）
    long long penaltyUntil_ = 0;           // 429 惩罚期截止（epoch 秒）
    bool permanentBlocked_ = false;        // 403：本进程内不再上报
    std::string lastLoginIso_;             // 本次在线周期的 login_time
    std::string lastError_;
    bool warned401_ = false;
};

}  // namespace dice::heart
