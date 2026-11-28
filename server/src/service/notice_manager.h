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
#include "../common/libcurl_http.h"
#include "../adapter/adapter_manager.h"
#include "../adapter/adapter_interface.h"
#include "notice_rules.h"
#include "notice_content.h"
#include "notice_event_text.h"
#include <nlohmann/json.hpp>
#include <spdlog/sinks/base_sink.h>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <ctime>
#include <optional>

namespace dice::notice {

using json = nlohmann::json;

constexpr int kRoutine   = 0b1;    // 例行操作（信任变更/别名绑定）
constexpr int kImportant = 0b10;   // 重要（好友/邀请/退群/定时/全局开关）
constexpr int kCritical  = 0b100;  // 关键（管理权限增减/拉黑）
constexpr int kError     = 0b1000; // 错误或警告（运行异常）
constexpr int kAll       = 0b1111;

// 事件目录：op → 所属区域。与所有 notify() 调用点保持同步；前端逐项勾选用同一份 op 名。
inline const std::vector<std::pair<std::string, int>>& eventCatalog() {
    return notice_rules::catalog();
}
// 某区域掩码覆盖的全部事件（指令 .notice level N 的展开）。
inline std::vector<std::string> eventsForMask(int mask) {
    std::vector<std::string> v;
    for (auto& [op, area] : eventCatalog()) if ((area & mask) && !notice_rules::optInOnly(op)) v.push_back(op);
    return v;
}

// 同一个故障只在开始时通知一次，之后持续发生也不再重复推送：
//   · 距上次出现已超过 resolvedAfter 秒 → 视为已恢复，再出现时当作新故障立即通知；
//   · 一直没恢复 → 每 24 小时提醒一次，并附上期间另出现的次数。
// 原版 Dice! 的内存／CPU／硬盘警告也是只在越过阈值与恢复时各通知一次。
class FaultGate {
  public:
    struct Decision { bool send = false; size_t suppressed = 0; };
    static constexpr long long kRemindEvery = 24 * 60 * 60;
    Decision admit(const std::string& key, long long now, long long resolvedAfter) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(key);
        if (found == entries_.end() || now < found->second.lastSeen || now - found->second.lastSeen >= resolvedAfter) {
            if (found == entries_.end() && entries_.size() >= 256) {
                auto oldest = std::min_element(entries_.begin(), entries_.end(),
                    [](const auto& a, const auto& b) { return a.second.lastSeen < b.second.lastSeen; });
                entries_.erase(oldest);
            }
            entries_[key] = {now, now, 0};
            return {true, 0};
        }
        auto& e = found->second;
        e.lastSeen = now;
        if (now - e.lastSent >= kRemindEvery) {
            Decision d{true, e.suppressed};
            e.lastSent = now; e.suppressed = 0;
            return d;
        }
        ++e.suppressed;
        return {false, 0};
    }
  private:
    struct Entry { long long lastSent, lastSeen; size_t suppressed; };
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};
inline FaultGate& faultGate() { static FaultGate gate; return gate; }
// 平台或插件的运行错误：一小时没再出现算恢复。更新检查失败按检查周期（默认 6 小时）
// 才会再出现，所以只按 24 小时提醒；退群失败同理。
inline long long faultResolvedAfter(const std::string& op) {
    return op == "error" ? 60 * 60 : 7 * 24 * 60 * 60;
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

inline void webhookPostAsync(const json& channel, const json& variables) {
    std::thread([channel, variables, context = activeProcessingContext]() {
        ProcessingScope scope(context); // Notification transport must never recursively notify.
        try {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const auto request = notice_webhook::build(channel, variables, now);
            dice::http::Fetch req;
            req.url = request.url;
            req.method = request.method;
            req.body = request.body;
            req.connectTimeoutSec = 5;
            req.timeoutSec = 15;
            req.maxBytes = 64 * 1024;
            req.allowHttp = true;
            bool contentType = false;
            for (auto it = request.headers.begin(); it != request.headers.end(); ++it) {
                auto key = it.key();
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (key == "content-type") contentType = true;
                req.headers.emplace_back(it.key(), it->get<std::string>());
            }
            if (!contentType) req.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
            const auto result = dice::http::fetch(req);
            if (!result.ok()) {
                DICE_LOG_WARN_M("通知", "Webhook 请求未完成（HTTP {}），请检查通知窗口地址及服务端状态。", result.http); return;
            }
            const auto profile = channel.value("profile", std::string("generic"));
            if (profile == "wecom" || profile == "dingtalk" || profile == "feishu") {
                const auto reply = json::parse(result.body, nullptr, false);
                const auto key = profile == "feishu" ? "code" : "errcode";
                if (!reply.is_object() || !reply.contains(key) || !reply[key].is_number_integer() || reply[key] != 0)
                    DICE_LOG_WARN_M("通知", "Webhook 服务端未确认成功，请检查机器人关键词、签名密钥及推送权限。");
            }
        } catch (const std::exception&) {
            // Never echo a URL, header, payload or provider response containing credentials.
            DICE_LOG_WARN_M("通知", "Webhook 请求准备失败，请检查通知窗口的地址、请求模板与签名设置。");
        }
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
            fs::path msgF = tmp / ("dnsm_" + std::to_string(id) + ".eml");
            std::error_code ec;
            {
                std::ofstream mf(msgF, std::ios::binary);
                mf << "From: <" << from << ">\r\nTo: <" << to << ">\r\n"
                   << "Subject: =?UTF-8?B?" << b64(subject) << "?=\r\n"
                   << "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
                   << "Content-Transfer-Encoding: base64\r\n\r\n" << b64(body) << "\r\n";
            }
            dice::http::Fetch req;
            req.url = std::string(ssl ? "smtps://" : "smtp://") + host + ":" + std::to_string(port);
            req.mailFrom = from;
            req.mailRcpt = to;
            req.username = user;
            req.password = pass;
            req.smtpStartTls = !ssl;
            req.uploadFile = msgF;
            req.timeoutSec = 20;
            dice::http::fetch(req);
            fs::remove(msgF, ec);
        } catch (...) {}
    }).detach();
}

// 推送 + 审计。窗口匹配：有 events 列表则按事件 op 逐项匹配，否则按区域掩码。
// origin* 用于排除来源窗口（避免回声）并记入审计「发生地」。
inline void notify(ConfigManager &cfg, AdapterManager &adapters, int level, const std::string &msg,
                   const std::string &originPlatform = "", const std::string &originChat = "",
                   const std::string &op = "", const std::string &originAdapterId = "",
                   const std::string &ruleId = "", const std::string& displayTitle = "",
                   const std::set<std::string>* ruleIds = nullptr, const json* destination = nullptr) {
    auto processing = activeProcessingContext;
    if (processing.origin.adapterId.empty()) processing.origin.adapterId = originAdapterId;
    if (processing.origin.platform.empty()) processing.origin.platform = originPlatform;
    if (processing.origin.groupId.empty() && !originChat.empty() && originChat != processing.origin.userId)
        processing.origin.groupId = originChat;
    enrichOrigin(processing.origin, adapters);
    const auto fromPlatform = originPlatform.empty() ? processing.origin.platform : originPlatform;
    const auto fromAdapter = originAdapterId.empty() ? processing.origin.adapterId : originAdapterId;
    const auto fromChat = originChat.empty() ? (!processing.origin.groupId.empty() ? processing.origin.groupId : processing.origin.userId) : originChat;
    const auto title = displayTitle.empty() ? eventTitle(op) : readable(displayTitle, 120);
    std::string reminder;
    // 更新失败／退群失败：同一原因只在开始时推送，持续存在时每天提醒一次。
    // 运行错误在 ErrorNotifySink 里按更细的键合并，这里不再重复处理。
    bool deliver = true;
    if (ruleId.empty() && !ruleIds && !destination && (op == "update_error" || op == "leave_error")) {
        const auto d = faultGate().admit(json::array({op, fromAdapter, msg}).dump(-1, ' ', false, json::error_handler_t::replace),
                                         (long long)std::time(nullptr), faultResolvedAfter(op));
        deliver = d.send;
        if (d.send && d.suppressed) reminder = "\n（过去 24 小时内相同问题另出现 " + std::to_string(d.suppressed) + " 次）";
    }
    auto render = [&](bool includeAdapter) {
        return notificationText(level, op, msg, processing, "", title, includeAdapter) + reminder;
    };
    const auto content = render(true); // Audit, external channels and cross-account chat.
    std::optional<std::string> sameAdapterContent;
    ProcessingScope source(ProcessingContext{
        {{"通知", title, "", ""}}, processing.trigger, "通知内容已生成", processing.origin});
    OutboundScope independentNotification(nullptr);
    std::string origin = fromChat.empty() ? std::string() : (fromPlatform + ":" + fromChat);
    audit(level, content, op, origin);
    if (!deliver) return;
    json c = conf(cfg);
    std::set<std::string> delivered;
    for (const auto &rule : notice_rules::all(c)) {
        if (ruleIds && !ruleIds->count(rule.value("id", std::string()))) continue;
        if (!ruleId.empty() && rule.value("id", std::string()) != ruleId)
            continue;
        if (op != "test" && (!notice_rules::subscribed(rule, level, op) ||
                             !notice_rules::sourceMatches(rule, fromPlatform, fromAdapter)))
            continue;
        if (op == "test" && ruleId.empty() && !ruleIds && !rule.value("enabled", true))
            continue;
        for (const auto &channel : rule.value("channels", json::array())) {
            if (destination && notice_rules::destinationKey(channel) != notice_rules::destinationKey(*destination)) continue;
            const auto kind = channel.value("kind", std::string());
            if (kind == "webhook") {
                const auto url = channel.value("url", std::string());
                if (notice_rules::webhook(url) && delivered.insert("webhook:" + url).second)
                    webhookPostAsync(channel, json{{"time", utils::formatTimeInTimezone(std::time(nullptr),
                                                                                  "%Y-%m-%d %H:%M:%S")},
                                               {"level", level},
                                               {"level_name", levelNames(level)},
                                               {"event", op},
                                               {"title", title},
                                               {"message", content}, {"origin", origin},
                                               {"platform", processing.origin.platform}, {"adapter_id", processing.origin.adapterId},
                                               {"account_id", processing.origin.accountId}, {"group_id", processing.origin.groupId},
                                               {"group_name", processing.origin.groupName}, {"user_id", processing.origin.userId}, {"user_name", processing.origin.userName}});
                continue;
            }
            if (kind == "smtp") {
                const auto to = channel.value("to", std::string());
                if (notice_rules::mailbox(to) && delivered.insert(notice_rules::destinationKey(channel)).second)
                    smtpSendAsync(notice_rules::smtpTransport(cfg.getAll(), c, channel), "[Dice!Next] " + title,
                                  content);
                continue;
            }
            if (kind != "chat")
                continue;
            Window w;
            w.platform = channel.value("platform", std::string());
            w.chatId = channel.value("chat_id", std::string());
            w.isGroup = channel.value("is_group", true);
            w.adapterId = channel.value("preferred_adapter", std::string());
            if (w.chatId.empty())
                continue;
            // 帐号窗口只收该帐号来源的通知；全局窗口（adapter_id 空）收所有帐号。
            // 测试通知（op=="test"）不受来源限制，逐窗口验证链路。
            if (!fromChat.empty() && w.platform == fromPlatform && w.chatId == fromChat)
                continue; // 不回推来源
            // 哪里来的就发给谁：帐号来源的通知先由来源适配器发；发送失败（例如
            // 掉线通知时来源已离线）或系统级通知，则按内部适配器编号顺序依次尝试，
            // 直到成功发出一次。
            auto trySend = [&](const AdapterPtr &a) -> bool {
                if (!a)
                    return false;
                if (!w.platform.empty() && a->platform() != w.platform)
                    return false;
                const auto key = json::array({a->id(), w.chatId, w.isGroup}).dump();
                if (delivered.count(key))
                    return true;
                ProcessingOrigin destination;
                destination.platform = a->platform();
                destination.adapterId = a->id();
                destination.targetId = w.chatId;
                destination.targetType = w.isGroup ? "group" : "user";
                enrichOrigin(destination, adapters);
                const auto target = std::string(w.isGroup ? "群" : "用户") +
                                    (destination.targetName.empty() || destination.targetName == w.chatId
                                         ? w.chatId
                                         : identity(destination.targetName, w.chatId));
                const auto platformName = a->platform() == "onebot_v11"    ? "OneBot"
                                          : a->platform() == "qq_official" ? "QQ官方"
                                                                           : a->platform();
                const auto label =
                    std::string(platformName) +
                    (destination.accountId.empty() ? "「" + a->name() + "」" : ":" + destination.accountId);
                const bool sameAdapter = !processing.origin.adapterId.empty() && processing.origin.adapterId == a->id();
                if (sameAdapter && !sameAdapterContent) sameAdapterContent = render(false);
                const auto& chatContent = sameAdapter ? *sameAdapterContent : content;
                auto prepared =
                    std::make_shared<PreparedOutput>(target, chatContent, activeProcessingContext, label);
                OutboundScope delivery(prepared);
                auto failure = OutboundAttempt::prepare(label, target, chatContent);
                if (!a->isConnected()) {
                    failure.failed("适配器未连接");
                    return false;
                }
                Message m;
                m.platform = a->platform();
                m.type = w.isGroup ? MessageType::kGroup : MessageType::kPrivate;
                m.targetId = w.chatId;
                m.content = chatContent;
                try {
                    a->sendMessage(m);
                    delivered.insert(key);
                    return true;
                } catch (const std::exception &error) {
                    failure.failed(error.what());
                    return false;
                } catch (...) {
                    failure.failed("发送接口抛出未知异常");
                    return false;
                }
            };
            bool sent = false;
            if (channel.value("delivery", std::string("auto")) == "selected") {
                for (const auto &id : channel.value("adapter_ids", json::array()))
                    if (id.is_string())
                        trySend(adapters.getAdapter(id.get<std::string>()));
                continue; // An explicit account selection must never fall back to another account.
            }
            const std::string preferredAdapterId = !fromAdapter.empty() ? fromAdapter : w.adapterId;
            if (!preferredAdapterId.empty()) {
                auto src = adapters.getAdapter(preferredAdapterId);
                if (src && src->isConnected() && (w.platform.empty() || src->platform() == w.platform))
                    sent = trySend(src);
            }
            if (!sent) {
                for (auto &a : orderedCandidates(adapters, w.platform)) {
                    if (sent)
                        break;
                    sent = trySend(a);
                }
            }
        }
    }
}

inline void emit(ConfigManager& cfg, AdapterManager& adapters, const EventText& text,
                 const std::string& adapterId = "") {
    notify(cfg, adapters, text.level, text.body, "", "", text.operation, adapterId, "", text.title);
}
// Notification delivery must not turn an already committed management change
// into an apparent failure (and cause the caller to repeat that change).
inline void emitSafely(ConfigManager& cfg, AdapterManager& adapters, const EventText& text,
                       const std::string& adapterId = "") noexcept {
    try { emit(cfg, adapters, text, adapterId); }
    catch (const std::exception& error) {
        try { DICE_LOG_WARN("管理操作已执行，但未能生成通知：{}", error.what()); } catch (...) {}
    } catch (...) {
        try { DICE_LOG_WARN("管理操作已执行，但未能生成通知：未知异常"); } catch (...) {}
    }
}

// Queueing an adapter action is not a leave confirmation. Correlate a later
// platform event without changing group policy, permissions or punishment.
inline bool requestGroupLeave(ConfigManager& cfg, AdapterManager& adapters, const AdapterPtr& adapter,
                              const std::string& groupId, LeaveIntent intent) {
    auto context = activeProcessingContext;
    if (adapter) {
        context.origin.platform = adapter->platform(); context.origin.adapterId = adapter->id();
        context.origin.accountId = adapter->getLoginId();
    }
    context.origin.groupId = groupId; context.origin.groupName = intent.groupName;
    context.origin.targetId.clear(); context.origin.targetName.clear();
    enrichOrigin(context.origin, adapters);
    if (intent.groupName.empty()) intent.groupName = context.origin.groupName;
    if (intent.actor.userId.empty() && intent.actor.entry.empty()) intent.actor = context.origin;
    ProcessingScope originScope(std::move(context));
    auto failed = [&](const std::string& error) {
        emitSafely(cfg, adapters, {"leave_error", "退群请求失败", requestedBy(intent) + "请求骰娘退出" +
            groupIdentity(intent.groupName, groupId) + "失败：" + readable(error, 1000) + "。", 8}, adapter ? adapter->id() : "");
        return false;
    };
    if (groupId.empty()) return failed("未提供群帐号，未提交退群请求");
    if (!adapter || !adapter->isConnected()) return failed("来源适配器未连接，未提交退群请求");
    if (!adapter->supportsGroupLeave()) return failed("此适配器不支持主动退群，未提交退群请求");
    const auto request = LeaveIntents::instance().remember(adapter->id(), adapter->platform(), groupId, intent);
    try { adapter->leaveGroup(groupId); }
    catch (const std::exception& error) { LeaveIntents::instance().cancel(request); return failed(error.what()); }
    catch (...) { LeaveIntents::instance().cancel(request); return failed("退群接口抛出未知异常"); }
    emitSafely(cfg, adapters, leaveRequested(groupId, intent), adapter->id());
    return true;
}

// ─── 运行报错推送 ──────────────────────────────────────────────────
// spdlog 自定义 sink：捕获 ERROR 级日志 → 通知订阅了 kError 的窗口。
// 防护：① 同步重入保护；② 从日志快照识别异步通知投递，禁止错误自循环；
//       ③ 同一个故障（同一插件／适配器的同一错误）只在开始时通知一次，见 FaultGate；
//          不同群、不同用户触发的同一插件错误算同一个故障，不分别推送。
// No mutex may be held across cb_: sending a notification can log another error.
// The callback is immutable, throttling is atomic and reentry is thread-local.
class ErrorNotifySink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
  public:
    using Callback = std::function<void(const ErrorReport &)>;
    using Clock = std::function<long long()>;
    explicit ErrorNotifySink(Callback cb, Clock clock = [] { return (long long)std::time(nullptr); })
        : cb_(std::move(cb)), clock_(std::move(clock)) {}

  protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        if (msg.level < spdlog::level::err || !cb_)
            return;
        static thread_local bool inNotify = false;
        if (inNotify)
            return; // 重入保护
        auto record = currentLogRecord();
        auto processing = activeOutboundContext.output ? activeOutboundContext.output->processing : activeProcessingContext;
        const auto details = record.is_object() && record.contains("details") && record["details"].is_object()
                                 ? record["details"]
                                 : json::object();
        if (details.contains("processing") && details["processing"].is_object())
            processing = processingContextFromJson(details["processing"]);
        if (isNotificationProcessing(processing) ||
            (details.contains("notification_delivery") && details["notification_delivery"].is_boolean() &&
             details["notification_delivery"].get<bool>()))
            return;
        ErrorReport report{std::move(record), std::move(processing)};
        if(report.record.contains("details") && report.record["details"].is_object()
            && report.record["details"].contains("notice_event") && report.record["details"]["notice_event"].is_string()
            && report.record["details"]["notice_event"].get<std::string>()=="update_error") return;
        if (report.record.empty()) report.record = {{"message", sanitizeLogText({msg.payload.data(), msg.payload.size()})},
            {"module", std::string(msg.logger_name.data(), msg.logger_name.size())}};
        auto message = report.record.value("message", std::string());
        const auto suffix = message.find("(msgID:"); if (suffix != std::string::npos) message.resize(suffix);
        // A platform error is one failure even when the attempted body/msgID
        // changes. Keep different targets/accounts/reasons independent.
        auto field = [&](const char *key) {
            const auto item = details.find(key);
            return item != details.end() && item->is_string() ? item->get<std::string>() : std::string();
        };
        const auto operation = field("operation");
        const auto errorKey =
            operation == "发送消息" || operation == "发送通知"
                ? json::array({field("adapter"), field("target"), field("error"), operation})
                      .dump(-1, ' ', false, json::error_handler_t::replace)
                : message.substr(0, 1200);
        const auto key = json::array({report.processing.label(), report.processing.origin.platform,
                                      report.processing.origin.adapterId,
                                      report.record.value("module", std::string()), errorKey})
                             .dump(-1, ' ', false, json::error_handler_t::replace);
        const auto decision = gate_.admit(key, clock_(), faultResolvedAfter("error"));
        if (!decision.send) return;
        report.suppressed = decision.suppressed;
        inNotify = true;
        try { cb_(report); }
        catch (...) { DICE_LOG_EXCEPTION("error notification delivery"); }
        inNotify = false;
    }
    void flush_() override {}
private:
    Callback cb_;
    Clock clock_;
    FaultGate gate_;
};

}  // namespace dice::notice
