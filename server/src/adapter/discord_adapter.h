#pragma once

// Discord Bot adapter — Gateway WebSocket (v10) + REST。
// 结构与 QQOfficialAdapter 一致：REST 取网关地址 → WSS 连接 → hello/identify/
// heartbeat → MESSAGE_CREATE 转 Message；发送走 REST POST /channels/{id}/messages。
// 频道消息映射为 kGroup（targetId=channel_id，群功能按频道生效）；私信映射为
// kPrivate（targetId=用户 id，回复经 extra.channel_id 直达 DM 频道）。

#include "adapter_interface.h"
#include "qq_gateway_socket.h"
#include "../core/identity/identity_binding.h"
#include "../common/logger.h"

#include <drogon/HttpClient.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
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

    json capabilities() const override {
        json caps = IAdapter::capabilities();
        caps["kick"] = true;   // DELETE /guilds/{g}/members/{u}
        caps["ban"] = true;    // 超时禁言 communication_disabled_until
        return caps;
    }

    /// 踢出服务器（频道 id → 所属 guild 经消息缓存反查）。
    void setGroupKick(const std::string& channelId, const std::string& userId) override {
        const std::string guild = guildOf(channelId);
        const std::string user = nativeId(userId, identity::Kind::User);
        if (guild.empty() || user.empty()) { lastError_ = "Discord 无法定位服务器（需先在该频道收到过消息）"; return; }
        restRequest("DELETE", "/api/v10/guilds/" + guild + "/members/" + user, json(), nullptr);
    }
    /// 禁言 = Discord 超时（timeout）。durationSec<=0 解除。上限 28 天。
    void setGroupBan(const std::string& channelId, const std::string& userId, int durationSec) override {
        const std::string guild = guildOf(channelId);
        const std::string user = nativeId(userId, identity::Kind::User);
        if (guild.empty() || user.empty()) { lastError_ = "Discord 无法定位服务器（需先在该频道收到过消息）"; return; }
        json body;
        if (durationSec > 0) {
            if (durationSec > 28 * 86400) durationSec = 28 * 86400;
            const std::time_t until = std::time(nullptr) + durationSec;
            char buf[32]{};
            std::tm tmv{};
#ifdef _WIN32
            gmtime_s(&tmv, &until);
#else
            gmtime_r(&until, &tmv);
#endif
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
            body = {{"communication_disabled_until", buf}};
        } else {
            body = {{"communication_disabled_until", nullptr}};
        }
        restRequest("PATCH", "/api/v10/guilds/" + guild + "/members/" + user, body, nullptr);
    }
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
        postChannelMessage(nativeId(channelId, identity::Kind::Group), text);
    }
    void sendPrivateMessage(const std::string& userId, const std::string& text) override {
        const std::string native = nativeId(userId, identity::Kind::User);
        // 需要先建 DM 频道（有缓存则直发）。
        {
            std::lock_guard lock(dmMutex_);
            auto it = dmChannels_.find(native);
            if (it != dmChannels_.end()) { postChannelMessage(it->second, text); return; }
        }
        auto self = shared_from_this();
        restRequest("POST", "/api/v10/users/@me/channels", json{{"recipient_id", native}},
            [self, native, text](const json& resp) {
                const std::string channel = resp.value("id", std::string());
                if (channel.empty()) { self->lastError_ = "Discord 无法创建私信频道"; return; }
                { std::lock_guard lock(self->dmMutex_); self->dmChannels_[native] = channel; }
                self->postChannelMessage(channel, text);
            });
    }

private:
    static std::string runCmdCapture(const std::string& cmd, size_t maxBytes = 2 * 1024 * 1024) {
#if defined(_WIN32)
        FILE* p = _popen(cmd.c_str(), "r");
#else
        FILE* p = popen(cmd.c_str(), "r");
#endif
        if (!p) return "";
        std::string out; char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) { out.append(buf, n); if (out.size() > maxBytes) break; }
#if defined(_WIN32)
        _pclose(p);
#else
        pclose(p);
#endif
        return out;
    }

    /// REST 调用（discord.com），走 curl 子进程 + 独立线程。回调只在 2xx 且 JSON
    /// 解析成功时收到对象。⚠️ 不能用 drogon HttpClient：discord.com 在 Cloudflare
    /// 后（TLS 强制校验 SNI，裸 IP 被拒），而 drogon 的 c-ares 解析器在部分
    /// VPN/公司网络上解析失败——curl 走系统 DNS 且 SNI 正确，两个坑都绕开。
    /// Token 经 curl -K 配置文件传递，不进命令行（进程列表不可见）。
    void restRequest(const std::string& method, const std::string& path, const json& body,
                     std::function<void(const json&)> onOk) {
        auto self = shared_from_this();
        const std::string bodyStr = body.is_null() ? std::string() : body.dump();
        std::thread([self, method, path, bodyStr, onOk = std::move(onOk)] {
            namespace fs = std::filesystem;
            auto esc = [](const std::string& s) {
                std::string o; o.reserve(s.size() + 8);
                for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
                return o;
            };
            static std::atomic<long long> seq{0};
            const long long id = ++seq;
            std::error_code ec;
            fs::path tmp = fs::temp_directory_path(ec);
            fs::path cfgF = tmp / ("dndc_" + std::to_string(id) + ".cfg");
            fs::path bodyF = tmp / ("dndc_" + std::to_string(id) + ".body");
            std::string out;
            try {
                {
                    std::ofstream cf(cfgF, std::ios::binary);
                    cf << "url = \"https://discord.com" << esc(path) << "\"\n";
                    cf << "request = \"" << esc(method) << "\"\n";
                    cf << "max-time = 15\nsilent\nshow-error\n";
                    cf << "proto = \"=https\"\n";
                    cf << "write-out = \"\\n%{http_code}\"\n";
                    cf << "header = \"Authorization: Bot " << esc(self->token_) << "\"\n";
                    cf << "header = \"User-Agent: DiceNext (https://github.com/DiceZone/Dice-Next, 3.0)\"\n";
                    if (!bodyStr.empty()) {
                        cf << "header = \"Content-Type: application/json\"\n";
                        std::ofstream bf(bodyF, std::ios::binary); bf << bodyStr;
                        cf << "data-binary = \"@" << esc(bodyF.string()) << "\"\n";
                    }
                }
                out = runCmdCapture("curl -K \"" + cfgF.string() + "\"");
            } catch (...) {}
            fs::remove(cfgF, ec); fs::remove(bodyF, ec);

            int status = 0;
            const auto nl = out.find_last_of('\n');
            if (nl != std::string::npos) { status = std::atoi(out.c_str() + nl + 1); out.erase(nl); }
            if (status < 200 || status >= 300) {
                self->lastError_ = "Discord 请求失败: " + path;
                self->connecting_ = false;
                DICE_LOG_WARN("Discord '{}': {} failed: HTTP {} {}", self->name_, path, status,
                              out.empty() ? std::string("(无响应，请检查网络与 curl)") : out.substr(0, 300));
                return;
            }
            if (!onOk) return;
            auto j = json::parse(out, nullptr, false);
            if (!j.is_discarded()) onOk(j);
        }).detach();
    }

    void fetchGatewayUrl() {
        auto self = shared_from_this();
        // GET /gateway/bot 同时校验 Token（401 = Token 无效）。
        restRequest("GET", "/api/v10/gateway/bot", json(),
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

    /// 取对象字段；缺失或为 null/非对象时回退空对象（op9 的 d 是布尔等）。
    static json objOf(const json& j, const char* key) {
        if (!j.is_object()) return json::object();
        auto it = j.find(key);
        return (it != j.end() && it->is_object()) ? *it : json::object();
    }

    void onGateway(const std::string& raw) {
        try {
            auto p = json::parse(raw);
            if (p.contains("s") && !p["s"].is_null() && p["s"].is_number_integer()) seq_ = p["s"].get<int64_t>();
            const int op = p.value("op", -1);
            if (op == 10) {   // hello
                beginHeartbeat(objOf(p, "d").value("heartbeat_interval", 41250));
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
                const auto t = p.contains("t") && p["t"].is_string() ? p["t"].get<std::string>() : std::string();
                const auto d = objOf(p, "d");
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
        const auto author = objOf(d, "author");
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
        const auto member = objOf(d, "member");
        if (member.contains("nick") && member["nick"].is_string() && !member["nick"].get<std::string>().empty())
            m.senderName = member["nick"].get<std::string>();
        m.extra = {{"channel_id", d.value("channel_id", std::string())}};
        if (d.contains("guild_id") && d["guild_id"].is_string()) {
            // 频道消息按群处理：targetId=频道 id，群设置/人物卡按频道生效。
            m.type = MessageType::kGroup;
            m.targetId = d.value("channel_id", std::string());
            m.extra["guild_id"] = d["guild_id"];
            // 频道→服务器映射缓存（踢人/禁言等 guild 级操作要用）。
            { std::lock_guard lock(guildMutex_); channelGuild_[m.targetId] = d["guild_id"].get<std::string>(); }
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
        const std::string native = translateCQ(text);
        if (native.empty()) return;
        restRequest("POST", "/api/v10/channels/" + channelId + "/messages", json{{"content", native}}, nullptr);
    }

    /// 频道 → 所属服务器（公共群号先转原生频道号再查缓存）。
    std::string guildOf(const std::string& channelPublicId) {
        const std::string channel = nativeId(channelPublicId, identity::Kind::Group);
        std::lock_guard lock(guildMutex_);
        auto it = channelGuild_.find(channel);
        return it != channelGuild_.end() ? it->second : std::string();
    }

    /// 出站 CQ 码转 Discord 原生：[CQ:at,qq=公共号] → <@原生id>；图片 URL 直贴
    /// （Discord 自动嵌入）；其余 CQ 段丢弃，避免把 [CQ:...] 原文发给用户。
    std::string translateCQ(const std::string& text) {
        std::string out; out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            if (text.compare(i, 4, "[CQ:") != 0) { out += text[i++]; continue; }
            const auto end = text.find(']', i);
            if (end == std::string::npos) { out += text.substr(i); break; }
            const std::string seg = text.substr(i + 4, end - i - 4);   // type,k=v,...
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
                if (qq == "all") out += "@everyone";
                else if (!qq.empty()) out += "<@" + nativeId(qq, identity::Kind::User) + ">";
            } else if (type == "image") {
                const std::string f = param("file"), u = param("url");
                const std::string& link = u.rfind("http", 0) == 0 ? u : f;
                if (link.rfind("http", 0) == 0) out += " " + link + " ";
            }
            // 其余（face/poke/reply/record…）丢弃。
        }
        return out;
    }

    /// 公共号（虚拟/真实 QQ）→ Discord 原生 id。非映射产物（本就是原生 id）原样返回。
    std::string nativeId(const std::string& publicId, identity::Kind kind) {
        if (publicId.empty() || !db_) return publicId;
        auto native = identity::BindingStore::instance().transportEndpoint(*db_, "discord", publicId, kind);
        return native.empty() ? publicId : native;
    }

    void sendTo(const Message& m, const std::string& text) {
        std::string channel;
        if (m.extra.is_object()) channel = m.extra.value("channel_id", std::string());   // 入站原生频道
        if (channel.empty() && m.type == MessageType::kPrivate) { sendPrivateMessage(m.targetId, text); return; }
        if (channel.empty()) channel = nativeId(m.targetId, identity::Kind::Group);
        postChannelMessage(channel, text);
    }

    void fail(const std::string& e) {
        lastError_ = e; connecting_ = false; connected_ = false;
        DICE_LOG_ERROR("Discord '{}': {}", name_, e);
    }

    std::string id_, name_, token_, loginId_, loginName_, sessionId_, gatewayUrl_, lastError_;
    Database* db_{identity::BindingStore::instance().database()};
    std::atomic<bool> connected_{false}, connecting_{false}, stopping_{false};
    int64_t seq_ = -1;
    std::shared_ptr<QQGatewaySocket> gateway_;
    std::optional<trantor::TimerId> heartbeatTimer_;
    MessageCallback messageCb_;
    EventCallback eventCb_;
    std::mutex dmMutex_;
    std::unordered_map<std::string, std::string> dmChannels_;   // userId → DM 频道 id
    std::mutex guildMutex_;
    std::unordered_map<std::string, std::string> channelGuild_; // 频道 id → guild id
};

}  // namespace dice
