#pragma once

// Discord Bot adapter — Gateway WebSocket (v10) + REST。
// 结构与 QQOfficialAdapter 一致：REST 取网关地址 → WSS 连接 → hello/identify/
// heartbeat → MESSAGE_CREATE 转 Message；发送走 REST POST /channels/{id}/messages。
// 频道消息映射为 kGroup（targetId=channel_id，群功能按频道生效）；私信映射为
// kPrivate（targetId=用户 id，回复经 extra.channel_id 直达 DM 频道）。

#include "adapter_interface.h"
#include "qq_gateway_socket.h"
#include "../common/logger.h"

#include <drogon/HttpClient.h>

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

class DiscordAdapter final : public IAdapter,
                             public std::enable_shared_from_this<DiscordAdapter> {
public:
    explicit DiscordAdapter(std::string adapterId) : id_(std::move(adapterId)) {}
    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string platform() const override { return "discord"; }
    std::string version() const override { return "gateway-v10"; }
    bool isConnected() const override { return connected_; }
    std::string lastError() const override { return lastError_; }
    std::string getLoginId() const override { return loginId_; }
    std::string getLoginName() const override { return loginName_; }
    std::string getGroupName(const std::string&) const override { return {}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
    void onMessage(MessageCallback cb) override { messageCb_ = std::move(cb); }
    void onEvent(EventCallback cb) override { eventCb_ = std::move(cb); }

    bool configure(const json& cfg) override {
        name_ = cfg.value("name", std::string("Discord Bot"));
        token_ = cfg.value("token", std::string());
        if (token_.empty()) { lastError_ = "Discord Bot 需要 Bot Token"; return false; }
        return true;
    }

    bool start() override {
        if (connecting_ || connected_) return true;
        if (token_.empty()) { lastError_ = "Discord Bot 未配置 Token"; return false; }
        stopping_ = false;
        connecting_ = true;
        fetchGatewayUrl();
        return true;
    }

    void stop() override {
        stopping_ = true; connecting_ = false; connected_ = false;
        if (heartbeatTimer_) { drogon::app().getLoop()->invalidateTimer(*heartbeatTimer_); heartbeatTimer_.reset(); }
        if (gateway_) gateway_->stop();
        gateway_.reset();
    }

    void sendMessage(const Message& msg) override { sendTo(msg, msg.content); }
    void sendReply(const Message& original, const std::string& text) override { sendTo(original, text); }
    void sendGroupMessage(const std::string& channelId, const std::string& text) override {
        postChannelMessage(channelId, text);
    }
    void sendPrivateMessage(const std::string& userId, const std::string& text) override {
        // 需要先建 DM 频道（有缓存则直发）。
        {
            std::lock_guard lock(dmMutex_);
            auto it = dmChannels_.find(userId);
            if (it != dmChannels_.end()) { postChannelMessage(it->second, text); return; }
        }
        auto self = shared_from_this();
        restRequest(drogon::Post, "/api/v10/users/@me/channels", json{{"recipient_id", userId}},
            [self, userId, text](const json& resp) {
                const std::string channel = resp.value("id", std::string());
                if (channel.empty()) { self->lastError_ = "Discord 无法创建私信频道"; return; }
                { std::lock_guard lock(self->dmMutex_); self->dmChannels_[userId] = channel; }
                self->postChannelMessage(channel, text);
            });
    }

private:
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

    /// REST 调用（discord.com）。resp 回调只在 2xx 且 JSON 解析成功时收到对象。
    void restRequest(drogon::HttpMethod method, const std::string& path, const json& body,
                     std::function<void(const json&)> onOk) {
        const auto ip = resolveIpv4("discord.com");
        if (ip.empty()) { fail("无法解析 discord.com"); return; }
        auto client = drogon::HttpClient::newHttpClient(ip, 443, true);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        req->addHeader("Host", "discord.com");
        req->addHeader("Authorization", "Bot " + token_);
        req->addHeader("User-Agent", "DiceNext (https://github.com/DiceZone/Dice-Next, 3.0)");
        if (!body.is_null()) { req->setContentTypeCode(drogon::CT_APPLICATION_JSON); req->setBody(body.dump()); }
        auto self = shared_from_this();
        client->sendRequest(req, [self, path, onOk = std::move(onOk)](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                std::string detail = resp ? std::string(resp->body()) : std::string("网络请求未完成");
                self->lastError_ = "Discord 请求失败: " + path;
                DICE_LOG_WARN("Discord '{}': {} failed: HTTP {} {}", self->name_, path,
                              resp ? static_cast<int>(resp->statusCode()) : 0, detail);
                return;
            }
            if (!onOk) return;
            auto j = json::parse(resp->body(), nullptr, false);
            if (!j.is_discarded()) onOk(j);
        });
    }

    void fetchGatewayUrl() {
        auto self = shared_from_this();
        // GET /gateway/bot 同时校验 Token（401 = Token 无效）。
        restRequest(drogon::Get, "/api/v10/gateway/bot", json(),
            [self](const json& j) {
                if (self->stopping_) return;
                const std::string url = j.value("url", std::string("wss://gateway.discord.gg"));
                self->connectGateway(url + "/?v=10&encoding=json");
            });
    }

    void connectGateway(const std::string& url) {
        gatewayUrl_ = url;
        const std::string prefix = "wss://";
        if (url.rfind(prefix, 0) != 0) { fail("Discord Gateway 地址格式无效"); return; }
        const auto slash = url.find('/', prefix.size());
        const std::string host = url.substr(prefix.size(), slash == std::string::npos ? std::string::npos : slash - prefix.size());
        const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
        auto self = shared_from_this();
        if (gateway_) gateway_->stop();
        gateway_ = std::make_shared<QQGatewaySocket>(host, 443, path,
            [self](std::string raw) { if (!self->stopping_) self->onGateway(raw); },
            [self](const std::string& error) { if (!self->stopping_) self->fail("连接 Discord Gateway 失败：" + error); },
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

    void onGateway(const std::string& raw) {
        try {
            auto p = json::parse(raw);
            if (p.contains("s") && !p["s"].is_null()) seq_ = p["s"].get<int64_t>();
            const int op = p.value("op", -1);
            if (op == 10) {   // hello
                beginHeartbeat(p["d"].value("heartbeat_interval", 41250));
                if (!sessionId_.empty() && seq_ >= 0) resume(); else identify();
                return;
            }
            if (op == 1) {    // 服务器索要立即心跳
                if (gateway_) gateway_->sendText(json{{"op", 1}, {"d", seq_ >= 0 ? json(seq_) : json(nullptr)}}.dump());
                return;
            }
            if (op == 11) return;   // heartbeat ack
            if (op == 7) { if (gateway_) gateway_->stop(); return; }   // reconnect → onClose 走重连
            if (op == 9) { sessionId_.clear(); seq_ = -1; identify(); return; }   // invalid session
            if (op == 0) {
                const auto t = p.value("t", std::string());
                const auto d = p.value("d", json::object());
                if (t == "READY") {
                    sessionId_ = d.value("session_id", std::string());
                    const auto user = d.value("user", json::object());
                    loginId_ = user.value("id", std::string());
                    loginName_ = user.value("username", std::string());
                    connected_ = true; connecting_ = false;
                    DICE_LOG_INFO("Discord '{}': READY as {}({})", name_, loginName_, loginId_);
                } else if (t == "RESUMED") {
                    connected_ = true; connecting_ = false;
                } else if (t == "MESSAGE_CREATE") {
                    dispatchMessage(d);
                }
            }
        } catch (const std::exception& e) { DICE_LOG_WARN("Discord parse: {}", e.what()); }
    }

    void identify() {
        if (!gateway_) return;
        // GUILDS | GUILD_MESSAGES | DIRECT_MESSAGES | MESSAGE_CONTENT
        const int64_t intents = (1 << 0) | (1 << 9) | (1 << 12) | (1 << 15);
        gateway_->sendText(json{{"op", 2}, {"d", {
            {"token", token_}, {"intents", intents},
            {"properties", {{"os", "DiceNext"}, {"browser", "DiceNext"}, {"device", "DiceNext"}}},
        }}}.dump());
    }
    void resume() {
        if (gateway_) gateway_->sendText(json{{"op", 6}, {"d", {{"token", token_}, {"session_id", sessionId_}, {"seq", seq_}}}}.dump());
    }
    void beginHeartbeat(int ms) {
        if (heartbeatTimer_) drogon::app().getLoop()->invalidateTimer(*heartbeatTimer_);
        auto self = shared_from_this();
        heartbeatTimer_ = drogon::app().getLoop()->runEvery(static_cast<double>(ms > 0 ? ms : 41250) / 1000.0, [self] {
            if (self->gateway_) self->gateway_->sendText(json{{"op", 1}, {"d", self->seq_ >= 0 ? json(self->seq_) : json(nullptr)}}.dump());
        });
    }

    void dispatchMessage(const json& d) {
        const auto author = d.value("author", json::object());
        if (author.value("bot", false)) return;                      // 忽略机器人（含自己）
        if (author.value("id", std::string()) == loginId_) return;
        Message m;
        m.platform = platform(); m.adapterId = id_; m.selfId = loginId_;
        m.id = d.value("id", std::string());
        m.content = d.value("content", std::string());
        m.timestamp = std::time(nullptr);
        m.senderId = author.value("id", std::string());
        if (author.contains("global_name") && author["global_name"].is_string()) m.senderName = author["global_name"].get<std::string>();
        if (m.senderName.empty()) m.senderName = author.value("username", m.senderId);
        const auto member = d.value("member", json::object());
        if (member.is_object() && member.contains("nick") && member["nick"].is_string() && !member["nick"].get<std::string>().empty())
            m.senderName = member["nick"].get<std::string>();
        m.extra = {{"channel_id", d.value("channel_id", std::string())}};
        if (d.contains("guild_id") && d["guild_id"].is_string()) {
            // 频道消息按群处理：targetId=频道 id，群设置/人物卡按频道生效。
            m.type = MessageType::kGroup;
            m.targetId = d.value("channel_id", std::string());
            m.extra["guild_id"] = d["guild_id"];
        } else {
            m.type = MessageType::kPrivate;
            m.targetId = m.senderId;
            std::lock_guard lock(dmMutex_);
            dmChannels_[m.senderId] = d.value("channel_id", std::string());
        }
        // @ 列表 + 去掉指向本机的 <@id> 标记（否则「@bot .r」前缀不在首位）。
        if (d.contains("mentions") && d["mentions"].is_array()) {
            for (const auto& mention : d["mentions"]) {
                const std::string mid = mention.value("id", std::string());
                if (mid.empty()) continue;
                m.atList.push_back(mid);
                if (mid == loginId_) {
                    for (const std::string& tok : {"<@" + mid + ">", "<@!" + mid + ">"}) {
                        size_t pos = 0;
                        while ((pos = m.content.find(tok, pos)) != std::string::npos) m.content.erase(pos, tok.size());
                    }
                }
            }
        }
        const auto first = m.content.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) m.content.clear();
        else { const auto last = m.content.find_last_not_of(" \t\r\n"); m.content = m.content.substr(first, last - first + 1); }
        m.rawContent = m.content; m.displayContent = m.content;
        if (d.contains("attachments") && !d["attachments"].empty()) m.extra["attachments"] = d["attachments"];
        if (messageCb_) messageCb_(m);
    }

    void postChannelMessage(const std::string& channelId, const std::string& text) {
        if (channelId.empty() || text.empty()) return;
        restRequest(drogon::Post, "/api/v10/channels/" + channelId + "/messages", json{{"content", text}}, nullptr);
    }

    void sendTo(const Message& m, const std::string& text) {
        std::string channel;
        if (m.extra.is_object()) channel = m.extra.value("channel_id", std::string());
        if (channel.empty() && m.type == MessageType::kPrivate) { sendPrivateMessage(m.targetId, text); return; }
        if (channel.empty()) channel = m.targetId;   // 群/频道消息 targetId 即频道 id
        postChannelMessage(channel, text);
    }

    void fail(const std::string& e) {
        lastError_ = e; connecting_ = false; connected_ = false;
        DICE_LOG_ERROR("Discord '{}': {}", name_, e);
    }

    std::string id_, name_, token_, loginId_, loginName_, sessionId_, gatewayUrl_, lastError_;
    std::atomic<bool> connected_{false}, connecting_{false}, stopping_{false};
    int64_t seq_ = -1;
    std::shared_ptr<QQGatewaySocket> gateway_;
    std::optional<trantor::TimerId> heartbeatTimer_;
    MessageCallback messageCb_;
    EventCallback eventCb_;
    std::mutex dmMutex_;
    std::unordered_map<std::string, std::string> dmChannels_;   // userId → DM 频道 id
};

}  // namespace dice
