#pragma once
// ─── Dice!Next — cloudban.dice.zone 云黑名单同步客户端 ─────────
// 协议见 Better-Dice-Control/docs/CLOUDBAN_API.md（第六节即本客户端约定）：
//   · 同步：GET {url}/api/v1/blacklist（增量游标 since=上次 server_time，分页取完）
//       status=active 且 danger>=阈值 → 并入本地黑名单，reason 统一前缀 "[云黑#<id>]"；
//       status=revoked（墓碑）→ 仅移除带该前缀的本地条目，绝不动骰主手动拉黑的行；
//   · 上报：本地拉黑时（分享开启）POST /api/v1/report，token 复用 heart 绑定 token；
//       reason 已带 "[云黑#" 前缀的条目**跳过上报**（防止同步下来的记录再回声上云）；
//   · 失败一律静默退避（下个周期重试），所有出站请求走 curl 分离线程。
// 本地黑名单 CRUD 经 init() 注入的回调转发到 CommandRouter（避免 header 互相包含）。

#include "../common/logger.h"
#include "../config/config_manager.h"
#include "../storage/database.h"   // BanlistRow
#include "ai_gateway.h"            // dice::ai::httpPostJson —— curl 配置文件出站模板

#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dice::cloudban {

using json = nlohmann::json;

inline constexpr const char* kOfficialCloudbanUrl = "https://cloudban.dice.zone";

/// 并入本地黑名单的 reason 前缀："[云黑#<id>]"（UTF-8 字面量，勿改——墓碑解除按它匹配）。
inline std::string entryPrefix(long long id) {
    return "[\xe4\xba\x91\xe9\xbb\x91#" + std::to_string(id) + "]";
}

/// curl GET（配置文件传参防注入，模式同 ai_gateway::httpPostJson，只是不带
/// request/data-binary 行 → curl 默认 GET）。返回 body，@p status 带回 HTTP 码（0=失败）。
inline std::string httpGetJson(const std::string& url, int timeoutSec, int& status) {
    namespace fs = std::filesystem;
    status = 0;
    auto esc = [](const std::string& s) { std::string o; o.reserve(s.size() + 8);
        for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; };
    static std::atomic<long long> seq{0};
    long long id = ++seq;
    fs::path cfgF = fs::temp_directory_path() / ("dncb_" + std::to_string(id) + ".cfg");
    std::error_code ec;
    try {
        std::ofstream cf(cfgF, std::ios::binary);
        cf << "url = \"" << esc(url) << "\"\n";
        cf << "max-time = " << timeoutSec << "\n";
        cf << "silent\nshow-error\nlocation\n";
        cf << "proto = \"=http,https\"\n";
        cf << "max-filesize = 4194304\n";                 // 4 MB 上限
        cf << "header = \"Accept: application/json\"\n";
        cf << "write-out = \"\\n%{http_code}\"\n";
    } catch (...) { fs::remove(cfgF, ec); return ""; }

    std::string cmd = "curl -K \"" + cfgF.string() + "\"";
    std::string out;
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe) {
        std::array<char, 8192> buf; size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }
    fs::remove(cfgF, ec);
    auto nl = out.find_last_of('\n');
    if (nl != std::string::npos) { status = std::atoi(out.c_str() + nl + 1); out.erase(nl); }
    return out;
}

/// URL 查询参数百分号编码（游标是 ISO8601，含 ':' '+' 等）。
inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

struct SyncResult {
    bool ok = false;
    int added = 0, removed = 0;
    std::string error;
};

class CloudbanService {
public:
    // 本地黑名单操作回调（CommandRouter::cloudBanHas/Add/Remove + banlistAll）。
    using HasFn    = std::function<bool(int type, int list, const std::string& id)>;
    using AddFn    = std::function<void(int type, int list, const std::string& id, const std::string& reason)>;
    using RemoveFn = std::function<bool(int type, int list, const std::string& id)>;
    using AllFn    = std::function<std::vector<BanlistRow>()>;

    static CloudbanService& instance() { static CloudbanService s; return s; }

    void init(ConfigManager* cfg, HasFn has, AddFn add, RemoveFn remove, AllFn all) {
        cfg_ = cfg;
        banHas_ = std::move(has);
        banAdd_ = std::move(add);
        banRemove_ = std::move(remove);
        banAll_ = std::move(all);
    }

    // ── 配置读取（每次现读 → 热更即生效）────────────────────
    bool enabled() const { return cfg_ && cfg_->get<bool>("dice/cloudban_enabled", false); }
    std::string url() const {
        std::string u = cfg_ ? cfg_->get<std::string>("dice/cloudban_url", std::string(kOfficialCloudbanUrl))
                             : std::string(kOfficialCloudbanUrl);
        if (u.empty()) u = kOfficialCloudbanUrl;
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
    /// cloudban_token 为空时使用任一适配器上的账号中心骰娘 API Key。
    /// heart_token 仅保留用于升级旧配置时的短期兼容。
    std::string token() const {
        if (!cfg_) return "";
        std::string t = cfg_->get<std::string>("dice/cloudban_token", std::string());
        if (t.empty()) t = cfg_->get<std::string>("dice/heart_token", std::string());
        if (t.empty()) {
            try {
                json all = cfg_->getAll();
                if (all.contains("adapters") && all["adapters"].is_array()) {
                    for (const auto& adapter : all["adapters"]) {
                        if (!adapter.is_object()) continue;
                        t = adapter.value("heart_api_key", adapter.value("heartApiKey", std::string()));
                        if (!t.empty()) break;
                    }
                }
            } catch (...) {}
        }
        return t;
    }
    bool share() const { return cfg_ && cfg_->get<bool>("dice/cloudban_share", true); }
    int minDanger() const { return cfg_ ? cfg_->get<int>("dice/cloudban_min_danger", 2) : 2; }
    int syncInterval() const {
        int v = cfg_ ? cfg_->get<int>("dice/cloudban_sync_interval", 21600) : 21600;
        if (v < 600) v = 600;
        return v;
    }

    /// 周期驱动（main.cpp runEvery 60s 与心跳共节奏）。内部按 sync_interval 判断到期。
    void tick() {
        if (!cfg_ || !enabled()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (epoch() - lastSyncAt_ < syncInterval()) return;
        }
        if (busy_.load()) return;
        std::thread([this]() { syncNow(false); }).detach();
    }

    /// 全量/增量同步一轮（同步执行——调用方自己放分离线程）。
    /// force=false 时（周期路径）不再检查 interval——tick 已判过；失败静默退避，
    /// lastSyncAt_ 在尝试开始就打点，避免失败后每 60s 重锤服务端。
    SyncResult syncNow(bool force) {
        SyncResult r;
        if (!cfg_ || !banAdd_ || !banRemove_ || !banAll_ || !banHas_) { r.error = "\xe6\x9c\x8d\xe5\x8a\xa1\xe6\x9c\xaa\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96"; return r; }
        if (!enabled()) { r.error = "\xe4\xba\x91\xe9\xbb\x91\xe5\x8a\x9f\xe8\x83\xbd\xe6\x9c\xaa\xe5\xbc\x80\xe5\x90\xaf"; return r; }   // 云黑功能未开启
        (void)force;
        if (busy_.exchange(true)) { r.error = "\xe5\x90\x8c\xe6\xad\xa5\xe8\xbf\x9b\xe8\xa1\x8c\xe4\xb8\xad"; return r; }   // 同步进行中
        {
            std::lock_guard<std::mutex> lk(mu_);
            lastSyncAt_ = epoch();
        }

        std::string base = url();
        std::string cursor = cfg_->get<std::string>("dice/cloudban_cursor", std::string());
        std::string serverTime;
        int mind = minDanger();
        int page = 1;
        const int pageSize = 500;      // 协议上限：尽量减少页数以避开服务端 IP 频控
        const int maxPages = 200;      // 兜底：单轮最多 10 万条
        bool netOk = true;
        bool drained = false;          // 是否真正取完（未取完不推进游标，否则尾部记录永久漏拉）

        while (true) {
            std::string u = base + "/api/v1/blacklist?page=" + std::to_string(page)
                          + "&page_size=" + std::to_string(pageSize);
            if (!cursor.empty()) u += "&since=" + urlEncode(cursor);
            int st = 0;
            std::string body = httpGetJson(u, 20, st);
            if (st == 429) {
                // 撞频控：退避后重试同一页（最多两次），仍失败则本轮中止且不推进游标，下轮再来
                bool recovered = false;
                for (int retry = 0; retry < 2; ++retry) {
                    std::this_thread::sleep_for(std::chrono::seconds(6));
                    st = 0; body = httpGetJson(u, 20, st);
                    if (st == 200) { recovered = true; break; }
                }
                if (!recovered) { netOk = false; r.error = "HTTP 429"; break; }
            } else if (st != 200) {
                netOk = false;
                r.error = st == 0 ? "\xe7\xbd\x91\xe7\xbb\x9c\xe8\xaf\xb7\xe6\xb1\x82\xe5\xa4\xb1\xe8\xb4\xa5" : ("HTTP " + std::to_string(st));   // 网络请求失败
                break;
            }
            int entryCount = 0, total = 0;
            try {
                json j = json::parse(body);
                serverTime = j.value("server_time", serverTime);
                total = j.value("total", 0);
                if (j.contains("entries") && j["entries"].is_array()) {
                    entryCount = (int)j["entries"].size();
                    for (const auto& e : j["entries"]) applyEntry(e, mind, r);
                }
            } catch (...) { netOk = false; r.error = "\xe5\x93\x8d\xe5\xba\x94\xe8\xa7\xa3\xe6\x9e\x90\xe5\xa4\xb1\xe8\xb4\xa5"; break; }   // 响应解析失败
            if (entryCount < pageSize || page * pageSize >= total) { drained = true; break; }
            if (++page > maxPages) break;   // 未取完但达上限：drained=false，不推进游标（下轮从当前 since 继续）
            std::this_thread::sleep_for(std::chrono::milliseconds(200));   // 页间轻节流，进一步避开频控
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            lastAdded_ = r.added;
            lastRemoved_ = r.removed;
            if (netOk) {
                r.ok = true;
                lastSyncIso_ = nowUtcIso();
                lastError_.clear();
                // 仅在真正取完整轮时推进游标：否则中途截断会让未取到的尾部记录被永久跳过
                if (drained && !serverTime.empty()) {
                    try { cfg_->set<std::string>("dice/cloudban_cursor", serverTime); cfg_->save(); } catch (...) {}
                }
            } else {
                lastError_ = r.error;
            }
        }
        if (r.ok && (r.added || r.removed))
            DICE_LOG_INFO("[\xe4\xba\x91\xe9\xbb\x91] \xe5\x90\x8c\xe6\xad\xa5\xe5\xae\x8c\xe6\x88\x90\xef\xbc\x9a\xe5\xb9\xb6\xe5\x85\xa5 {} \xe6\x9d\xa1\xef\xbc\x8c\xe8\xa7\xa3\xe9\x99\xa4 {} \xe6\x9d\xa1", r.added, r.removed);   // 同步完成：并入 N 条，解除 N 条
        else if (!r.ok)
            DICE_LOG_DEBUG("[\xe4\xba\x91\xe9\xbb\x91] \xe5\x90\x8c\xe6\xad\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a{}\xef\xbc\x88\xe4\xb8\x8b\xe5\x91\xa8\xe6\x9c\x9f\xe9\x87\x8d\xe8\xaf\x95\xef\xbc\x89", r.error);   // 同步失败（下周期重试）
        busy_.store(false);
        return r;
    }

    /// 本地拉黑 → 上报云端（status=pending，管理员核验后才全网生效）。
    /// @p targetType "user"/"group"；@p danger 1-3 默认 2。分离线程执行，失败静默。
    void reportToCloud(const std::string& targetType, const std::string& targetId,
                       const std::string& banType, const std::string& reason, int danger = 2) {
        if (!cfg_ || targetId.empty()) return;
        if (!enabled() || !share()) return;
        std::string tk = token();
        if (tk.empty()) return;
        // 防回声环：同步下来的 "[云黑#...]" 条目绝不再上报回云端。
        if (reason.rfind("[\xe4\xba\x91\xe9\xbb\x91#", 0) == 0) return;
        // 当日去重：同一目标一天只报一次。
        std::string day = todayUtc();
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::string key = targetType + ":" + targetId;
            auto it = reported_.find(key);
            if (it != reported_.end() && it->second == day) return;
            reported_[key] = day;
        }
        std::string rs = reason.empty() ? std::string("\xe9\xaa\xb0\xe4\xb8\xbb\xe6\x9c\xac\xe5\x9c\xb0\xe6\x8b\x89\xe9\xbb\x91") : reason;   // 骰主本地拉黑
        if (rs.size() > 450) {   // 协议 reason ≤500 字符，按 UTF-8 边界裁剪
            size_t cut = 450;
            while (cut > 0 && (rs[cut] & 0xC0) == 0x80) --cut;
            rs = rs.substr(0, cut);
        }
        if (danger < 1) danger = 1;
        if (danger > 3) danger = 3;
        std::string base = url();
        json body{
            {"access_token", tk},
            {"target_type", targetType == "group" ? "group" : "user"},
            {"target_id", targetId},
            {"ban_type", banType.empty() ? "other" : banType},
            {"danger", danger},
            {"reason", rs},
        };
        std::thread([base, payload = body.dump(), targetId]() {
            int st = 0;
            dice::ai::httpPostJson(base + "/api/v1/report", "", payload, 15, st);
            if (st == 200)
                DICE_LOG_INFO("[\xe4\xba\x91\xe9\xbb\x91] \xe5\xb7\xb2\xe4\xb8\x8a\xe6\x8a\xa5 {} \xe5\xbe\x85\xe6\xa0\xb8\xe9\xaa\x8c", targetId);   // 已上报 X 待核验
            else
                DICE_LOG_DEBUG("[\xe4\xba\x91\xe9\xbb\x91] \xe4\xb8\x8a\xe6\x8a\xa5\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x88HTTP {}\xef\xbc\x89", st);   // 上报失败（静默）
        }).detach();
    }

    /// 供 /api/system/cloudban 展示的最近同步状态。
    json lastState() const {
        std::lock_guard<std::mutex> lk(mu_);
        return json{
            {"last_sync_at", lastSyncIso_},
            {"last_sync_added", lastAdded_},
            {"last_sync_removed", lastRemoved_},
            {"last_error", lastError_},
        };
    }

private:
    CloudbanService() = default;

    static long long epoch() {
        return (long long)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    static std::string nowUtcIso() {
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
    static std::string todayUtc() { return nowUtcIso().substr(0, 10); }

    /// 应用一条同步记录：active → 并入；revoked → 按 "[云黑#id]" 前缀解除。
    /// 整体 try/catch —— 单条脏数据（字段类型异常等）跳过，不拖垮整轮同步。
    void applyEntry(const json& e, int mind, SyncResult& r) {
        try { applyEntryImpl(e, mind, r); } catch (...) {}
    }
    void applyEntryImpl(const json& e, int mind, SyncResult& r) {
        if (!e.is_object()) return;
        long long id = e.value("id", (long long)0);
        if (id <= 0) return;
        std::string status = e.value("status", std::string());
        std::string prefix = entryPrefix(id);
        if (status == "active") {
            int danger = e.value("danger", 2);
            if (danger < mind) return;
            std::string tt = e.value("target_type", std::string());
            std::string tid = e.value("target_id", std::string());
            if (tid.empty()) return;
            int type = (tt == "group") ? 1 : 0;
            if (banHas_(type, 0, tid)) return;   // 已在黑名单（含骰主手动拉的）→ 不重复计数
            std::string reason = prefix + " " + e.value("ban_type", std::string("other"));
            std::string why = e.value("reason", std::string());
            if (!why.empty()) reason += " " + why;
            banAdd_(type, 0, tid, reason);
            ++r.added;
        } else if (status == "revoked") {
            // 墓碑：仅解除 reason 以 "[云黑#<id>]" 开头的行，绝不动无前缀条目。
            for (const auto& row : banAll_()) {
                if (row.listType != 0) continue;
                if (row.reason.rfind(prefix, 0) != 0) continue;
                if (banRemove_(row.targetType, row.listType, row.targetId)) ++r.removed;
            }
        }
    }

    ConfigManager* cfg_ = nullptr;
    HasFn banHas_;
    AddFn banAdd_;
    RemoveFn banRemove_;
    AllFn banAll_;

    mutable std::mutex mu_;
    std::atomic<bool> busy_{false};
    long long lastSyncAt_ = 0;              // 上次同步尝试（epoch 秒，失败也打点=静默退避）
    std::string lastSyncIso_;               // 上次**成功**同步时刻
    int lastAdded_ = 0, lastRemoved_ = 0;
    std::string lastError_;
    std::map<std::string, std::string> reported_;   // "type:id" → 当日（上报去重）
};

}  // namespace dice::cloudban
