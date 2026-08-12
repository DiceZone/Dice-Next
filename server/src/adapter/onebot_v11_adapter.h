#pragma once

#include "../core/identity/identity_binding.h"
// ─── Dice!Next v3.0.0 — OneBot v11 Adapter ───────────────────
// Implements the OneBot v11 protocol over WebSocket.
//
// Supports:
//   - Forward WS: Dice!Next connects to OneBot client (NapCat/LLOneBot/Lagrange)
//   - Reverse WS: OneBot client connects to Dice!Next (TODO)
//
// OneBot v11 Protocol Reference:
//   https://github.com/botuniverse/onebot-11

#include "adapter_interface.h"
#include "../common/logger.h"
#include "../common/markdown.h"
#include "../service/image_send.h"   // 发送期图片码解析
#include "self_echo_filter.h"        // 自回声去重 + 跨骰防环护栏

#include <drogon/WebSocketClient.h>
#include <drogon/HttpAppFramework.h>  // for drogon::app()
#include <drogon/utils/Utilities.h>   // for base64Encode (group-file upload)
#include <trantor/net/EventLoop.h>
#include <trantor/net/TcpServer.h>
#include <trantor/net/InetAddress.h>

#include <atomic>
#include <queue>
#include <set>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <future>
#include <chrono>

namespace dice {

class OneBotV11Adapter : public IAdapter,
                         public std::enable_shared_from_this<OneBotV11Adapter> {
public:
    OneBotV11Adapter(const std::string& adapterId)
        : id_(adapterId), name_(adapterId)
    {}

    // ─── IAdapter Interface ──────────────────────────────────

    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "1.0.0"; }

    json capabilities() const override {
        return {
            {"friends", true}, {"friend_delete", true}, {"kick", true}, {"ban", true}, {"poke", true},
            {"forward", true}, {"group_file", true}, {"group_card", true},
            {"member_list", true}, {"group_leave", true},
        };
    }

    bool configure(const json& config) override {
        name_   = config.value("name", id_);
        endpoint_ = config.value("endpoint", "ws://localhost:6700");
        // Strip a trailing slash so newWebSocketClient gets a clean scheme://host:port.
        while (!endpoint_.empty() && endpoint_.back() == '/') endpoint_.pop_back();
        accessToken_ = config.value("accessToken", "");
        if (config.contains("connectionMode"))
            mode_ = config["connectionMode"].get<std::string>();
        setMessageFormatOverride(parseFormatOverride(config.value("message_format", std::string())));
        DICE_LOG_INFO("OneBotV11 '{}': configured endpoint={} mode={}", name_, endpoint_, mode_);
        return true;
    }

    bool start() override {
        if (isConnected()) return true;
        stopping_ = false;

        // ── Forward WS: Dice connects to OneBot server ──────────
        if (mode_ == "forward_ws") {
            return startForwardWs();
        }

        // ── Reverse WS: OneBot client connects to Dice ──────────
        if (mode_ == "reverse_ws") {
            return startReverseWs();
        }

        lastError_ = "connection mode '" + mode_ + "' not implemented yet (use forward_ws or reverse_ws)";
        DICE_LOG_ERROR("OneBotV11 '{}': {}", name_, lastError_);
        return false;
    }

    void stop() override {
        stopping_ = true;
        if (tcpServer_) { tcpServer_->stop(); tcpServer_.reset(); }
        if (revLoop_) { revLoop_->getLoop()->quit(); revLoop_.reset(); }
        if (wsClient_) { wsClient_->stop(); wsClient_.reset(); }
        connected_ = false;
        DICE_LOG_INFO("OneBotV11 '{}': stopped", name_);
    }

    bool isConnected() const override { return connected_; }
    std::string lastError() const override { return lastError_; }

    /// fine-grained link state. "connected" / "timeout"（重连20次失败已暂停）/ "disconnected".
    /// "timeout" 用来和「已禁用」区分开：适配器仍是 enabled，只是暂停自动重连，可手动重连。
    std::string connectionStatus() const override {
        if (connected_) return "connected";
        if (timedOut_)  return "timeout";
        return "disconnected";
    }

    /// 手动重连——清零退避计数与超时标记，重新开始连接。
    /// （自动重连走 scheduleReconnect 不清零，否则退避失效；只有手动 resume 才清零。）
    void resumeConnection() override {
        reconnectAttempts_ = 0;
        timedOut_ = false;
        lastError_.clear();
        if (!connected_) start();
    }

    // ─── Send Messages ───────────────────────────────────────

    // 跨骰：出站预处理——跨骰护栏（开头是指令→插零宽空格）+ 登记自回声去重。
    std::string prepOutgoing(const std::string& target, const std::string& text) {
        // Markdown is the platform-neutral outbound format. OneBot v11 has no
        // Markdown message type, so only its adapter copy is downgraded. CQ and
        // image codes are protected by the converter and parsed afterwards.
        std::string out = guardCrossBot(markdown::toPlainText(text));
        SelfEchoFilter::instance().mark(platform() + ":" + target, normalizeEcho(out));
        return out;
    }

    void sendMessage(const Message& msg) override {
        json payload;
        std::string action;

        // Use OneBot array message format; parse any embedded image/at/CQ codes.
        json messageArray = buildSegments(prepOutgoing(msg.targetId, msg.content));

        switch (msg.type) {
        case MessageType::kPrivate:
            payload = {{"user_id", parseId(msg.targetId)}, {"message", messageArray}};
            action = "send_private_msg";
            break;
        case MessageType::kGroup:
            payload = {{"group_id", parseId(msg.targetId)}, {"message", messageArray}};
            action = "send_group_msg";
            break;
        case MessageType::kChannel:
            payload = {{"guild_id", msg.extra.value("guild_id", msg.targetId)},
                       {"channel_id", msg.targetId},
                       {"message", messageArray}};
            action = "send_guild_channel_msg";
            break;
        }

        sendOneBotAction(action, payload);
    }

    void sendReply(const Message& original, const std::string& replyText) override {
        // Build an array-format reply, quoting the original message when possible.
        json messageArray = json::array();
        if (!original.id.empty()) {
            messageArray.push_back({{"type","reply"},{"data",json{{"id",original.id}}}});
        }
        for (auto& seg : buildSegments(prepOutgoing(original.targetId, replyText))) messageArray.push_back(seg);

        // Route to the SAME conversation the message came from — do not
        // broadcast to both group and private (that was a bug).
        switch (original.type) {
        case MessageType::kPrivate:
            sendOneBotAction("send_private_msg", {
                {"user_id", parseId(original.targetId)},
                {"message", messageArray}
            });
            break;
        case MessageType::kChannel:
            sendOneBotAction("send_guild_channel_msg", {
                {"guild_id", original.extra.value("guild_id", original.targetId)},
                {"channel_id", original.targetId},
                {"message", messageArray}
            });
            break;
        case MessageType::kGroup:
        default:
            sendOneBotAction("send_group_msg", {
                {"group_id", parseId(original.targetId)},
                {"message", messageArray}
            });
            break;
        }
    }

    // ─── Event Handlers ──────────────────────────────────────

    void onMessage(MessageCallback cb) override {
        messageCallbacks_.push_back(std::move(cb));
    }

    void onEvent(EventCallback cb) override {
        eventCallbacks_.push_back(std::move(cb));
    }

    void setFriendRequest(const std::string& flag, bool approve,
                          const std::string& remark = "") override {
        sendOneBotAction("set_friend_add_request",
            {{"flag", flag}, {"approve", approve}, {"remark", remark}});
    }
    void setGroupRequest(const std::string& flag, const std::string& subType,
                         bool approve, const std::string& reason = "") override {
        sendOneBotAction("set_group_add_request",
            {{"flag", flag}, {"sub_type", subType}, {"approve", approve}, {"reason", reason}});
    }

    // ─── Platform Operations ─────────────────────────────────

    std::string getLoginId() const override { return loginId_; }
    std::string getLoginName() const override { return loginName_; }

    std::string getGroupName(const std::string& groupId) const override {
        return groupNames_.count(groupId) ? groupNames_.at(groupId) : groupId;
    }

    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string& groupId, const std::string& userId) const override {
        const std::string role = memberRole(groupId, userId);
        return role == "admin" || role == "owner";
    }
    bool isGroupOwner(const std::string& groupId, const std::string& userId) const override {
        return memberRole(groupId, userId) == "owner";
    }
    /// Ask the platform for ONE member's role when the message did not carry
    /// `sender.role`.  Rate-limited per group+user so permission checks on
    /// every command cannot spam the QQ client.
    void refreshMemberRole(const std::string& groupId, const std::string& userId) override {
        if (groupId.empty() || userId.empty()) return;
        {
            std::lock_guard<std::mutex> lk(dataMutex_);
            const auto now = std::chrono::steady_clock::now();
            const std::string key = groupId + "\x1f" + userId;
            auto it = roleRefreshAt_.find(key);
            if (it != roleRefreshAt_.end() && now - it->second < std::chrono::seconds(20)) return;
            roleRefreshAt_[key] = now;
        }
        sendOneBotAction("get_group_member_info",
            {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}},
            "role:" + groupId + ":" + userId);
    }
    void setGroupKick(const std::string& groupId, const std::string& userId) override {
        DICE_LOG_INFO("OneBotV11 '{}': set_group_kick group={} user={}", name_, groupId, userId);
        sendOneBotAction("set_group_kick", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}});
    }
    void setGroupBan(const std::string& groupId, const std::string& userId, int durationSec) override {
        DICE_LOG_INFO("OneBotV11 '{}': set_group_ban group={} user={} duration={}s{}",
                      name_, groupId, userId, durationSec, durationSec == 0 ? " (解禁)" : "");
        sendOneBotAction("set_group_ban", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}, {"duration", durationSec}});
    }
    void setGroupSpecialTitle(const std::string& groupId, const std::string& userId,
                              const std::string& title) override {
        sendOneBotAction("set_group_special_title", {{"group_id", parseId(groupId)},
            {"user_id", parseId(userId)}, {"special_title", title}});
    }

    // ─── Joined-group / member caches (web group-management) ──
    void refreshGroupList() override {
        sendOneBotAction("get_group_list", json::object(), "glist");
        sendOneBotAction("get_friend_list", json::object(), "flist");   // 好友数量
    }
    // 主动反查群名。snowluma 等协议端 get_group_list 不带 group_name 时，
    // 逐群 get_group_info 补齐。**非阻塞**——结果经 "ginfo:<gid>" echo 回流缓存；
    // 不能在 WS 接收线程同步等 invokeAction（事件回调就跑在该线程，会自锁 8s）。
    // ginfoInflight_ 去重，避免重复请求。
    void requestGroupInfo(const std::string& gid) {
        if (gid.empty() || !connected_) return;
        {
            std::lock_guard<std::mutex> lk(dataMutex_);
            if (!ginfoInflight_.insert(gid).second) return;   // 已在查
        }
        sendOneBotAction("get_group_info", {{"group_id", parseId(gid)}, {"no_cache", false}}, "ginfo:" + gid);
    }
    std::vector<std::pair<std::string, std::string>> getGroupList() const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        std::vector<std::pair<std::string, std::string>> v;
        for (auto& [k, n] : groupList_) v.push_back({k, n});
        return v;
    }
    int getFriendCount() const override { std::lock_guard<std::mutex> lk(dataMutex_); return friendCount_; }
    std::vector<std::string> getFriendList() const override {
        std::lock_guard<std::mutex> lk(dataMutex_); return friendList_;
    }

    /// 删除好友（NapCat delete_friend）。
    void deleteFriend(const std::string& userId) override {
        sendOneBotAction("delete_friend", {{"user_id", parseId(userId)}});
        DICE_LOG_INFO("OneBotV11 '{}': delete_friend {}", name_, userId);
    }

    /// 同步调用 OneBot API，等待 echo 响应（群文件列表/下载链等）。
    /// ⚠️ 不得在适配器接收线程调用（等待的响应正是该线程投递的 → 死锁）；
    /// WebUI 的 drogon 处理线程使用没问题。超时/未连接返回 null。
    json invokeAction(const std::string& action, const json& params, int timeoutMs = 8000) override {
        if (!connected_) return json();
        static std::atomic<long> s_invokeSeq{0};
        const std::string echo = "invoke:" + id_ + ":" + std::to_string(++s_invokeSeq);
        auto pr = std::make_shared<std::promise<json>>();
        {
            std::lock_guard<std::mutex> lk(invokeMutex_);
            pendingInvokes_[echo] = pr;
        }
        sendOneBotAction(action, params, echo);
        json out;
        auto fut = pr->get_future();
        if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
            try { out = fut.get(); } catch (...) {}
        } else {
            DICE_LOG_WARN("OneBotV11 '{}': invokeAction '{}' timed out after {}ms", name_, action, timeoutMs);
        }
        {
            std::lock_guard<std::mutex> lk(invokeMutex_);
            pendingInvokes_.erase(echo);
        }
        return out;
    }

    /// 戳一戳。优先 NapCat 扩展 API group_poke；老实现(go-cqhttp)走 [CQ:poke] 消息段。
    void sendGroupPoke(const std::string& groupId, const std::string& userId) override {
        sendOneBotAction("group_poke", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}});
    }

    /// NapCat/go-cqhttp get_group_msg_history —— 结果经 kGroupHistory 事件回流。
    void requestGroupHistory(const std::string& groupId, int count) override {
        json p{{"group_id", parseId(groupId)}};
        if (count > 0) p["count"] = count;   // NapCat 支持 count；不支持的实现忽略该参数
        sendOneBotAction("get_group_msg_history", p, "history:" + groupId);
    }
    void refreshMembers(const std::string& groupId) override {
        sendOneBotAction("get_group_member_list", {{"group_id", parseId(groupId)}}, "members:" + groupId);
        if (!loginId_.empty())
            sendOneBotAction("get_group_member_info",
                {{"group_id", parseId(groupId)}, {"user_id", parseId(loginId_)}},
                "role:" + groupId + ":" + loginId_);
    }
    int getGroupMemberCount(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        auto it = groupCount_.find(groupId);
        return it != groupCount_.end() ? it->second : 0;
    }
    std::string getSelfRole(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        auto it = selfRole_.find(groupId);
        return it != selfRole_.end() ? it->second : "";
    }
    void refreshSelfRole(const std::string& groupId) override {
        if (loginId_.empty()) return;
        sendOneBotAction("get_group_member_info",
            {{"group_id", parseId(groupId)}, {"user_id", parseId(loginId_)}},
            "role:" + groupId + ":" + loginId_);
    }
    json getMembers(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(dataMutex_);
        auto it = memberLists_.find(groupId);
        return it != memberLists_.end() ? it->second : json::array();
    }

    void setGroupCard(const std::string& groupId, const std::string& userId,
                      const std::string& card) override {
        if (parseId(userId) == 0) {
            DICE_LOG_WARN("OneBotV11 '{}': set_group_card skipped — empty/invalid user_id (login not ready?)", name_);
            return;
        }
        DICE_LOG_INFO("OneBotV11 '{}': set_group_card group={} user={} card='{}'", name_, groupId, userId, card);
        sendOneBotAction("set_group_card", {{"group_id", parseId(groupId)},
            {"user_id", parseId(userId)}, {"card", card}});
    }
    void setGroupName(const std::string& groupId, const std::string& name) override {
        DICE_LOG_INFO("OneBotV11 '{}': set_group_name group={} name='{}'", name_, groupId, name);
        sendOneBotAction("set_group_name", {{"group_id", parseId(groupId)}, {"group_name", name}});
    }
    void sendGroupMessage(const std::string& groupId, const std::string& text) override {
        sendOneBotAction("send_group_msg", {{"group_id", parseId(groupId)}, {"message", buildSegments(prepOutgoing(groupId, text))}});
    }
    /// Send to a group as a STRING message so the platform parses CQ codes
    /// ([CQ:at,qq=..] / [CQ:image,..]). auto_escape=false keeps them as codes.
    void sendGroupMessageCQ(const std::string& groupId, const std::string& cqText) override {
        sendOneBotAction("send_group_msg",
            {{"group_id", parseId(groupId)}, {"message", normalizeCQText(prepOutgoing(groupId, cqText))}, {"auto_escape", false}});
    }
    void sendPrivateMessage(const std::string& userId, const std::string& text) override {
        sendOneBotAction("send_private_msg", {{"user_id", parseId(userId)}, {"message", buildSegments(prepOutgoing(userId, text))}});
    }
    /// Send a 合并转发 (merged-forward / chat-record) message: each node is one bubble,
    /// attributed to the bot. Uses the OneBot `send_group_forward_msg` custom-node API.
    bool sendGroupForwardMsg(const std::string& groupId,
                             const std::vector<std::string>& nodes) override {
        if (nodes.empty()) return false;
        std::string botName = loginName_.empty() ? std::string("\xe9\xaa\xb0\xe5\xa8\x98") : loginName_;  // 骰娘
        std::string uin = loginId_.empty() ? std::string("10000") : loginId_;
        json messages = json::array();
        for (const auto& n : nodes) {
            const std::string plain = prepOutgoing(groupId, n);
            messages.push_back({
                {"type", "node"},
                {"data", {{"name", botName}, {"uin", uin}, {"content", buildSegments(plain)}}}
            });
        }
        sendOneBotAction("send_group_forward_msg",
            {{"group_id", parseId(groupId)}, {"messages", messages}});
        return true;
    }
    void uploadGroupFile(const std::string& groupId, const std::string& name,
                         const std::string& content, const std::string& localPath = "") override {
        // Try the local path FIRST (cheap, no payload bloat — works when OneBot
        // shares our filesystem / runs on the same host). If that fails (remote /
        // containerized OneBot can't stat it → the ENOENT retcode=1200), the echo
        // handler resends the content as base64:// (no shared FS needed).
        if (!localPath.empty()) {
            std::string echo = "ufup:" + std::to_string(++echoSeq_);
            {
                std::lock_guard<std::mutex> lk(dataMutex_);
                pendingUploads_[echo] = PendingUpload{groupId, name, content};
            }
            sendOneBotAction("upload_group_file",
                {{"group_id", parseId(groupId)}, {"file", localPath}, {"name", name}}, echo);
            return;
        }
        // No local copy → go straight to base64.
        uploadGroupFileBase64(groupId, name, content);
    }

    /// Send a group file as base64:// (the cross-host path). Used as the fallback
    /// after a local-path attempt fails, or directly when no on-disk copy exists.
    void uploadGroupFileBase64(const std::string& groupId, const std::string& name,
                               const std::string& content) {
        if (content.empty()) return;
        std::string b64 = drogon::utils::base64Encode(
            reinterpret_cast<const unsigned char*>(content.data()), content.size());
        sendOneBotAction("upload_group_file",
            {{"group_id", parseId(groupId)}, {"file", "base64://" + b64}, {"name", name}},
            "ufupb");  // base64 attempt; failure here is logged by the generic handler
    }

    /// Leave the group, deferred ~1.5s so the .dismiss reply is delivered first.
    void leaveGroup(const std::string& groupId) override {
        auto self = shared_from_this();
        int64_t gid = parseId(groupId);
        drogon::app().getLoop()->runAfter(1.5, [self, gid]() {
            self->sendOneBotAction("set_group_leave", {{"group_id", gid}});
            DICE_LOG_INFO("OneBotV11 '{}': left group {}", self->name_, gid);
        });
    }

private:
    // ─── Forward WS: Dice connects to OneBot client ────────────
    bool startForwardWs() {
        wsClient_ = drogon::WebSocketClient::newWebSocketClient(endpoint_);
        if (!wsClient_) { lastError_ = "Failed to create WS client"; return false; }
        auto self = shared_from_this();
        wsClient_->setMessageHandler([self](std::string&& raw, const drogon::WebSocketClientPtr&, const drogon::WebSocketMessageType& type) {
            if (type == drogon::WebSocketMessageType::Text || type == drogon::WebSocketMessageType::Binary)
                self->onRawMessage(std::move(raw));
        });
        wsClient_->setConnectionClosedHandler([self](const drogon::WebSocketClientPtr&) {
            self->connected_ = false;
            DICE_LOG_WARN("OneBotV11 '{}': connection closed", self->name_);
            self->scheduleReconnect();
        });
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/");
        if (!accessToken_.empty()) req->addHeader("Authorization", "Bearer " + accessToken_);
        DICE_LOG_INFO("OneBotV11 '{}': connecting to {} ...", name_, endpoint_);
        wsClient_->connectToServer(req, [self](drogon::ReqResult r, const drogon::HttpResponsePtr&, const drogon::WebSocketClientPtr&) {
            if (r != drogon::ReqResult::Ok) { self->connected_=false; self->lastError_="WS connect failed"; self->scheduleReconnect(); }
            else { self->connected_=true; self->lastError_.clear();  self->timedOut_=false; DICE_LOG_INFO("OneBotV11 '{}': connected", self->name_); self->sendOneBotAction("get_login_info", json::object()); self->refreshGroupList(); self->startGroupRefreshTimer(); }
                    // after stable connection, reset reconnect counter
                    if (self->stabilityTimer_) self->stabilityTimer_.reset();
                    self->stabilityTimer_ = std::make_shared<trantor::TimerId>();
                    auto weak = std::weak_ptr<OneBotV11Adapter>(self);
                    *self->stabilityTimer_ = drogon::app().getLoop()->runAfter(kStableAfterSec, [weak]() {
                        if (auto s = weak.lock()) { s->reconnectAttempts_ = 0; s->stabilityTimer_.reset(); }
                    });
        });
        return true;
    }

    // ─── Reverse WS: OneBot client connects to Dice ───────────
    bool startReverseWs() {
        int port = 6700;
        try { port = std::stoi(endpoint_); } catch (...) { lastError_ = "Invalid port: " + endpoint_; return false; }
        auto self = shared_from_this();
        revLoop_ = std::make_shared<trantor::EventLoopThread>();
        revLoop_->run();
        tcpServer_ = std::make_shared<trantor::TcpServer>(revLoop_->getLoop(),
            trantor::InetAddress(static_cast<uint16_t>(port)), "revws-" + id_);
        tcpServer_->setRecvMessageCallback([self](const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) {
            self->onRevWsData(conn, buf);
        });
        tcpServer_->setConnectionCallback([self](const trantor::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                // Accept exactly ONE primary connection. Any extra connection — a
                // second OneBot client, or a stray HTTP/browser probe — is closed
                // immediately and never becomes the primary, so probing the port
                // cannot disturb the live OneBot link.
                // IMPORTANT: never call forceClose() while holding revConnMutex_ —
                // trantor may invoke the disconnect callback synchronously, which
                // re-locks the same (non-recursive) mutex → undefined behavior.
                bool reject = false;
                {
                    std::lock_guard lk(self->revConnMutex_);
                    if (self->revConn_) reject = true;
                    else { self->revConn_ = conn; self->revBuffer_.clear(); self->revFrag_.clear(); }
                }
                if (reject) {
                    DICE_LOG_WARN("OneBotV11 '{}': rejecting extra connection on reverse-WS port", self->name_);
                    conn->forceClose();
                }
            } else {
                // Only react when the PRIMARY connection goes away.
                std::lock_guard lk(self->revConnMutex_);
                if (self->revConn_ == conn) {
                    self->revConn_.reset();
                    if (self->connected_) {
                        self->connected_ = false;
                        DICE_LOG_WARN("OneBotV11 '{}': reverse WS client disconnected", self->name_);
                    }
                }
            }
        });
        // TcpServer::start() MUST run on its own event-loop thread, otherwise
        // trantor aborts ("forbidden to run loop on threads other than event-loop
        // thread"). startAll() runs on the main loop, so dispatch into revLoop_.
        revLoop_->getLoop()->runInLoop([self]() { self->tcpServer_->start(); });
        DICE_LOG_INFO("OneBotV11 '{}': listening on port {} for reverse WS", name_, port);
        return true;
    }

    // Handle data from the reverse WS TCP connection (raw, includes handshake)
    static std::string wsAcceptKey(const std::string& key) {
        // WebSocket accept key = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
        // Use embedded SHA1 implementation to avoid OpenSSL linking
        static const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string combined = key + magic;
        
        // Simple SHA1 implementation (public domain)
        uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
        std::vector<uint8_t> msg(combined.begin(), combined.end());
        uint64_t ml = msg.size() * 8;
        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back((ml >> (i*8)) & 0xFF);
        
        for (size_t i = 0; i < msg.size(); i += 64) {
            uint32_t w[80];
            for (int j = 0; j < 16; ++j) w[j] = (msg[i+j*4]<<24)|(msg[i+j*4+1]<<16)|(msg[i+j*4+2]<<8)|msg[i+j*4+3];
            for (int j = 16; j < 80; ++j) { uint32_t t=w[j-3]^w[j-8]^w[j-14]^w[j-16]; w[j]=(t<<1)|(t>>31); }
            uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
            for (int j=0;j<80;++j){uint32_t f,k;if(j<20){f=(b&c)|(~b&d);k=0x5A827999;}else if(j<40){f=b^c^d;k=0x6ED9EBA1;}else if(j<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}else{f=b^c^d;k=0xCA62C1D6;}uint32_t t=((a<<5)|(a>>27))+f+e+k+w[j];e=d;d=c;c=(b<<30)|(b>>2);b=a;a=t;}
            h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
        }
        unsigned char hash[20];
        for (int i=0;i<4;i++){hash[i]=(h0>>(24-8*i))&0xFF;hash[i+4]=(h1>>(24-8*i))&0xFF;hash[i+8]=(h2>>(24-8*i))&0xFF;hash[i+12]=(h3>>(24-8*i))&0xFF;hash[i+16]=(h4>>(24-8*i))&0xFF;}
        
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int i = 0;
        for (; i + 3 <= 20; i += 3) {            // 6 full groups → 18 bytes / 24 chars
            uint32_t v = (hash[i]<<16)|(hash[i+1]<<8)|hash[i+2];
            result += b64[(v>>18)&0x3F]; result += b64[(v>>12)&0x3F];
            result += b64[(v>>6)&0x3F];  result += b64[v&0x3F];
        }
        // 20 % 3 == 2 remaining bytes → 3 chars + one '=' pad (SHA1 is always 20 bytes)
        uint32_t v = (hash[i]<<16)|(hash[i+1]<<8);
        result += b64[(v>>18)&0x3F]; result += b64[(v>>12)&0x3F];
        result += b64[(v>>6)&0x3F];  result += '=';
        return result;
    }

    void onRevWsData(const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) {
        // Only ever process data from the one primary connection. Bytes buffered
        // on a rejected probe connection can still be delivered here — drop them
        // (and ensure that socket is closed) without touching any live state.
        // forceClose() must be called OUTSIDE the lock (see connection callback).
        {
            bool foreign;
            { std::lock_guard lk(revConnMutex_); foreign = (conn != revConn_); }
            if (foreign) { conn->forceClose(); return; }
        }

        // Accumulate into the PERSISTENT per-connection buffer. A single WebSocket
        // frame (e.g. a large get_group_member_list reply) can be split across
        // several TCP reads — consuming only complete frames and keeping the
        // remainder here is what prevents the "garbled half-frame" corruption.
        // recvMessageCallback always fires on revLoop_, so revBuffer_ needs no lock.
        revBuffer_.append(buf->peek(), buf->readableBytes());
        buf->retrieveAll();

        if (!connected_) {
            auto pos = revBuffer_.find("\r\n\r\n");
            if (pos == std::string::npos) return;   // wait for the full header block
            std::string headers = revBuffer_.substr(0, pos);
            std::string wsKey;
            std::istringstream iss(headers);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("Sec-WebSocket-Key:", 0) == 0) {  // 18 chars
                    wsKey = line.substr(18);
                    wsKey.erase(0, wsKey.find_first_not_of(" \t"));
                    if (auto e = wsKey.find_last_not_of(" \t"); e != std::string::npos)
                        wsKey.erase(e + 1);
                }
            }
            // Not a WebSocket upgrade (e.g. a plain HTTP/browser probe): reply
            // politely and close. The disconnect callback frees the primary slot.
            if (wsKey.empty()) {
                conn->send("HTTP/1.1 426 Upgrade Required\r\n"
                           "Connection: close\r\nContent-Length: 0\r\n\r\n");
                conn->forceClose();
                revBuffer_.clear();
                return;
            }
            conn->send("HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + wsAcceptKey(wsKey) + "\r\n\r\n");
            DICE_LOG_INFO("OneBotV11 '{}': reverse WS client connected via handshake", name_);
            connected_ = true;
            revBuffer_.erase(0, pos + 4);   // drop the handshake, keep any trailing frame bytes
            sendOneBotAction("get_login_info", json::object());
            refreshGroupList();
            startGroupRefreshTimer();
            // fall through to parse any frames that arrived glued to the handshake
        }

        // Parse complete WebSocket frames out of revBuffer_, reassembling
        // fragmented messages (FIN bit) across continuation frames.
        const std::string& d = revBuffer_;
        size_t consumed = 0;
        while (d.size() - consumed >= 2) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(d.data()) + consumed;
            size_t avail = d.size() - consumed;
            bool fin = (p[0] & 0x80) != 0;
            uint8_t opcode = p[0] & 0x0F;
            bool masked = (p[1] & 0x80) != 0;
            uint64_t payloadLen = p[1] & 0x7F;
            size_t offset = 2;
            if (payloadLen == 126) { if (avail < 4) break; payloadLen = (p[2] << 8) | p[3]; offset = 4; }
            else if (payloadLen == 127) { if (avail < 10) break; payloadLen = 0; for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | p[2 + i]; offset = 10; }
            size_t maskOffset = offset;
            if (masked) offset += 4;
            if (avail < offset + payloadLen) break;   // incomplete frame — wait for more

            std::string payload;
            payload.resize(payloadLen);
            if (masked) {
                const unsigned char* mask = p + maskOffset;
                const unsigned char* src = p + offset;
                for (uint64_t i = 0; i < payloadLen; i++) payload[i] = src[i] ^ mask[i % 4];
            } else {
                payload.assign(reinterpret_cast<const char*>(p + offset), payloadLen);
            }
            consumed += offset + payloadLen;

            if (opcode == 0x8) {                         // Close
                connected_ = false;
                revBuffer_.clear();
                conn->forceClose();
                return;
            } else if (opcode == 0x9) {                  // Ping → Pong
                uint8_t pong[] = {0x8A, 0x00};
                conn->send(std::string(reinterpret_cast<char*>(pong), 2));
            } else if (opcode == 0xA) {                  // Pong — ignore
            } else if (opcode == 0x1 || opcode == 0x2) { // Text/Binary
                if (fin) onRawMessage(std::move(payload));
                else revFrag_ = std::move(payload);      // first fragment
            } else if (opcode == 0x0) {                  // Continuation
                revFrag_ += payload;
                if (fin) { onRawMessage(std::move(revFrag_)); revFrag_.clear(); }
            }
        }
        if (consumed) revBuffer_.erase(0, consumed);
    }

    // Send a cooked WebSocket text frame to the reverse-WS client
    void sendRevWsFrame(const std::string& text) {
        std::lock_guard lk(revConnMutex_);
        if (!revConn_ || !revConn_->connected()) return;
        std::string frame;
        frame += '\x81'; // FIN + Text opcode
        if (text.size() < 126) { frame += (char)text.size(); }
        else if (text.size() < 65536) { frame += (char)126; frame += (char)(text.size()>>8); frame += (char)(text.size()&0xFF); }
        else { frame += (char)127; for(int i=7;i>=0;i--) frame += (char)(text.size()>>(8*i)); }
        frame += text;
        revConn_->send(frame);
    }

    /// Start the periodic (1-min) group-name refresh, once. Pulls group info
    /// quietly so cached group names stay current; failures are logged by the
    /// echo handler. Guarded so it's only ever scheduled a single time.
    void startGroupRefreshTimer() {
        bool expected = false;
        if (!refreshTimerStarted_.compare_exchange_strong(expected, true)) return;
        auto self = shared_from_this();
        drogon::app().getLoop()->runEvery(60.0, [self]() {
            if (self->connected_) self->refreshGroupList();
        });
    }

    /// Schedule a reconnect attempt (forward WS only). 退避策略：
    ///   前 10 次每 5 秒重连；第 11~20 次每 60 秒；超过 20 次暂停自动重连，状态置「连接超时」。
    void scheduleReconnect() {
        if (stopping_ || mode_ != "forward_ws") return;
        int attempt = ++reconnectAttempts_;
        if (attempt > kPauseAfterAttempts) {
            timedOut_ = true;
            lastError_ = "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xb6\x85\xe6\x97\xb6";  // 连接超时
            DICE_LOG_WARN("OneBotV11 '{}': reconnect paused after {} failed attempts (connection timeout); manual reconnect required",
                          name_, kPauseAfterAttempts);
            return;
        }
        double delay = (attempt > kFastRetryAttempts) ? kSlowReconnectDelaySec : kReconnectDelaySec;
        auto self = shared_from_this();
        DICE_LOG_INFO("OneBotV11 '{}': reconnect attempt {} in {}s", name_, attempt, delay);
        drogon::app().getLoop()->runAfter(delay, [self]() {
            if (!self->stopping_ && !self->connected_) self->start();
        });
    }

    // ─── OneBot v11 Protocol ─────────────────────────────────

    void sendOneBotAction(const std::string& action, const json& params,
                          const std::string& echo = "") {
        if (!connected_) { DICE_LOG_WARN("OneBotV11 '{}': cannot send, not connected", name_); return; }
        json req{{"action", action}, {"params", params}};
        req["echo"] = echo.empty() ? std::to_string(++echoSeq_) : echo;

        if (mode_ == "reverse_ws") {
            sendRevWsFrame(req.dump());
        } else if (wsClient_) {
            wsClient_->getConnection()->send(req.dump());
        }
    }

public:
    // Runtime toggle: true = dump raw OneBot JSON events; false = human-readable.
    // Settable from config (log.raw_events) and the web admin (/api/system/log-mode).
    inline static std::atomic<bool> s_rawEventLog{false};

    // 自响应：true = 也处理骰娘账号自身发出的消息（post_type=message_sent），
    // 允许用骰娘账号发指令自控。默认关。config dice/respond_self + /api/system/respond-self。
    inline static std::atomic<bool> s_respondSelf{false};

private:
    void onRawMessage(std::string&& raw) {
        const bool rawMode = s_rawEventLog.load();
        try {
            auto j = json::parse(raw);
            // 详细模式：每个事件输出一条「完整可读解释」（取代原来截断到 200 字节
            // 的原始 JSON 转储——那个既不完整又不知所谓，尤其图片/复杂消息）。简洁模式下
            // 仍由后面的 收↩/事件 行处理（且静音心跳与接口成功响应）。
            if (rawMode)
                DICE_LOG_INFO("\xe8\xaf\xa6\xe7\xbb\x86 {}", interpretEvent(j));   // 详细

            // Handle post_type: message events. 自响应开启时，也把骰娘账号自身
            // 发出的消息（post_type=message_sent）当作消息处理，用于自控。
            std::string postType = j.contains("post_type") ? j.value("post_type", std::string()) : "";
            // 自身消息可能来自 post_type=message_sent（NapCat 扩展），也可能是
            // post_type=message 且 user_id==self_id（部分实现/配置）。两种都算「自身」。
            std::string mSelfId = jsonField(j, "self_id");
            std::string mUserId = jsonField(j, "user_id");
            bool isSelf = (postType == "message_sent")
                        || (postType == "message" && !mSelfId.empty() && mUserId == mSelfId);
            if (isSelf && !s_respondSelf.load()
                && (postType == "message" || postType == "message_sent")) {
                // 自响应未开启 → 忽略自身消息（保持默认不自触发）。首次收到时打一条提示，
                // 帮助区分「NapCat 未转发自身消息」（无此日志）与「开关未开」（有此日志）。
                static std::atomic<bool> s_selfHintShown{false};
                if (!s_selfHintShown.exchange(true))
                    DICE_LOG_INFO("检测到骰娘账号自身消息——如需用骰娘账号自控，请在系统设置开启「自响应消息」");
            } else if (postType == "message" || (isSelf && s_respondSelf.load())) {
                Message msg;
                msg.fromSelf = isSelf;
                // message_id / message_seq may arrive as an integer (NapCat) or a
                // string depending on the OneBot implementation — handle both.
                msg.id = jsonField(j, "message_id");
                if (msg.id.empty()) msg.id = jsonField(j, "message_seq");
                msg.platform = platform();   // "onebot_v11" — used for i18n locale resolution
                msg.selfId = jsonField(j, "self_id");  // which bot received this (multi-bot targeting)
                msg.adapterId = id_;          // which adapter received this (for .dismiss etc.)
                msg.timestamp = j.value("time", (int64_t)0);

                // Parse message content — supports both string and array format.
                // @ mentions are collected into atList (for account targeting) and
                // kept OUT of content so the command text stays clean.
                auto& rawMsg = j["message"];
                std::string text;        // clean text for command parsing
                std::string cqContent;   // CQ-coded (text+image+at+face) for log transcript
                std::string disp;        // human-readable, ordered (text/@xx/[图片]/[表情])
                if (rawMsg.is_string()) {
                    // String format embeds @ as [CQ:at,qq=..]; pull those into
                    // atList (so multi-bot @-targeting / 代骰 work) while stripping
                    // CQ codes out of the command text. The raw string keeps the
                    // CQ codes (incl. images) for the transcript.
                    std::string s = rawMsg.get<std::string>();
                    cqContent = s;
                    text = stripCQCollectAt(s, msg.atList);
                    disp = cqToDisplay(s);   // codes → [图片]/[表情]/@xx in original order
                } else if (rawMsg.is_array()) {
                    // OneBot array message: [{"type":"text","data":{"text":"hello"}}, ...]
                    for (auto& seg : rawMsg) {
                        const std::string segType = seg.value("type", "");
                        const auto& data = seg.contains("data") ? seg["data"] : json::object();
                        disp += segToDisplay(segType, data);  // ordered readable form
                        if (segType == "text") {
                            std::string t = data.value("text", "");
                            text += t; cqContent += t;
                        } else if (segType == "at") {
                            std::string qq = jsonField(data, "qq");          // string or number
                            if (!qq.empty()) { msg.atList.push_back(qq); cqContent += "[CQ:at,qq=" + qq + "]"; }
                        } else if (segType == "image") {
                            std::string url = jsonField(data, "url");
                            std::string file = jsonField(data, "file");
                            std::string ref = !url.empty() ? url : file;     // url is viewable; file as fallback
                            if (!ref.empty()) cqContent += "[CQ:image,file=" + ref + "]";
                        } else if (segType == "face") {
                            std::string id = jsonField(data, "id");
                            if (!id.empty()) cqContent += "[CQ:face,id=" + id + "]";
                        } else if (segType == "file") {
                            // NTQQ 群文件可作为消息段到达（NapCat）。带出可下载引用，
                            // 模拟聊天/记录里渲染成文件条目（file_id 供 get_group_file_url）。
                            std::string fname = data.value("file", data.value("name", std::string()));
                            std::string fid = jsonField(data, "file_id");
                            std::string fsize = jsonField(data, "file_size");
                            cqContent += "[CQ:file,name=" + fname + ",id=" + fid + ",size=" + fsize + "]";
                        }
                    }
                }
                // rawContent keeps the CQ-coded original (so .log transcripts can
                // still resolve images); displayContent is the readable form for
                // logs/transcripts/模拟聊天; content is the cleaned command text.
                msg.rawContent = cqContent;
                msg.displayContent = disp.empty() ? text : disp;
                msg.content = text;

                auto msgType = j.value("message_type", "group");
                if (msgType == "private") {
                    msg.type = MessageType::kPrivate;
                    msg.senderId = std::to_string(j.value("user_id", 0LL));
                    // 自身私聊消息(message_sent)里 user_id=自己，对话对象在 target_id；
                    // 回复要发到对方私聊窗口，而不是发给自己。普通私聊 target_id 缺省=user_id。
                    if (isSelf) {
                        std::string tgt = jsonField(j, "target_id");
                        msg.targetId = (!tgt.empty() && tgt != "0") ? tgt : msg.senderId;
                    } else {
                        msg.targetId = msg.senderId;
                    }
                } else if (msgType == "guild") {
                    msg.type = MessageType::kChannel;
                    msg.senderId = std::to_string(j.value("user_id", 0LL));
                    msg.targetId = std::to_string(j.value("channel_id", 0LL));
                    msg.extra["guild_id"] = j.value("guild_id", "");
                } else {
                    msg.type = MessageType::kGroup;
                    msg.senderId = std::to_string(j.value("user_id", 0LL));
                    msg.targetId = std::to_string(j.value("group_id", 0LL));
                    // Resolve group name from cache (populated via get_group_list API response)
                    auto gnIt = groupNames_.find(msg.targetId);
                    if (gnIt != groupNames_.end()) msg.extra["groupName"] = gnIt->second;
                }

                // Sender display name: use the user's real QQ nickname (fresh every
                // message), NOT the group card — because .sn rewrites the card with a
                // stat string ("名字 san77/99 hp10000/10000 …"), which must never
                // become the {nick} we show. The card is kept in extra for reference.
                if (j.contains("sender") && j["sender"].is_object()) {
                    const auto& s = j["sender"];
                    msg.senderName = s.value("nickname", "");
                    if (msg.senderName.empty()) msg.senderName = s.value("card", "");
                    msg.extra["card"] = s.value("card", "");
                    msg.extra["role"] = s.value("role", "");
                }

                msg.extra["raw"] = j;
                if (!rawMode) {
                    // Human-readable, with a direction marker: "收↩ [群 群名(id)] 昵称(uid): 内容".
                    std::string nick = msg.senderName.empty() ? msg.senderId : msg.senderName;
                    std::string where = (msg.type == MessageType::kPrivate)
                        ? std::string("私聊")
                        : "群 " + getGroupName(msg.targetId) + "(" + msg.targetId + ")";
                    // Use the readable, order-preserving form: @ stays where the
                    // user typed it, images/faces show as [图片]/[表情]. (content
                    // strips @ and would force all mentions to the tail.)
                    std::string shown = msg.displayContent.empty() ? msg.content : msg.displayContent;
                    // Flatten CR/LF so a user can't paste fake "[time] ..." lines that
                    // would be parsed as separate dashboard log entries (log injection).
                    std::string flat; flat.reserve(shown.size());
                    for (char ch : shown) { if (ch == '\n') flat += "\xe2\x8f\x8e"; else if (ch != '\r') flat += ch; }  // \n → ⏎
                    DICE_LOG_INFO("收\xe2\x86\xa9 [{}] {}({}): {}", where, nick, msg.senderId, flat);
                }

                // 自回声去重：若这是骰娘账号自身发出的消息（message_sent），且能在
                // 近期「本程序主动发送」登记里匹配到 → 判为骰娘自己的回复的回声，直接丢弃，
                // 不进管线（否则会无限自触发）；匹配不到才是操作者手打的自控指令，正常处理。
                if (isSelf && SelfEchoFilter::instance().consume(
                        platform() + ":" + msg.targetId, normalizeEcho(msg.content))) {
                    return;
                }
                // Notify registered callbacks (→ CommandRouter)
                for (auto& cb : messageCallbacks_) {
                    cb(msg);
                }
            }
            // Handle post_type: notice / request events (group join, friend add,
            // friend/group requests, poke). Abstracted into BotEvent for the engine.
            else if (j.contains("post_type") &&
                     (j["post_type"] == "notice" || j["post_type"] == "request")) {
                handlePlatformEvent(j);
            }
            // Handle echo responses (api call results)
            else if (j.contains("echo")) {
                std::string echo = j["echo"].is_string() ? j["echo"].get<std::string>() : "";
                json d = j.contains("data") ? j["data"] : json(nullptr);
                bool failed = j.value("status", std::string("ok")) == "failed" ||
                    (j.contains("retcode") && j["retcode"].is_number_integer() && j["retcode"].get<int>() != 0);
                // Group-file upload fallback: a local-path attempt (echo "ufup:*")
                // that fails (e.g. remote OneBot can't stat the path) is retried as
                // base64:// instead of just being logged as a failure.
                if (echo.rfind("ufup:", 0) == 0) {
                    PendingUpload pu; bool have = false;
                    {
                        std::lock_guard<std::mutex> lk(dataMutex_);
                        auto it = pendingUploads_.find(echo);
                        if (it != pendingUploads_.end()) { pu = it->second; have = true; pendingUploads_.erase(it); }
                    }
                    if (failed && have) {
                        DICE_LOG_INFO("OneBotV11 '{}': local group-file upload failed, retrying via base64", name_);
                        uploadGroupFileBase64(pu.groupId, pu.name, pu.content);
                    }
                    return;  // handled (success → nothing more to do)
                }
                // 同步调用（invokeAction）的响应 → 兑现等待中的 promise。
                if (echo.rfind("invoke:", 0) == 0) {
                    std::shared_ptr<std::promise<json>> pr;
                    {
                        std::lock_guard<std::mutex> lk(invokeMutex_);
                        auto it = pendingInvokes_.find(echo);
                        if (it != pendingInvokes_.end()) { pr = it->second; pendingInvokes_.erase(it); }
                    }
                    if (pr) { try { pr->set_value(j); } catch (...) {} }
                    return;
                }
                // 群名反查（get_group_info）响应 → 缓存 group_name。
                if (echo.rfind("ginfo:", 0) == 0) {
                    std::string gid = echo.substr(6);
                    std::lock_guard<std::mutex> lk(dataMutex_);
                    ginfoInflight_.erase(gid);
                    if (d.is_object()) {
                        std::string gname = d.value("group_name", std::string());
                        if (!gname.empty() && gname != gid) {
                            groupNames_[gid] = gname;               // 供 getGroupName / 通知显示真名
                            if (groupList_.count(gid)) groupList_[gid] = gname;
                        }
                    }
                    return;
                }
                // Surface FAILED API calls (e.g. set_group_ban/set_group_card rejected
                // for lack of permission or bad params) — otherwise they fail silently.
                if (failed) {
                    DICE_LOG_WARN("OneBotV11 '{}': action failed (retcode={}): {}", name_,
                                  j.contains("retcode") ? j["retcode"].dump() : std::string("?"),
                                  raw.substr(0, 300));
                }
                if (echo == "glist") {
                    // Group-info pull: silent on success (per spec), log only failures.
                    if (!d.is_array()) {
                        DICE_LOG_ERROR("OneBotV11 '{}': get_group_list failed: {}", name_,
                                       j.value("status", std::string("?")));
                    } else {
                        std::vector<std::string> needName;   // 协议端没给名字、待 get_group_info 反查
                        {
                        std::lock_guard<std::mutex> lk(dataMutex_);
                        groupList_.clear();
                        for (auto& g : d) {
                            std::string gid = jsonField(g, "group_id");
                            if (gid.empty()) continue;
                            std::string gname = g.value("group_name", std::string());
                            if (gname.empty()) gname = gid;               // 无名 → 先占位为号码
                            groupList_[gid] = gname;
                            // 不用「无名占位」覆盖此前 get_group_info 反查到的真名。
                            auto ex = groupNames_.find(gid);
                            bool haveReal = ex != groupNames_.end() && !ex->second.empty() && ex->second != gid;
                            if (!haveReal) groupNames_[gid] = gname;
                            if (gname == gid && !haveReal) needName.push_back(gid);
                            if (g.contains("member_count") && g["member_count"].is_number())
                                groupCount_[gid] = g["member_count"].get<int>();
                        }
                        }   // 释放锁后再反查（requestGroupInfo 内部会再锁 dataMutex_）
                        for (auto& gid : needName) requestGroupInfo(gid);
                    }
                } else if (echo == "flist") {
                    // friend-list pull — cache count + uid list (silent on success).
                    if (d.is_array()) {
                        std::lock_guard<std::mutex> lk(dataMutex_);
                        friendCount_ = (int)d.size();
                        friendList_.clear();
                        for (auto& f : d) {
                            std::string uid = jsonField(f, "user_id");
                            if (!uid.empty()) friendList_.push_back(uid);
                        }
                    }
                } else if (echo.rfind("history:", 0) == 0) {
                    // 历史消息 → 打包成 kGroupHistory 事件回流给引擎入库。
                    const json* msgs = nullptr;
                    if (d.is_object() && d.contains("messages") && d["messages"].is_array()) msgs = &d["messages"];
                    else if (d.is_array()) msgs = &d;
                    if (msgs) {
                        BotEvent hv;
                        hv.type = EventType::kGroupHistory;
                        hv.platform = platform(); hv.adapterId = id_;
                        hv.selfId = loginId_;
                        hv.groupId = echo.substr(8);
                        hv.extra = json{{"messages", *msgs}};
                        DICE_LOG_INFO("OneBotV11 '{}': group {} history fetched, {} message(s)",
                                      name_, hv.groupId, msgs->size());
                        for (auto& cb : eventCallbacks_) cb(hv);
                    }
                } else if (echo.rfind("members:", 0) == 0 && d.is_array()) {
                    std::string gid = echo.substr(8);
                    std::lock_guard<std::mutex> lk(dataMutex_);
                    memberLists_[gid] = d;
                    for (auto& m : d)
                        if (jsonField(m, "user_id") == loginId_) { selfRole_[gid] = m.value("role", "member"); break; }
                } else if (echo.rfind("role:", 0) == 0 && d.is_object()) {
                    const std::string tail = echo.substr(5);
                    const std::string role = d.value("role", "member");
                    std::lock_guard<std::mutex> lk(dataMutex_);
                    const auto sep = tail.rfind(':');
                    if (sep == std::string::npos) {
                        // legacy format: role:<gid> (bot's own role only)
                        selfRole_[tail] = role;
                    } else {
                        const std::string gid = tail.substr(0, sep);
                        const std::string uid = tail.substr(sep + 1);
                        if (uid == loginId_) selfRole_[gid] = role;
                        if (!gid.empty() && !uid.empty()) memberRoles_[gid + "\x1f" + uid] = role;
                    }
                } else if (d.is_object()) {
                    // get_login_info response
                    if (d.contains("user_id")) loginId_ = std::to_string(d["user_id"].get<int64_t>());
                    if (d.contains("nickname")) loginName_ = d["nickname"];
                }
            }
            // Handle lifecycle events
            else if (j.contains("meta_event_type")) {
                auto meta = j["meta_event_type"].get<std::string>();
                if (meta == "lifecycle") {
                    loginId_ = std::to_string(j.value("self_id", 0LL));
                    DICE_LOG_INFO("OneBotV11 '{}': lifecycle, self_id={}", name_, loginId_);
                }
                // Respond to heartbeat with .meta_event (some OneBot impls need this)
                // Heartbeat just confirms connection is alive — no action needed
            }
        } catch (const std::exception& e) {
            DICE_LOG_WARN("OneBotV11 '{}': failed to handle event: {}", name_, e.what());
        }
    }

    /// Parse a OneBot notice/request event into a BotEvent and dispatch it.
    void handlePlatformEvent(const json& j) {
        BotEvent ev;
        ev.platform = platform();
        ev.adapterId = id_;
        ev.selfId = jsonField(j, "self_id");
        ev.timestamp = j.value("time", (int64_t)0);
        ev.groupId = jsonField(j, "group_id");
        ev.userId = jsonField(j, "user_id");
        ev.operatorId = jsonField(j, "operator_id");
        ev.comment = j.value("comment", "");
        ev.flag = j.value("flag", "");
        ev.subType = j.value("sub_type", "");
        ev.extra = j;

        const std::string post = j.value("post_type", "");
        std::string label;
        if (post == "notice") {
            const std::string nt = j.value("notice_type", "");
            if (nt == "group_increase")      { ev.type = EventType::kGroupIncrease; label = "入群"; }
            else if (nt == "group_decrease") { ev.type = EventType::kGroupDecrease; label = "退群"; }
            else if (nt == "friend_add")     { ev.type = EventType::kFriendAdd;     label = "新好友"; }
            else if (nt == "notify" && j.value("sub_type", "") == "poke") {
                ev.type = EventType::kPoke;
                ev.userId = jsonField(j, "target_id");     // who was poked
                ev.operatorId = jsonField(j, "user_id");   // who poked
                label = "戳一戳";
            }
            else if (nt == "group_recall") {               // 群消息撤回
                ev.type = EventType::kGroupRecall;
                ev.extra["message_id"] = jsonField(j, "message_id");
                label = "撤回";
            }
            else if (nt == "group_upload") {               // 群文件上传
                ev.type = EventType::kGroupUpload;
                json f = j.contains("file") && j["file"].is_object() ? j["file"] : json::object();
                ev.extra["file"] = json{{"id", jsonField(f, "id")}, {"name", f.value("name", std::string())},
                                        {"size", jsonField(f, "size")}, {"busid", jsonField(f, "busid")}};
                label = "\xe7\xbe\xa4\xe6\x96\x87\xe4\xbb\xb6";   // 群文件
            } else { ev.type = EventType::kOther; }
        } else { // request
            const std::string rt = j.value("request_type", "");
            if (rt == "friend")     { ev.type = EventType::kFriendRequest; label = "加好友请求"; }
            else if (rt == "group") { ev.type = EventType::kGroupRequest;  label = "加群请求"; }
            else { ev.type = EventType::kOther; }
        }
        if (ev.type == EventType::kOther) return;   // nothing we act on

        // 群名未知 → 异步反查补齐缓存，供本群后续通知显示「名(号)」。
        if (!ev.groupId.empty()) {
            bool known;
            {
                std::lock_guard<std::mutex> lk(dataMutex_);
                auto it = groupNames_.find(ev.groupId);
                known = it != groupNames_.end() && !it->second.empty() && it->second != ev.groupId;
            }
            if (!known) requestGroupInfo(ev.groupId);
        }

        if (!s_rawEventLog.load() && !label.empty()) {
            std::string where = ev.groupId.empty() ? std::string("私聊")
                                                   : "群 " + getGroupName(ev.groupId) + "(" + ev.groupId + ")";
            DICE_LOG_INFO("\xe4\xba\x8b\xe4\xbb\xb6 [{}] {} user={}", where, label, ev.userId);  // 事件
        }
        for (auto& cb : eventCallbacks_) cb(ev);
    }

    /// Read a JSON field as a string whether it's stored as string or number.
    /// OneBot implementations are inconsistent about id/qq field types.
    static std::string jsonField(const json& j, const char* key) {
        if (!j.contains(key) || j[key].is_null()) return "";
        const auto& v = j[key];
        if (v.is_string())           return v.get<std::string>();
        if (v.is_number_integer())   return std::to_string(v.get<int64_t>());
        if (v.is_number_unsigned())  return std::to_string(v.get<uint64_t>());
        if (v.is_number_float())     return std::to_string(v.get<long long>());
        return "";
    }

    /// CQ-code value unescape (commas/brackets/amp are escaped inside CQ codes).
    static std::string cqUnescape(std::string s) {
        auto rep = [&](const std::string& a, const std::string& b) {
            size_t p; while ((p = s.find(a)) != std::string::npos) s.replace(p, a.size(), b);
        };
        rep("&#44;", ","); rep("&#91;", "["); rep("&#93;", "]"); rep("&amp;", "&");
        return s;
    }

    /// Parse one bracket body into a OneBot segment, or null if not a known code.
    /// Supports the platform-neutral image code ([img,file=..],), CQ codes
    /// ([CQ:image,file=..]/[CQ:at,qq=..]/[CQ:face,id=..]/[CQ:music,...]) and compatible image
    /// codes ([图片:..]/[图:..]).
    static json parseCode(const std::string& inner) {
        // 平台中立图片码：[img,file=<本地路径或URL>]
        if (inner.rfind("img,", 0) == 0) {
            std::string rest = inner.substr(4);
            size_t pos = 0;
            while (pos < rest.size()) {
                size_t nc = rest.find(',', pos);
                std::string pair = rest.substr(pos, nc == std::string::npos ? std::string::npos : nc - pos);
                size_t eq = pair.find('=');
                if (eq != std::string::npos && pair.substr(0, eq) == "file") {
                    std::string f = cqUnescape(pair.substr(eq + 1));
                    if (!f.empty()) return json{{"type", "image"}, {"data", json{{"file", f}}}};
                }
                if (nc == std::string::npos) break;
                pos = nc + 1;
            }
            return json(nullptr);
        }
        if (inner.rfind("CQ:", 0) == 0) {
            std::string body = inner.substr(3);
            size_t comma = body.find(',');
            std::string type = (comma == std::string::npos) ? body : body.substr(0, comma);
            std::map<std::string, std::string> kv;
            if (comma != std::string::npos) {
                std::string rest = body.substr(comma + 1);
                size_t pos = 0;
                while (pos < rest.size()) {
                    size_t nc = rest.find(',', pos);
                    std::string pair = rest.substr(pos, nc == std::string::npos ? std::string::npos : nc - pos);
                    size_t eq = pair.find('=');
                    if (eq != std::string::npos) kv[pair.substr(0, eq)] = cqUnescape(pair.substr(eq + 1));
                    if (nc == std::string::npos) break;
                    pos = nc + 1;
                }
            }
            if (type == "image") {
                std::string f = kv.count("file") ? kv["file"] : kv["url"];
                if (!f.empty()) return json{{"type", "image"}, {"data", json{{"file", f}}}};
            } else if (type == "at") {
                if (!kv["qq"].empty()) return json{{"type", "at"}, {"data", json{{"qq", kv["qq"]}}}};
            } else if (type == "face") {
                if (!kv["id"].empty()) return json{{"type", "face"}, {"data", json{{"id", kv["id"]}}}};
            } else if (type == "music") {
                // Lua 点歌插件通常直接返回 CQ 音乐码。OneBot 数组消息必须把它
                // 转成 music segment；否则整段 CQ 文本会作为普通文本显示。
                json data = json::object();
                for (const auto& [key, value] : kv) data[key] = value;
                return json{{"type", "music"}, {"data", std::move(data)}};
            }
            return json(nullptr);
        }
        // 兼容图片占位符：[图片:X] / [图:X]
        const std::string p1 = "\xe5\x9b\xbe\xe7\x89\x87:";  // 图片:
        const std::string p2 = "\xe5\x9b\xbe:";              // 图:
        if (inner.rfind(p1, 0) == 0 && inner.size() > p1.size())
            return json{{"type", "image"}, {"data", json{{"file", inner.substr(p1.size())}}}};
        if (inner.rfind(p2, 0) == 0 && inner.size() > p2.size())
            return json{{"type", "image"}, {"data", json{{"file", inner.substr(p2.size())}}}};
        return json(nullptr);
    }

    /// Build a OneBot message array from text that may embed image/at/face/music codes
    /// (CQ codes or compatible placeholders). Plain runs become text segments; unknown
    /// bracket codes are kept as literal text.
    static json buildSegments(const std::string& text) {
        json arr = json::array();
        std::string buf;
        auto flush = [&]() { if (!buf.empty()) { arr.push_back(json{{"type", "text"}, {"data", json{{"text", buf}}}}); buf.clear(); } };
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '[') {
                size_t end = text.find(']', i);
                if (end != std::string::npos) {
                    json seg = parseCode(text.substr(i + 1, end - i - 1));
                    if (!seg.is_null()) {
                        // 图片按「图片发送方式」配置转换（本地路径/本站资产
                        // 链接 → base64:// 或 http://<host>/api/assets/..）。
                        if (seg.value("type", "") == "image")
                            seg["data"]["file"] = imgsend::resolve(seg["data"].value("file", ""));
                        flush(); arr.push_back(seg); i = end + 1; continue;
                    }
                }
            }
            buf += text[i++];
        }
        flush();
        if (arr.empty()) arr.push_back(json{{"type", "text"}, {"data", json{{"text", ""}}}});
        return arr;
    }

    /// 字符串格式发送前的归一化——平台中立 [img,file=..] 转成 CQ 图片码，
    /// [CQ:image,..] 的本地资产引用按「图片发送方式」重解析；其余码原样保留
    /// （[CQ:poke] 等仍由平台解析）。
    static std::string normalizeCQText(const std::string& text) {
        std::string out; out.reserve(text.size());
        auto cqEscape = [](const std::string& s) {
            std::string r; r.reserve(s.size());
            for (char c : s) {
                if (c == '&') r += "&amp;"; else if (c == '[') r += "&#91;";
                else if (c == ']') r += "&#93;"; else if (c == ',') r += "&#44;";
                else r += c;
            }
            return r;
        };
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '[' && (text.compare(i, 5, "[img,") == 0 || text.compare(i, 9, "[CQ:image") == 0)) {
                size_t end = text.find(']', i);
                if (end != std::string::npos) {
                    json seg = parseCode(text.substr(i + 1, end - i - 1));
                    if (seg.is_object() && seg.value("type", "") == "image") {
                        out += "[CQ:image,file=" + cqEscape(imgsend::resolve(seg["data"].value("file", ""))) + "]";
                        i = end + 1;
                        continue;
                    }
                }
            }
            out += text[i++];
        }
        return out;
    }

    /// Strip CQ codes for clean command text, AND collect any [CQ:at,qq=..]
    /// targets into @p atList. Used for string-format messages (array format
    /// extracts @ inline). qq="all" represents @全体成员.
    static std::string stripCQCollectAt(const std::string& raw, std::vector<std::string>& atList) {
        std::string result;
        result.reserve(raw.size());
        for (size_t i = 0; i < raw.size();) {
            if (raw[i] == '[' && raw.compare(i, 4, "[CQ:") == 0) {
                size_t end = raw.find(']', i);
                if (end != std::string::npos) {
                    json seg = parseCode(raw.substr(i + 1, end - i - 1));
                    if (seg.is_object() && seg.value("type", "") == "at") {
                        std::string qq = jsonField(seg["data"], "qq");
                        if (!qq.empty()) atList.push_back(qq);
                    }
                    i = end + 1;
                    continue;   // drop the CQ code from the clean text
                }
            }
            result += raw[i++];
        }
        return result;
    }

    /// Convert a CQ-coded string into a human-readable form, preserving the
    /// ORIGINAL order of text and codes. Images/faces/etc. become bracketed
    /// labels ([图片]/[表情]/...) and @ becomes @<qq>. Used for log/transcript
    /// display so users see something meaningful instead of raw CQ codes.
    static std::string cqToDisplay(const std::string& raw) {
        std::string result;
        result.reserve(raw.size());
        for (size_t i = 0; i < raw.size();) {
            if (raw[i] == '[' && raw.compare(i, 4, "[CQ:") == 0) {
                size_t end = raw.find(']', i);
                if (end != std::string::npos) {
                    json seg = parseCode(raw.substr(i + 1, end - i - 1));
                    if (seg.is_object()) result += segToDisplay(seg.value("type", ""), seg["data"]);
                    i = end + 1;
                    continue;
                }
            }
            // Unescape OneBot CQ entities back to literals for readability.
            if (raw.compare(i, 4, "&#91;") == 0) { result += '['; i += 5; continue; }
            if (raw.compare(i, 4, "&#93;") == 0) { result += ']'; i += 5; continue; }
            if (raw.compare(i, 5, "&amp;") == 0) { result += '&'; i += 5; continue; }
            result += raw[i++];
        }
        return result;
    }

    /// Map one segment (type + data) to its human-readable label.
    static std::string segToDisplay(const std::string& type, const json& data) {
        if (type == "text") return data.value("text", "");
        if (type == "at") {
            std::string qq = jsonField(data, "qq");
            return qq == "all" ? "@全体成员" : ("@" + qq);
        }
        if (type == "image") return "[图片]";
        if (type == "face" || type == "mface" || type == "sface") return "[表情]";
        if (type == "record") return "[语音]";
        if (type == "video") return "[视频]";
        if (type == "file") return "[文件]";
        if (type == "reply") return "[回复]";
        if (type == "forward") return "[合并转发]";
        if (type == "json" || type == "xml") return "[卡片]";
        if (type == "poke") return "[戳一戳]";
        if (type == "share") return "[分享]";
        if (type == "music") return "[音乐]";
        if (type == "dice") return "[骰子]";
        if (type == "rps") return "[猜拳]";
        return "[" + type + "]";
    }

    // ── 详细日志：把一条 OneBot 消息体渲染成可读内容（图片带 URL）──
    std::string describeMessageBody(const json& message) const {
        if (message.is_string()) return cqToDisplay(message.get<std::string>());
        if (!message.is_array()) return "";
        std::string out;
        for (const auto& seg : message) {
            if (!seg.is_object()) continue;
            const std::string type = seg.value("type", "");
            const json& data = seg.contains("data") && seg["data"].is_object() ? seg["data"] : json::object();
            if (type == "image") {
                std::string url = jsonField(data, "url");
                if (url.empty()) url = jsonField(data, "file");
                out += url.empty() ? "[图片]" : "[图片:" + url + "]";
            } else {
                out += segToDisplay(type, data);
            }
        }
        return out;
    }

    // ── 详细日志：把任意 OneBot 事件解释成一行可读文本（替换原来截断的
    // 原始 JSON 转储）。覆盖 消息 / 通知 / 请求 / 元事件(心跳/生命周期) / 接口响应。──
    std::string interpretEvent(const json& j) const {
        const std::string post = j.value("post_type", "");
        if (post == "message") {
            const std::string mt = j.value("message_type", "");
            std::string gid = jsonField(j, "group_id"), uid = jsonField(j, "user_id");
            std::string nick;
            if (j.contains("sender") && j["sender"].is_object()) {
                nick = j["sender"].value("card", "");
                if (nick.empty()) nick = j["sender"].value("nickname", "");
            }
            if (nick.empty()) nick = uid;
            std::string where = (mt == "private") ? std::string("\xe7\xa7\x81\xe8\x81\x8a")   // 私聊
                : "\xe7\xbe\xa4 " + getGroupName(gid) + "(" + gid + ")";                       // 群 名(id)
            std::string body = j.contains("message") ? describeMessageBody(j["message"]) : "";
            // 折叠换行，避免被仪表盘当成多条日志。
            std::string flat; flat.reserve(body.size());
            for (char ch : body) { if (ch == '\n') flat += "\xe2\x8f\x8e"; else if (ch != '\r') flat += ch; }
            return "\xe6\xb6\x88\xe6\x81\xaf [" + where + "] " + nick + "(" + uid + "): " + flat;  // 消息
        }
        if (post == "notice") {
            const std::string nt = j.value("notice_type", ""), sub = j.value("sub_type", "");
            std::string gid = jsonField(j, "group_id");
            std::string where = gid.empty() ? std::string("\xe7\xa7\x81\xe8\x81\x8a")
                : "\xe7\xbe\xa4 " + getGroupName(gid) + "(" + gid + ")";
            std::string desc = nt;
            if (nt == "group_increase")      desc = "\xe5\x85\xa5\xe7\xbe\xa4";                  // 入群
            else if (nt == "group_decrease") desc = "\xe9\x80\x80\xe7\xbe\xa4";                  // 退群
            else if (nt == "friend_add")     desc = "\xe6\x96\xb0\xe5\xa5\xbd\xe5\x8f\x8b";      // 新好友
            else if (nt == "group_recall" || nt == "friend_recall") desc = "\xe6\x92\xa4\xe5\x9b\x9e";  // 撤回
            else if (nt == "group_ban")      desc = (sub == "lift_ban") ? "\xe8\xa7\xa3\xe7\xa6\x81" : "\xe7\xa6\x81\xe8\xa8\x80";  // 解禁/禁言
            else if (nt == "notify" && sub == "poke") desc = "\xe6\x88\xb3\xe4\xb8\x80\xe6\x88\xb3";    // 戳一戳
            std::string op = jsonField(j, "operator_id");
            return "\xe9\x80\x9a\xe7\x9f\xa5 [" + where + "] " + desc + " user=" + jsonField(j, "user_id")  // 通知
                 + (op.empty() ? "" : " op=" + op);
        }
        if (post == "request") {
            const std::string rt = j.value("request_type", "");
            std::string desc = rt == "friend" ? "\xe5\x8a\xa0\xe5\xa5\xbd\xe5\x8f\x8b\xe8\xaf\xb7\xe6\xb1\x82"   // 加好友请求
                             : rt == "group"  ? "\xe5\x8a\xa0\xe7\xbe\xa4\xe8\xaf\xb7\xe6\xb1\x82"               // 加群请求
                             : rt;
            return "\xe8\xaf\xb7\xe6\xb1\x82 " + desc + " user=" + jsonField(j, "user_id")       // 请求
                 + " \xe9\xaa\x8c\xe8\xaf\x81='" + j.value("comment", std::string()) + "'";       // 验证
        }
        if (post == "meta_event") {
            const std::string me = j.value("meta_event_type", "");
            if (me == "heartbeat") return "\xe5\xbf\x83\xe8\xb7\xb3";                              // 心跳
            if (me == "lifecycle") return "\xe7\x94\x9f\xe5\x91\xbd\xe5\x91\xa8\xe6\x9c\x9f self=" + jsonField(j, "self_id");  // 生命周期
            return "\xe5\x85\x83\xe4\xba\x8b\xe4\xbb\xb6 " + me;                                   // 元事件
        }
        if (j.contains("echo")) {
            std::string echo = j["echo"].is_string() ? j["echo"].get<std::string>() : "";
            std::string status = j.value("status", std::string("?"));
            std::string extra;
            if (j.contains("data") && !j["data"].is_null()) {
                const json& d = j["data"];
                if (d.is_array()) extra = " " + std::to_string(d.size()) + "\xe9\xa1\xb9";          // N项
                else if (d.is_object()) extra = " 1\xe9\xa1\xb9";
            }
            return "\xe6\x8e\xa5\xe5\x8f\xa3\xe5\x93\x8d\xe5\xba\x94 echo='" + echo + "' " + status + extra;  // 接口响应
        }
        return "\xe4\xba\x8b\xe4\xbb\xb6 " + (post.empty() ? std::string("?") : post);             // 事件
    }

    /// Strip CQ codes from text to get clean content for command matching
    static std::string stripCQ(const std::string& raw) {
        std::string result;
        result.reserve(raw.size());
        bool inCQ = false;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '[' && i + 1 < raw.size() && raw[i + 1] == 'C' && raw.substr(i, 4) == "[CQ:") {
                inCQ = true;
                continue;
            }
            if (inCQ && raw[i] == ']') {
                inCQ = false;
                continue;
            }
            if (!inCQ) result += raw[i];
        }
        return result;
    }

    static int64_t parseId(const std::string& id) {
        try { return std::stoll(identity::BindingStore::rawQQ(id)); } catch (...) { return 0; }
    }

    // ─── State ───────────────────────────────────────────────

    std::string id_;
    std::string name_;
    std::string endpoint_;
    std::string accessToken_;
    std::string mode_{"forward_ws"};  // forward_ws | reverse_ws | http

    std::string loginId_;
    std::string loginName_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::string lastError_;

    // reconnect backoff state
    std::atomic<int>  reconnectAttempts_{0};   // consecutive failed reconnects since last success
    std::atomic<bool> timedOut_{false};        // true once auto-reconnect is paused (status "timeout")
    // stability timer — only reset reconnectAttempts_ after stable connection
    static constexpr double kStableAfterSec = 30.0;   // seconds before resetting reconnectAttempts_
    std::shared_ptr<trantor::TimerId> stabilityTimer_;

    static constexpr double kReconnectDelaySec     = 5.0;   // attempts 1..10
    static constexpr double kSlowReconnectDelaySec = 60.0;  // attempts 11..20
    static constexpr int    kFastRetryAttempts     = 10;    // > this → 1-min interval
    static constexpr int    kPauseAfterAttempts    = 20;    // > this → pause (timeout)

    drogon::WebSocketClientPtr wsClient_;
    // Reverse-WS server
    std::shared_ptr<trantor::TcpServer> tcpServer_;
    std::shared_ptr<trantor::EventLoopThread> revLoop_; // dedicated event loop for reverse WS
    std::mutex revConnMutex_;
    trantor::TcpConnectionPtr revConn_; // the active OneBot client connection
    std::string revBuffer_; // persistent accumulator for incoming bytes (frame reassembly)
    std::string revFrag_;   // reassembled payload across fragmented (FIN=0) frames
    std::vector<MessageCallback> messageCallbacks_;
    std::vector<EventCallback> eventCallbacks_;
    std::map<std::string, std::string> groupNames_;
    std::atomic<uint64_t> echoSeq_{0};
    std::atomic<bool> refreshTimerStarted_{false};  // 1-min group-name refresh guard

    // Caches for web group-management (filled from get_group_list / member APIs).
    mutable std::mutex dataMutex_;
    std::map<std::string, std::string> groupList_;   // joined groups: gid -> name
    std::set<std::string> ginfoInflight_;            // get_group_info 反查去重（gid 在查中）
    std::map<std::string, int> groupCount_;          // gid -> member count
    int friendCount_ = -1;                            // 好友数量（-1=未知/未同步）
    std::vector<std::string> friendList_;             // 好友 uid 列表（flist 同步）
    std::map<std::string, std::string> selfRole_;    // gid -> owner|admin|member
    std::map<std::string, json> memberLists_;        // gid -> members array
    std::map<std::string, std::string> memberRoles_;  // gid\x1fuid -> owner|admin|member
    std::map<std::string, std::chrono::steady_clock::time_point> roleRefreshAt_;  // 定向查角色限频

    /// Cached role for one member: targeted lookup first, then the full member
    /// list (get_group_member_list) if it has already been fetched.
    std::string memberRole(const std::string& groupId, const std::string& userId) const {
        std::lock_guard<std::mutex> lk(dataMutex_);
        if (!groupId.empty() && !userId.empty()) {
            auto it = memberRoles_.find(groupId + "\x1f" + userId);
            if (it != memberRoles_.end()) return it->second;
        }
        auto lit = memberLists_.find(groupId);
        if (lit != memberLists_.end() && lit->second.is_array()) {
            for (const auto& m : lit->second) {
                if (!m.is_object()) continue;
                if (jsonField(m, "user_id") == userId) return m.value("role", std::string());
            }
        }
        return {};
    }

    // Pending group-file uploads awaiting a possible base64 fallback. Keyed by the
    // echo of the local-path attempt; if that fails we resend the content as
    // base64:// (cross-container/cross-device, no shared filesystem needed).
    struct PendingUpload { std::string groupId, name, content; };
    std::map<std::string, PendingUpload> pendingUploads_;  // guarded by dataMutex_

    // 同步调用（invokeAction）等待中的 promise（echo → promise）。
    mutable std::mutex invokeMutex_;
    std::map<std::string, std::shared_ptr<std::promise<json>>> pendingInvokes_;
};

} // namespace dice
