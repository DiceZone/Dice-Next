#pragma once

// KOOK (开黑啦) Bot adapter — Gateway WebSocket + REST v3。
// 流程：GET /api/v3/user/me 校验 Token 并取机器人身份 → GET /api/v3/gateway/index
// 取网关地址 → WSS 连接（s:1 hello → 每 30s s:2 ping / s:3 pong）→ s:0 事件转
// Message。频道消息映射 kGroup（targetId=频道 id）；私聊映射 kPrivate（用户 id）。
// 发送：POST /api/v3/message/create（频道）/ /api/v3/direct-message/create（私聊）。

#include "adapter_interface.h"
#include "qq_gateway_socket.h"
#include "../core/identity/identity_binding.h"
#include "../common/logger.h"
#include "../common/markdown.h"

#include <drogon/HttpClient.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <optional>
#include <unordered_map>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace dice {

class KookAdapter final : public IAdapter,
                          public std::enable_shared_from_this<KookAdapter> {
public:
    explicit KookAdapter(std::string adapterId) : id_(std::move(adapterId)) {}
    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string platform() const override { return "kook"; }
    std::string version() const override { return "kook-v3"; }
    bool isConnected() const override { return connected_; }
    std::string lastError() const override { return lastError_; }
    std::string getLoginId() const override { return loginId_; }
    std::string getLoginName() const override { return loginName_; }
    std::string clientId() const { return clientId_; }
    std::string getGroupName(const std::string&) const override { return {}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }

    json capabilities() const override {
        json caps = IAdapter::capabilities();
        caps["kick"] = true;   // POST /api/v3/guild/kickout
        // KOOK 无文字禁言 API（guild-mute 是语音静音）→ ban 保持 false。
        return caps;
    }

    /// 踢出服务器（频道 id → 所属 guild 经消息缓存反查）。
    void setGroupKick(const std::string& channelId, const std::string& userId) override {
        const std::string guild = guildOf(channelId);
        const std::string user = nativeId(userId, identity::Kind::User);
        if (guild.empty() || user.empty()) { lastError_ = "KOOK 无法定位服务器（需先在该频道收到过消息）"; return; }
        restRequest(drogon::Post, "/api/v3/guild/kickout",
                    json{{"guild_id", guild}, {"target_id", user}}, nullptr);
    }
    void setGroupBan(const std::string&, const std::string&, int) override {}   // KOOK 无文字禁言
    void onMessage(MessageCallback cb) override { messageCb_ = std::move(cb); }
    void onEvent(EventCallback cb) override { eventCb_ = std::move(cb); }

    bool configure(const json& cfg) override {
        name_ = cfg.value("name", std::string("KOOK Bot"));
        token_ = cfg.value("token", std::string());
        setMessageFormatOverride(parseFormatOverride(cfg.value("message_format", std::string())));
        if (token_.empty()) { lastError_ = "KOOK Bot 需要 Bot Token"; return false; }
        return true;
    }

    bool start() override {
        if (connecting_ || connected_) return true;
        if (token_.empty()) { lastError_ = "KOOK Bot 未配置 Token"; return false; }
        stopping_ = false;
        connecting_ = true;
        fetchSelf();
        return true;
    }

    void stop() override {
        stopping_ = true; connecting_ = false; connected_ = false;
        if (pingTimer_) { drogon::app().getLoop()->invalidateTimer(*pingTimer_); pingTimer_.reset(); }
        if (gateway_) gateway_->stop();
        gateway_.reset();
    }

    void sendMessage(const Message& msg) override { sendTo(msg, msg.content, ContentFormat::kPlainText); }
    void sendReply(const Message& original, const std::string& text) override {
        sendReplyFormatted(original, text, ContentFormat::kPlainText);
    }
    void sendReplyFormatted(const Message& original, const std::string& text, ContentFormat format) override {
        sendTo(original, text, format);
    }
    void sendGroupMessage(const std::string& channelId, const std::string& text) override {
        sendGroupMessageFormatted(channelId, text, ContentFormat::kPlainText);
    }
    void sendGroupMessageFormatted(const std::string& channelId, const std::string& text, ContentFormat format) override {
        const std::string native = nativeId(channelId, identity::Kind::Group);
        const std::string content = translateCQ(text);
        if (native.empty() || content.empty()) return;
        restRequest(drogon::Post, "/api/v3/message/create",
                    outboundPayload(native, content, format), nullptr);
    }
    void sendPrivateMessage(const std::string& userId, const std::string& text) override {
        sendPrivateMessageFormatted(userId, text, ContentFormat::kPlainText);
    }
    void sendPrivateMessageFormatted(const std::string& userId, const std::string& text, ContentFormat format) override {
        const std::string native = nativeId(userId, identity::Kind::User);
        const std::string content = translateCQ(text);
        if (native.empty() || content.empty()) return;
        restRequest(drogon::Post, "/api/v3/direct-message/create",
                    outboundPayload(native, content, format), nullptr);
    }

private:
    /// KOOK CardMessage is a documented rich-message type.  Keep oversized
    /// messages in the existing KMarkdown/text path so no reply is truncated.
    json outboundPayload(const std::string& target, const std::string& content, ContentFormat format) {
        const bool isMarkdown = format == ContentFormat::kMarkdown;
        if (content.size() > 5000)
            return json{{"type", 1}, {"target_id", target}, {"content", isMarkdown ? markdown::toPlainText(content) : content}};
        if (!effectiveCardMode())
            return json{{"type", 1}, {"target_id", target},
                        {"content", isMarkdown ? markdown::toPlainText(content) : content}};
        const std::string wire = isMarkdown ? content : markdown::escapeLiteral(content);
        const json card = json::array({{
            {"type", "card"}, {"theme", "primary"}, {"size", "sm"},
            {"modules", json::array({{
                {"type", "section"},
                {"text", {{"type", "kmarkdown"}, {"content", wire}}}
            }})}
        }});
        return json{{"type", 10}, {"target_id", target}, {"content", card.dump()}};
    }

    static std::string resolveIpv4(const std::string& host) {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results) return {};
        char text[INET_ADDRSTRLEN]{};
        auto* addr = reinterpret_cast<sockaddr_in*>(results->ai_addr);
        const char* result = inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
        freeaddrinfo(results);
        return result ? std::string(result) : std::string();
    }

    /// REST 调用（www.kookapp.cn）。回调只在 HTTP 2xx 且 code==0 时收到 data 对象。
    void restRequest(drogon::HttpMethod method, const std::string& path, const json& body,
                     std::function<void(const json&)> onOk) {
        const auto ip = resolveIpv4("www.kookapp.cn");
        if (ip.empty()) { fail("无法解析 www.kookapp.cn"); return; }
        auto client = drogon::HttpClient::newHttpClient(ip, 443, true);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        req->addHeader("Host", "www.kookapp.cn");
        req->addHeader("Authorization", "Bot " + token_);
        if (!body.is_null()) { req->setContentTypeCode(drogon::CT_APPLICATION_JSON); req->setBody(body.dump()); }
        auto self = shared_from_this();
        client->sendRequest(req, [self, path, onOk = std::move(onOk)](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                self->lastError_ = "KOOK 请求失败: " + path;
                self->connecting_ = false;
                DICE_LOG_WARN("KOOK '{}': {} failed: HTTP {} {}", self->name_, path,
                              resp ? static_cast<int>(resp->statusCode()) : 0,
                              resp ? std::string(resp->body()) : std::string("网络请求未完成"));
                return;
            }
            auto j = json::parse(resp->body(), nullptr, false);
            if (j.is_discarded()) return;
            if (j.value("code", -1) != 0) {
                self->lastError_ = "KOOK: " + j.value("message", std::string("请求被拒绝"));
                DICE_LOG_WARN("KOOK '{}': {} code={} {}", self->name_, path, j.value("code", -1), j.value("message", std::string()));
                return;
            }
            if (onOk) onOk(j.value("data", json::object()));
        }, 15.0);
    }

    void fetchSelf() {
        auto self = shared_from_this();
        restRequest(drogon::Get, "/api/v3/user/me", json(), [self](const json& d) {
            if (self->stopping_) return;
            self->loginId_ = d.value("id", std::string());
            self->loginName_ = d.value("username", std::string());
            self->clientId_ = d.value("client_id", std::string());
            self->fetchGatewayUrl();
        });
    }

    void fetchGatewayUrl() {
        auto self = shared_from_this();
        restRequest(drogon::Get, "/api/v3/gateway/index?compress=0", json(), [self](const json& d) {
            if (self->stopping_) return;
            const std::string url = d.value("url", std::string());
            if (url.empty()) { self->fail("KOOK 未返回网关地址"); return; }
            self->connectGateway(url);
        });
    }

    void connectGateway(const std::string& url) {
        const std::string prefix = "wss://";
        if (url.rfind(prefix, 0) != 0) { fail("KOOK 网关地址格式无效"); return; }
        const auto slash = url.find('/', prefix.size());
        const std::string host = url.substr(prefix.size(), slash == std::string::npos ? std::string::npos : slash - prefix.size());
        const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
        auto self = shared_from_this();
        if (gateway_) gateway_->stop();
        gateway_ = std::make_shared<QQGatewaySocket>(host, 443, path,
            [self](std::string raw) { if (!self->stopping_) self->onGateway(raw); },
            [self](const std::string& error) { if (!self->stopping_) self->fail("连接 KOOK 网关失败：" + error); },
            [self] { if (!self->stopping_) { self->connected_ = false; self->connecting_ = false; self->scheduleReconnect(); } });
        gateway_->start();
    }

    void scheduleReconnect() {
        if (stopping_) return;
        connecting_ = true;
        auto self = shared_from_this();
        drogon::app().getLoop()->runAfter(5.0, [self] {
            if (!self->stopping_) self->fetchGatewayUrl();
        });
    }

    /// 取对象字段；字段缺失或为 null/非对象时回退空对象（KOOK 的 extra 可为 null）。
    static json objOf(const json& j, const char* key) {
        if (!j.is_object()) return json::object();
        auto it = j.find(key);
        return (it != j.end() && it->is_object()) ? *it : json::object();
    }

    void onGateway(const std::string& raw) {
        try {
            auto p = json::parse(raw);
            const int s = p.value("s", -1);
            if (p.contains("sn") && p["sn"].is_number_integer()) sn_ = p["sn"].get<int64_t>();
            if (s == 1) {          // hello
                const auto d = objOf(p, "d");
                if (d.value("code", 0) != 0) { fail("KOOK 网关 hello 失败 code=" + std::to_string(d.value("code", 0))); return; }
                sessionId_ = d.value("session_id", std::string());
                connected_ = true; connecting_ = false;
                beginPing();
                DICE_LOG_INFO("KOOK '{}': connected as {}({})", name_, loginName_, loginId_);
            } else if (s == 3) {   // pong
            } else if (s == 5) {   // reconnect 指令：按协议丢弃 sn 重新拉取网关
                sn_ = 0; sessionId_.clear();
                if (gateway_) gateway_->stop();
            } else if (s == 0) {   // event
                dispatchMessage(objOf(p, "d"));
            }
        } catch (const std::exception& e) { DICE_LOG_WARN("KOOK parse: {}", e.what()); }
    }

    void beginPing() {
        if (pingTimer_) drogon::app().getLoop()->invalidateTimer(*pingTimer_);
        auto self = shared_from_this();
        pingTimer_ = drogon::app().getLoop()->runEvery(30.0, [self] {
            if (self->gateway_) self->gateway_->sendText(json{{"s", 2}, {"sn", self->sn_}}.dump());
        });
    }

    void dispatchMessage(const json& d) {
        const int type = d.value("type", 0);
        if (type != 1 && type != 9) return;                          // 仅文本 / KMarkdown
        const std::string channelType = d.value("channel_type", std::string());
        if (channelType != "GROUP" && channelType != "PERSON") return;
        const std::string authorId = d.value("author_id", std::string());
        if (authorId.empty() || authorId == loginId_ || authorId == "1") return;   // 忽略自己与系统
        const auto extra = objOf(d, "extra");     // PERSON 消息的 extra 可为 null
        const auto author = objOf(extra, "author");
        if (author.value("bot", false)) return;

        Message m;
        m.platform = platform(); m.adapterId = id_; m.selfId = loginId_;
        m.id = d.value("msg_id", std::string());
        m.content = d.value("content", std::string());
        m.timestamp = std::time(nullptr);
        m.senderId = authorId;
        m.senderName = author.value("nickname", std::string());
        if (m.senderName.empty()) m.senderName = author.value("username", authorId);
        if (channelType == "GROUP") {
            m.type = MessageType::kGroup;
            m.targetId = d.value("target_id", std::string());        // 频道 id
            if (extra.contains("guild_id") && extra["guild_id"].is_string()) {
                m.extra["guild_id"] = extra["guild_id"];
                std::lock_guard lock(guildMutex_);
                channelGuild_[m.targetId] = extra["guild_id"].get<std::string>();
            }
        } else {
            m.type = MessageType::kPrivate;
            m.targetId = authorId;
        }
        // @ 列表 + 去掉指向本机的 (met)id(met) 标记（KMarkdown @ 语法）。
        if (extra.contains("mention") && extra["mention"].is_array()) {
            for (const auto& mid : extra["mention"])
                if (mid.is_string()) m.atList.push_back(mid.get<std::string>());
        }
        if (extra.value("mention_all", false)) m.atList.push_back("all");
        if (!loginId_.empty()) {
            const std::string tok = "(met)" + loginId_ + "(met)";
            size_t pos = 0;
            bool mentionedSelf = false;
            while ((pos = m.content.find(tok, pos)) != std::string::npos) { m.content.erase(pos, tok.size()); mentionedSelf = true; }
            if (mentionedSelf && std::find(m.atList.begin(), m.atList.end(), loginId_) == m.atList.end())
                m.atList.push_back(loginId_);
        }
        const auto first = m.content.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) m.content.clear();
        else { const auto last = m.content.find_last_not_of(" \t\r\n"); m.content = m.content.substr(first, last - first + 1); }
        m.rawContent = m.content; m.displayContent = m.content;
        if (messageCb_) messageCb_(m);
    }

    /// 频道 → 所属服务器（公共群号先转原生频道号再查缓存）。
    std::string guildOf(const std::string& channelPublicId) {
        const std::string channel = nativeId(channelPublicId, identity::Kind::Group);
        std::lock_guard lock(guildMutex_);
        auto it = channelGuild_.find(channel);
        return it != channelGuild_.end() ? it->second : std::string();
    }

    /// 出站 CQ 码转 KOOK 原生：[CQ:at,qq=公共号] → (met)原生id(met)；图片 URL 直贴；
    /// 其余 CQ 段丢弃，避免把 [CQ:...] 原文发给用户。
    std::string translateCQ(const std::string& text) {
        std::string out; out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            if (text.compare(i, 4, "[CQ:") != 0) { out += text[i++]; continue; }
            const auto end = text.find(']', i);
            if (end == std::string::npos) { out += text.substr(i); break; }
            const std::string seg = text.substr(i + 4, end - i - 4);
            i = end + 1;
            const auto comma = seg.find(',');
            const std::string type = seg.substr(0, comma == std::string::npos ? seg.size() : comma);
            auto param = [&seg](const std::string& key) -> std::string {
                const auto p = seg.find("," + key + "=");
                if (p == std::string::npos) return {};
                const auto v = p + key.size() + 2;
                const auto e = seg.find(',', v);
                return seg.substr(v, e == std::string::npos ? std::string::npos : e - v);
            };
            if (type == "at") {
                const std::string qq = param("qq");
                if (qq == "all") out += "(met)all(met)";
                else if (!qq.empty()) out += "(met)" + nativeId(qq, identity::Kind::User) + "(met)";
            } else if (type == "image") {
                const std::string f = param("file"), u = param("url");
                const std::string& link = u.rfind("http", 0) == 0 ? u : f;
                if (link.rfind("http", 0) == 0) out += " " + link + " ";
            }
        }
        return out;
    }

    /// 公共号（虚拟/真实 QQ）→ KOOK 原生 id。非映射产物原样返回。
    std::string nativeId(const std::string& publicId, identity::Kind kind) {
        if (publicId.empty() || !db_) return publicId;
        auto native = identity::BindingStore::instance().transportEndpoint(*db_, "kook", publicId, kind);
        return native.empty() ? publicId : native;
    }

    void sendTo(const Message& m, const std::string& text, ContentFormat format) {
        // 回复优先走入站带回的原生 id（免查表）。
        std::string native;
        if (m.extra.is_object()) native = m.extra.value("__identity_native_target", std::string());
        const std::string content = translateCQ(text);
        if (content.empty()) return;
        if (m.type == MessageType::kPrivate) {
            if (!native.empty()) restRequest(drogon::Post, "/api/v3/direct-message/create",
                                             outboundPayload(native, content, format), nullptr);
            else sendPrivateMessageFormatted(m.targetId, text, format);
        } else {
            if (!native.empty()) restRequest(drogon::Post, "/api/v3/message/create",
                                             outboundPayload(native, content, format), nullptr);
            else sendGroupMessageFormatted(m.targetId, text, format);
        }
    }

    void fail(const std::string& e) {
        lastError_ = e; connecting_ = false; connected_ = false;
        DICE_LOG_ERROR("KOOK '{}': {}", name_, e);
    }

    std::string id_, name_, token_, loginId_, loginName_, clientId_, sessionId_, lastError_;
    Database* db_{identity::BindingStore::instance().database()};
    std::mutex guildMutex_;
    std::unordered_map<std::string, std::string> channelGuild_;   // 频道 id → guild id
    std::atomic<bool> connected_{false}, connecting_{false}, stopping_{false};
    int64_t sn_ = 0;
    std::shared_ptr<QQGatewaySocket> gateway_;
    std::optional<trantor::TimerId> pingTimer_;
    MessageCallback messageCb_;
    EventCallback eventCb_;
};

}  // namespace dice
