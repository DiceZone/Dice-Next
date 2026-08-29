#pragma once
// ─── Dice!Next — 通知系统 ──────────────────────────────────────────
// 通知会写入审计日志、回当前会话，并推送给订阅相应级别的
// 「通知窗口」（console.NoticeList）。这里复刻 ①③（②由指令回复自身完成），并扩展：
//   · 事件细粒度订阅：每个通知窗口可逐项勾选具体事件（op），指令 .notice level N
//     仍按区域快速订阅（= 勾选该区域全部事件）。
//   · 第三方推送：Webhook（POST JSON）与 SMTP 邮件，均按区域掩码过滤、后台线程发送。
//
// 区域位掩码：kRoutine(例行) / kImportant(重要) / kCritical(关键) / kError(错误)。
// 配置 dice/notice：{ windows: [ { platform, chat_id, is_group, name, level_mask, events[] } ],
//                     webhook: { enabled, url, level_mask },
//                     smtp: { enabled, host, port, ssl, user, pass, from, to, level_mask } }

#include "../config/config_manager.h"
#include "../common/subprocess.h"
#include "../adapter/adapter_manager.h"
#include "../adapter/adapter_interface.h"
#include <nlohmann/json.hpp>
#include <spdlog/sinks/base_sink.h>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <ctime>

namespace dice::notice {

using json = nlohmann::json;

constexpr int kRoutine   = 0b1;    // 例行操作（信任变更/别名绑定）
constexpr int kImportant = 0b10;   // 重要（好友/邀请/退群/定时/全局开关）
constexpr int kCritical  = 0b100;  // 关键（管理权限增减/拉黑）
constexpr int kError     = 0b1000; // 错误或警告（运行异常）
constexpr int kAll       = 0b1111;

// 事件目录：op → 所属区域。与所有 notify() 调用点保持同步；前端逐项勾选用同一份 op 名。
inline const std::vector<std::pair<std::string, int>>& eventCatalog() {
    static const std::vector<std::pair<std::string, int>> v{
        {"trust", kRoutine}, {"alias", kRoutine},
        {"friend_req", kImportant}, {"friend_add", kImportant}, {"group_invite", kImportant},
        {"leave", kImportant}, {"dismiss", kImportant}, {"group_left", kImportant},
        {"blacklist_leave", kImportant}, {"nonfriend_leave", kImportant}, {"keyword_leave", kImportant},
        {"schedule", kImportant}, {"global", kImportant},
        {"censor", kImportant},
        {"update_available", kImportant}, {"update_result", kImportant},
        {"admin", kCritical}, {"blacklist", kCritical},
        {"error", kError}, {"update_error", kError},
    };
    return v;
}
// 某区域掩码覆盖的全部事件（指令 .notice level N 的展开）。
inline std::vector<std::string> eventsForMask(int mask) {
    std::vector<std::string> v;
    for (auto& [op, area] : eventCatalog()) if (area & mask) v.push_back(op);
    return v;
}
inline int areaOf(const std::string& op) {
    for (auto& [o, area] : eventCatalog()) if (o == op) return area;
    return kImportant;   // 未登记的事件按「重要」
}

struct Window {
    std::string platform, chatId;
    std::string adapterId;   // 空=全局窗口（所有帐号来源的通知都发）；非空=仅该适配器帐号来源
    bool isGroup = true;
    int mask = kAll;
    std::vector<std::string> events;   // 非空=逐项订阅；空=按 mask 区域订阅（指令/旧数据）
};

inline json conf(ConfigManager& cfg) {
    try { auto n = cfg.get<json>("dice/notice", json::object()); return n.is_object() ? n : json::object(); }
    catch (...) { return json::object(); }
}
inline std::vector<Window> windows(ConfigManager& cfg) {
    std::vector<Window> v; json c = conf(cfg);
    if (c.contains("windows") && c["windows"].is_array())
        for (auto& w : c["windows"]) {
            if (!w.is_object()) continue;
            Window win;
            win.platform = w.value("platform", std::string());
            win.adapterId = w.value("adapter_id", std::string());
            win.chatId   = w.value("chat_id", std::string());
            win.isGroup  = w.value("is_group", true);
            win.mask     = w.value("level_mask", (int)kAll);
            if (w.contains("events") && w["events"].is_array())
                for (auto& e : w["events"]) if (e.is_string()) win.events.push_back(e.get<std::string>());
            if (!win.chatId.empty()) v.push_back(std::move(win));
        }
    return v;
}

/// 已连接且平台匹配的适配器，按内部适配器编号升序排列（系统级通知的回退顺序）。
inline std::vector<AdapterPtr> orderedCandidates(AdapterManager& adapters, const std::string& platform) {
    std::vector<AdapterPtr> v;
    for (auto& a : adapters.allAdapters()) {
        if (!a || !a->isConnected()) continue;
        if (!platform.empty() && a->platform() != platform) continue;
        v.push_back(a);
    }
    std::sort(v.begin(), v.end(), [](const AdapterPtr& x, const AdapterPtr& y) {
        int xi = 0, yi = 0;
        try { xi = std::stoi(x->id()); } catch (...) {}
        try { yi = std::stoi(y->id()); } catch (...) {}
        return xi < yi;
    });
    return v;
}

// 结构化审计：每行一条 JSON（data/audit/notice_<date>.jsonl），含来源（发生地）。
inline void audit(int level, const std::string& msg, const std::string& op = "", const std::string& origin = "") {
    try {
        namespace fs = std::filesystem;
        fs::create_directories("data/audit");
        std::time_t t = std::time(nullptr);
        const std::string date = utils::formatTimeInTimezone(t, "%Y-%m-%d");
        const std::string ts = utils::formatTimeInTimezone(t, "%Y-%m-%d %H:%M:%S");
        json rec = {{"ts", ts}, {"level", level}, {"op", op}, {"msg", msg}, {"origin", origin}};
        std::ofstream f(std::string("data/audit/notice_") + date + ".jsonl", std::ios::app | std::ios::binary);
        f << rec.dump() << "\n";
    } catch (...) {}
}

// curl 配置文件里的转义。
inline std::string curlEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
    return o;
}

// Webhook：后台线程 POST JSON（配置文件传参防注入，超时 10s，静默失败）。
inline void webhookPostAsync(const std::string& url, const std::string& body) {
    if (url.rfind("http", 0) != 0) return;
    std::thread([url, body]() {
        namespace fs = std::filesystem;
        static std::atomic<long long> seq{0};
        long long id = ++seq;
        fs::path tmp = fs::temp_directory_path();
        fs::path cfgF = tmp / ("dnwh_" + std::to_string(id) + ".cfg");
        fs::path bodyF = tmp / ("dnwh_" + std::to_string(id) + ".body");
        std::error_code ec;
        try {
            { std::ofstream bf(bodyF, std::ios::binary); bf << body; }
            std::ofstream cf(cfgF, std::ios::binary);
            cf << "url = \"" << curlEsc(url) << "\"\nrequest = \"POST\"\nmax-time = 10\nsilent\n"
               << "proto = \"=http,https\"\n"
               << "header = \"Content-Type: application/json\"\n"
               << "data-binary = \"@" << curlEsc(bodyF.string()) << "\"\n";
        } catch (...) { fs::remove(cfgF, ec); fs::remove(bodyF, ec); return; }
        dice::proc::curlConfig(cfgF);
        fs::remove(cfgF, ec); fs::remove(bodyF, ec);
    }).detach();
}

// 标准 base64（SMTP 主题 RFC2047 编码用）。
inline std::string b64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        bool two = (i + 1 < in.size());
        if (two) v |= (unsigned char)in[i + 1] << 8;
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
        o += two ? T[(v >> 6) & 63] : '='; o += '=';
    }
    return o;
}

// SMTP：后台线程 curl smtp(s):// 发信。ssl=true → smtps（465 隐式 TLS）；false → smtp +
// 尽力 STARTTLS（curl "ssl" 选项，587/25）。主题 RFC2047 UTF-8，正文 text/plain UTF-8。
inline void smtpSendAsync(const json& s, const std::string& subject, const std::string& body) {
    std::thread([s, subject, body]() {
        try {
            std::string host = s.value("host", std::string());
            std::string user = s.value("user", std::string());
            std::string pass = s.value("pass", std::string());
            std::string from = s.value("from", std::string());
            std::string to   = s.value("to", std::string());
            int port = s.value("port", 465);
            bool ssl = s.value("ssl", true);
            if (host.empty() || from.empty() || to.empty()) return;
            namespace fs = std::filesystem;
            static std::atomic<long long> seq{0};
            long long id = ++seq;
            fs::path tmp = fs::temp_directory_path();
            fs::path cfgF = tmp / ("dnsm_" + std::to_string(id) + ".cfg");
            fs::path msgF = tmp / ("dnsm_" + std::to_string(id) + ".eml");
            std::error_code ec;
            {
                std::ofstream mf(msgF, std::ios::binary);
                mf << "From: <" << from << ">\r\nTo: <" << to << ">\r\n"
                   << "Subject: =?UTF-8?B?" << b64(subject) << "?=\r\n"
                   << "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
                   << "Content-Transfer-Encoding: base64\r\n\r\n" << b64(body) << "\r\n";
            }
            {
                std::ofstream cf(cfgF, std::ios::binary);
                cf << "url = \"" << (ssl ? "smtps" : "smtp") << "://" << curlEsc(host) << ":" << port << "\"\n";
                cf << "mail-from = \"" << curlEsc(from) << "\"\n";
                cf << "mail-rcpt = \"" << curlEsc(to) << "\"\n";
                if (!user.empty()) cf << "user = \"" << curlEsc(user + ":" + pass) << "\"\n";
                if (!ssl) cf << "ssl\n";   // STARTTLS（尽力）
                cf << "upload-file = \"" << curlEsc(msgF.string()) << "\"\nmax-time = 20\nsilent\n";
            }
            dice::proc::curlConfig(cfgF);
            fs::remove(cfgF, ec); fs::remove(msgF, ec);
        } catch (...) {}
    }).detach();
}

// 推送 + 审计。窗口匹配：有 events 列表则按事件 op 逐项匹配，否则按区域掩码。
// origin* 用于排除来源窗口（避免回声）并记入审计「发生地」。
inline void notify(ConfigManager& cfg, AdapterManager& adapters, int level, const std::string& msg,
                   const std::string& originPlatform = "", const std::string& originChat = "",
                   const std::string& op = "", const std::string& originAdapterId = "") {
    std::string origin = originChat.empty() ? std::string() : (originPlatform + ":" + originChat);
    audit(level, msg, op, origin);
    json c = conf(cfg);
    for (auto& w : windows(cfg)) {
        // 帐号窗口只收该帐号来源的通知；全局窗口（adapter_id 空）收所有帐号。
        // 测试通知（op=="test"）不受来源限制，逐窗口验证链路。
        if (op != "test" && !w.adapterId.empty() && !originAdapterId.empty() &&
            w.adapterId != originAdapterId) continue;
        bool hit = (op == "test")   // 测试通知不受订阅过滤（骰主手动触发验证链路）
            || (op.empty() ? ((w.mask & level) != 0)
                : (!w.events.empty()
                    ? (std::find(w.events.begin(), w.events.end(), op) != w.events.end())
                    : ((w.mask & level) != 0)));
        if (!hit) continue;
        if (!originChat.empty() && w.platform == originPlatform && w.chatId == originChat) continue;  // 不回推来源
        // 哪里来的就发给谁：帐号来源的通知先由来源适配器发；发送失败（例如
        // 掉线通知时来源已离线）或系统级通知，则按内部适配器编号顺序依次尝试，
        // 直到成功发出一次。
        auto trySend = [&](const AdapterPtr& a) -> bool {
            if (!a || !a->isConnected()) return false;
            try {
                Message m;
                m.platform = a->platform();
                m.type = w.isGroup ? MessageType::kGroup : MessageType::kPrivate;
                m.targetId = w.chatId;
                m.content = msg;
                a->sendMessage(m);
                return true;
            } catch (...) { return false; }
        };
        bool sent = false;
        const std::string preferredAdapterId =
            !originAdapterId.empty() ? originAdapterId : w.adapterId;
        if (!preferredAdapterId.empty()) {
            auto src = adapters.getAdapter(preferredAdapterId);
            if (src && src->isConnected()
                && (w.platform.empty() || src->platform() == w.platform))
                sent = trySend(src);
        }
        if (!sent) {
            for (auto& a : orderedCandidates(adapters, w.platform)) {
                if (sent) break;
                sent = trySend(a);
            }
        }
    }
    // 第三方推送（按区域掩码过滤；后台线程，不阻塞调用方）。
    if (c.contains("webhook") && c["webhook"].is_object()) {
        auto& wh = c["webhook"];
        if (wh.value("enabled", false) && (wh.value("level_mask", (int)kAll) & level)) {
            std::time_t t = std::time(nullptr);
            const std::string ts = utils::formatTimeInTimezone(t, "%Y-%m-%d %H:%M:%S");
            webhookPostAsync(wh.value("url", std::string()),
                json{{"ts", ts}, {"level", level}, {"op", op}, {"msg", msg}, {"origin", origin}}.dump());
        }
    }
    if (c.contains("smtp") && c["smtp"].is_object()) {
        auto& sm = c["smtp"];
        if (sm.value("enabled", false) && (sm.value("level_mask", (int)kAll) & level))
            smtpSendAsync(sm, "[Dice!Next] " + op, msg + (origin.empty() ? "" : "\n\n(" + origin + ")"));
    }
}

// ─── 运行报错推送 ──────────────────────────────────────────────────
// spdlog 自定义 sink：捕获 ERROR 级日志 → 通知订阅了 kError 的窗口。
// 防护：① thread_local 重入保护（notify 发送本身出错打日志时不再递归）；
//       ② 节流：60 秒窗口内只推第一条（其余仍在日志文件里）。
class ErrorNotifySink : public spdlog::sinks::base_sink<std::mutex> {
public:
    using Callback = std::function<void(const std::string&)>;
    explicit ErrorNotifySink(Callback cb) : cb_(std::move(cb)) {}
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level < spdlog::level::err || !cb_) return;
        static thread_local bool inNotify = false;
        if (inNotify) return;                                  // 重入保护
        static std::atomic<long long> lastSent{0};
        long long now = (long long)std::time(nullptr);
        long long prev = lastSent.load();
        if (now - prev < 60) return;                           // 节流：60s 一条
        if (!lastSent.compare_exchange_strong(prev, now)) return;
        inNotify = true;
        try { cb_(std::string(msg.payload.data(), msg.payload.size())); } catch (...) {}
        inNotify = false;
    }
    void flush_() override {}
private:
    Callback cb_;
};

}  // namespace dice::notice
