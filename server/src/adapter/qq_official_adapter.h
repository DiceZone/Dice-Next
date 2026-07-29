#pragma once

// QQ Bot 2.0 official OpenAPI adapter.  This implementation deliberately uses
// Gateway WebSocket only; no webhook listener is registered.

#include "adapter_interface.h"
#include "qq_gateway_socket.h"
#include "../core/identity/identity_binding.h"
#include "../common/logger.h"

#include <drogon/HttpClient.h>
#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <map>
#include <optional>
#include <stdexcept>
#include <ctime>
#include <unordered_map>
#include <cstring>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace dice {

class QQOfficialAdapter final : public IAdapter,
                                public std::enable_shared_from_this<QQOfficialAdapter> {
public:
    explicit QQOfficialAdapter(std::string adapterId) : id_(std::move(adapterId)) {}
    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string platform() const override { return "qq_official"; }
    const std::string& appId() const noexcept { return appId_; }
    const std::string& displayQQ() const noexcept { return displayQQ_; }
    std::string version() const override { return "qqbot-2.0"; }
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
        name_ = cfg.value("name", std::string("QQ 官方机器人"));
        appId_ = cfg.value("appId", std::string());
        appSecret_ = cfg.value("appSecret", std::string());
        displayQQ_ = cfg.value("qqNumber", std::string());
        if (appId_.empty() || appSecret_.empty()) { lastError_ = "QQ 官方机器人需要 AppID 和 AppSecret"; return false; }
        return true;
    }

    bool start() override {
        if (connecting_ || connected_) return true;
        if (appId_.empty() || appSecret_.empty()) { lastError_ = "QQ 官方机器人未配置 AppID/AppSecret"; return false; }
        stopping_ = false;
        connecting_ = true;
        fetchAccessToken();
        return true;
    }

    void stop() override {
        stopping_ = true; connecting_ = false; connected_ = false;
        if (heartbeatTimer_) { drogon::app().getLoop()->invalidateTimer(*heartbeatTimer_); heartbeatTimer_.reset(); }
        if (accessTokenTimer_) { drogon::app().getLoop()->invalidateTimer(*accessTokenTimer_); accessTokenTimer_.reset(); }
        if (gateway_) gateway_->stop();
        gateway_.reset();
    }

    void sendMessage(const Message& msg) override { sendTo(msg, msg.content); }
    void sendReply(const Message& original, const std::string& text) override { sendTo(original, text); }
    void sendGroupMessage(const std::string& groupId, const std::string& text) override {
        Message m; m.type = MessageType::kGroup; m.targetId = groupId; sendTo(m, text);
    }
    void sendPrivateMessage(const std::string& userId, const std::string& text) override {
        Message m; m.type = MessageType::kPrivate; m.targetId = userId; sendTo(m, text);
    }

    /// 本地图片 → 公网 URL 的发布器（main.cpp 注入，走 系统设置→图床）。
    /// 官方富媒体接口只接受公网 url（file_data 官方「暂未支持」），本地图必须先发布。
    static void setImagePublisher(std::function<std::string(const std::string&)> fn) { imagePublisher_ = std::move(fn); }

    struct QrResult { bool ok=false; json data; std::string error; };
    using QrCallback = std::function<void(QrResult)>;

    // The official connector SDK uses exactly these two HTTPS endpoints.  The
    // AES key never leaves this process; the browser only receives a session id
    // and the official QR URL.
    static void beginQrLogin(QrCallback cb) {
        std::string key(32, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), 32) != 1) { cb({false,{},"无法生成扫码密钥"}); return; }
        auto client = httpsClient("q.qq.com"); if(!client){cb({false,{},"无法解析 q.qq.com"});return;}
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post); req->setPath("/lite/create_bind_task");
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->addHeader("Accept", "application/json");
        req->addHeader("User-Agent", "DiceNext-QQBot/3.0");
        req->addHeader("Host", "q.qq.com");
        req->setBody(json{{"key", drogon::utils::base64Encode(key)}}.dump());
        client->sendRequest(req, [key=std::move(key), cb=std::move(cb)](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) mutable {
            try {
                if (r != drogon::ReqResult::Ok || !resp) {
                    DICE_LOG_WARN("QQOfficial QR task request failed: result={}", static_cast<int>(r));
                    throw std::runtime_error("创建扫码任务失败（网络请求未完成）");
                }
                if (resp->statusCode() >= 300) {
                    DICE_LOG_WARN("QQOfficial QR task request failed: HTTP {}", static_cast<int>(resp->statusCode()));
                    throw std::runtime_error("创建扫码任务失败（QQ 服务返回 HTTP " + std::to_string(static_cast<int>(resp->statusCode())) + "）");
                }
                auto j = json::parse(resp->body());
                if (j.value("retcode", -1) != 0) throw std::runtime_error(j.value("msg", std::string("创建扫码任务失败")));
                std::string task = j.at("data").at("task_id").get<std::string>();
                std::string sid = randomSessionId();
                { std::lock_guard lock(qrMutex_); qrSessions_[sid] = {task, std::move(key), std::time(nullptr)}; }
                cb({true, {{"sessionId",sid},{"url","https://q.qq.com/qqbot/openclaw/connect.html?task_id=" + task + "&source=DiceNext&_wv=2"}}, {}});
            } catch (const std::exception& e) { cb({false,{},e.what()}); }
        });
    }

    static void pollQrLogin(const std::string& sid, QrCallback cb) {
        QrSession session;
        { std::lock_guard lock(qrMutex_); auto it=qrSessions_.find(sid); if(it==qrSessions_.end()) { cb({false,{},"扫码会话不存在或已过期"}); return; } session=it->second; }
        auto client=httpsClient("q.qq.com"); if(!client){cb({false,{},"无法解析 q.qq.com"});return;} auto req=drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post); req->setPath("/lite/poll_bind_result"); req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->addHeader("Accept", "application/json"); req->addHeader("User-Agent", "DiceNext-QQBot/3.0"); req->addHeader("Host", "q.qq.com");
        req->setBody(json{{"task_id",session.taskId}}.dump());
        client->sendRequest(req, [sid,session=std::move(session),cb=std::move(cb)](drogon::ReqResult r,const drogon::HttpResponsePtr& resp) mutable {
            try {
                if(r!=drogon::ReqResult::Ok||!resp) { DICE_LOG_WARN("QQOfficial QR poll failed: result={}", static_cast<int>(r)); throw std::runtime_error("查询扫码状态失败（网络请求未完成）"); }
                if(resp->statusCode()>=300) throw std::runtime_error("查询扫码状态失败（QQ 服务返回 HTTP "+std::to_string(static_cast<int>(resp->statusCode()))+"）"); auto j=json::parse(resp->body());
                if(j.value("retcode",-1)!=0) throw std::runtime_error(j.value("msg",std::string("查询扫码状态失败")));
                auto d=j.value("data",json::object()); int status=d.value("status",0);
                if(status==1) { cb({true,{{"status","pending"}}, {}}); return; }
                if(status==3) { std::lock_guard lock(qrMutex_); qrSessions_.erase(sid); cb({true,{{"status","expired"}}, {}}); return; }
                if(status!=2) { cb({true,{{"status","pending"}}, {}}); return; }
                std::string secret=decryptSecret(d.value("bot_encrypt_secret",std::string()),session.key);
                if(secret.empty()) throw std::runtime_error("扫码凭据解密失败");
                { std::lock_guard lock(qrMutex_); qrSessions_.erase(sid); }
                cb({true,{{"status","completed"},{"appId",d.value("bot_appid",std::string())},{"appSecret",secret}}, {}});
            } catch(const std::exception& e){ cb({false,{},e.what()}); }
        });
    }

private:
    struct QrSession { std::string taskId, key; std::time_t createdAt{}; };
    inline static std::mutex qrMutex_;
    inline static std::unordered_map<std::string,QrSession> qrSessions_;
    static std::string resolveIpv4(const std::string& host) {
        addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
        addrinfo* results=nullptr; if(getaddrinfo(host.c_str(),nullptr,&hints,&results)!=0 || !results) return {};
        char text[INET_ADDRSTRLEN]{}; auto* addr=reinterpret_cast<sockaddr_in*>(results->ai_addr);
        const char* result=inet_ntop(AF_INET,&addr->sin_addr,text,sizeof(text)); freeaddrinfo(results); return result?std::string(result):std::string();
    }
    static drogon::HttpClientPtr httpsClient(const std::string& host) {
        const auto ip=resolveIpv4(host); return ip.empty()?nullptr:drogon::HttpClient::newHttpClient(ip,443,true);
    }
    static std::string randomSessionId() { std::string raw(18,'\0'); RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()),(int)raw.size()); return drogon::utils::base64Encode(raw); }
    static std::string decryptSecret(const std::string& cipher64,const std::string& key) {
        auto raw=drogon::utils::base64Decode(cipher64); if(raw.size()<29||key.size()!=32) return {};
        const unsigned char* iv=reinterpret_cast<const unsigned char*>(raw.data()); const unsigned char* tag=reinterpret_cast<const unsigned char*>(raw.data()+raw.size()-16);
        const unsigned char* enc=reinterpret_cast<const unsigned char*>(raw.data()+12); int encLen=(int)raw.size()-28, len=0, total=0; std::string out(encLen,'\0');
        EVP_CIPHER_CTX* c=EVP_CIPHER_CTX_new(); if(!c) return {}; bool ok=EVP_DecryptInit_ex(c,EVP_aes_256_gcm(),nullptr,nullptr,nullptr)==1
            && EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_IVLEN,12,nullptr)==1
            && EVP_DecryptInit_ex(c,nullptr,nullptr,reinterpret_cast<const unsigned char*>(key.data()),iv)==1
            && EVP_DecryptUpdate(c,reinterpret_cast<unsigned char*>(out.data()),&len,enc,encLen)==1;
        total=len; ok=ok && EVP_CIPHER_CTX_ctrl(c,EVP_CTRL_GCM_SET_TAG,16,const_cast<unsigned char*>(tag))==1 && EVP_DecryptFinal_ex(c,reinterpret_cast<unsigned char*>(out.data())+total,&len)==1; EVP_CIPHER_CTX_free(c); if(!ok)return {}; out.resize(total+len); return out;
    }
    void fetchAccessToken() {
        auto self=shared_from_this(); auto c=httpsClient("bots.qq.com"); if(!c){fail("无法解析 bots.qq.com");return;} auto r=drogon::HttpRequest::newHttpRequest(); r->setMethod(drogon::Post); r->setPath("/app/getAppAccessToken"); r->setContentTypeCode(drogon::CT_APPLICATION_JSON); r->addHeader("Host","bots.qq.com"); r->setBody(json{{"appId",appId_},{"clientSecret",appSecret_}}.dump());
        c->sendRequest(r,[self](drogon::ReqResult rr,const drogon::HttpResponsePtr& resp){ try { if(self->stopping_) return; if(rr!=drogon::ReqResult::Ok||!resp||resp->statusCode()>=300) throw std::runtime_error("获取 AccessToken 失败"); auto j=json::parse(resp->body()); self->accessToken_=j.at("access_token").get<std::string>(); int expiresIn=7200; if(j.contains("expires_in")){const auto& v=j["expires_in"]; if(v.is_number_integer()||v.is_number_unsigned()) expiresIn=v.get<int>(); else if(v.is_string()) try{expiresIn=std::stoi(v.get<std::string>());}catch(...){}} self->scheduleTokenRefresh(expiresIn); if(!self->connected_) self->openGateway(); } catch(const std::exception& e){ self->fail(e.what()); }});
    }
    void openGateway() {
        auto self=shared_from_this(); auto c=httpsClient("api.sgroup.qq.com"); if(!c){fail("无法解析 api.sgroup.qq.com");return;} auto r=drogon::HttpRequest::newHttpRequest(); r->setPath("/gateway/bot"); r->addHeader("Host","api.sgroup.qq.com"); r->addHeader("Authorization","QQBot "+accessToken_);
        c->sendRequest(r,[self](drogon::ReqResult rr,const drogon::HttpResponsePtr& resp){ try { if(rr!=drogon::ReqResult::Ok||!resp) throw std::runtime_error("获取 Gateway 地址失败（网络请求未完成）"); const auto body=resp->body(); auto j=json::parse(body); if(resp->statusCode()>=300||!j.contains("url")||!j["url"].is_string()){DICE_LOG_WARN("QQOfficial Gateway response: HTTP {} {}",static_cast<int>(resp->statusCode()),body); throw std::runtime_error("获取 Gateway 地址失败（请查看后台日志中的 Gateway 响应）");} self->connectGateway(j["url"].get<std::string>()); } catch(const std::exception&e){self->fail(e.what());} });
    }
    void connectGateway(const std::string& url) {
        gatewayUrl_=url;
        const std::string prefix="wss://"; if(url.rfind(prefix,0)!=0){fail("QQ Gateway 地址格式无效");return;} const auto slash=url.find('/',prefix.size()); const std::string authority=url.substr(prefix.size(),slash==std::string::npos?std::string::npos:slash-prefix.size()); const auto colon=authority.rfind(':'); const std::string host=colon==std::string::npos?authority:authority.substr(0,colon); uint16_t port=443; try{if(colon!=std::string::npos)port=static_cast<uint16_t>(std::stoi(authority.substr(colon+1)));}catch(...){fail("QQ Gateway 端口无效");return;} const std::string path=slash==std::string::npos?"/":url.substr(slash);
        auto self=shared_from_this();
        // QQ's Gateway CDN requires the hostname in TLS SNI.  This transport
        // resolves through the OS, avoiding c-ares VPN/CNAME failures without
        // sacrificing SNI by connecting to a bare IP address.
        if (gateway_) gateway_->stop();
        gateway_=std::make_shared<QQGatewaySocket>(host,port,path,
            [self](std::string raw){ if(!self->stopping_) self->onGateway(raw); },
            [self](const std::string& error){ if(!self->stopping_) self->fail("连接 QQ Gateway 失败："+error); },
            [self]{ if(!self->stopping_){ self->connected_=false; self->connecting_=false; }});
        gateway_->start();
    }
    void onGateway(const std::string& raw) {
        try { auto p=json::parse(raw); if(p.contains("s")&&!p["s"].is_null()) seq_=p["s"].get<int64_t>(); int op=p.value("op",-1); if(op==10){ if(!sessionId_.empty() && seq_>=0) resume(); else identify(); return; } if(op==7){ scheduleGatewayReconnect(); return; } if(op==0){ auto t=p.value("t",std::string()); auto d=p.value("d",json::object()); if(t=="READY"){sessionId_=d.value("session_id",std::string()); auto u=d.value("user",json::object()); loginId_=u.value("id",std::string()); loginName_=u.value("username",std::string()); connected_=true; connecting_=false; beginHeartbeat(p["d"].value("heartbeat_interval",45000));} else if(t=="RESUMED"){connected_=true; connecting_=false;} else dispatch(t,d,p.value("id",std::string())); }} catch(const std::exception&e){DICE_LOG_WARN("QQOfficial parse: {}",e.what());}
    }
    void identify(){ if(gateway_) gateway_->sendText(json{{"op",2},{"d",{{"token","QQBot "+accessToken_},{"intents",(1<<25)|(1<<26)|(1<<30)},{"shard",json::array({0,1})},{"properties",{{"$os","DiceNext"},{"$browser","DiceNext"},{"$device","DiceNext"}}}}}}.dump()); }
    void resume(){ if(gateway_) gateway_->sendText(json{{"op",6},{"d",{{"token","QQBot "+accessToken_},{"session_id",sessionId_},{"seq",seq_}}}}.dump()); }
    void scheduleGatewayReconnect(){ if(stopping_||gatewayUrl_.empty()) return; connected_=false; connecting_=true; auto self=shared_from_this(); drogon::app().getLoop()->runAfter(5.0,[self]{ if(!self->stopping_) self->connectGateway(self->gatewayUrl_); }); }
    void scheduleTokenRefresh(int expiresIn){ if(accessTokenTimer_) drogon::app().getLoop()->invalidateTimer(*accessTokenTimer_); const int refreshIn=expiresIn>300?expiresIn-300:60; auto self=shared_from_this(); accessTokenTimer_=drogon::app().getLoop()->runAfter(static_cast<double>(refreshIn),[self]{ if(!self->stopping_) self->fetchAccessToken(); }); }
    void beginHeartbeat(int ms){ if(heartbeatTimer_)drogon::app().getLoop()->invalidateTimer(*heartbeatTimer_); auto self=shared_from_this(); const int intervalMs=ms>0?ms:1000; heartbeatTimer_=drogon::app().getLoop()->runEvery(static_cast<double>(intervalMs)/1000.0,[self]{if(self->gateway_)self->gateway_->sendText(json{{"op",1},{"d",self->seq_>=0?json(self->seq_):json(nullptr)}}.dump());}); }
    void dispatch(const std::string&t,const json&d,const std::string&eventId){
        Message m; m.platform=platform();m.adapterId=id_;m.selfId=loginId_;m.id=d.value("id",eventId);m.content=d.value("content",std::string());m.rawContent=m.content;m.displayContent=m.content;m.timestamp=std::time(nullptr);m.extra={{"event_id",eventId},{"event_type",t},{"official_bot_id",appId_}};
        const auto author=d.value("author",json::object());
        if(t=="C2C_MESSAGE_CREATE"){m.type=MessageType::kPrivate;m.senderId=author.value("user_openid",std::string());m.senderName=author.value("username",std::string());m.targetId=m.senderId;}
        else if(t=="GROUP_AT_MESSAGE_CREATE"||t=="GROUP_MESSAGE_CREATE"){m.type=MessageType::kGroup;m.senderId=author.value("member_openid",std::string());m.senderName=author.value("username",std::string());m.targetId=d.value("group_openid",std::string());m.extra["group_id"]=d.value("group_id",std::string());
            // GROUP_AT_MESSAGE_CREATE is delivered only to the bot which was @ed.
            // Its payload is allowed to omit mentions, so the event itself must be
            // treated as an explicit mention; this also lets @ wake a disabled bot.
            if(t=="GROUP_AT_MESSAGE_CREATE"&&!loginId_.empty())m.atList.push_back(loginId_);
            // 官方正文会保留 <@OpenID>；若不移除，Dice! 的命令前缀不在首位，
            // 例如「@机器人 .r」会被当作普通文本。仅剥离明确指向本机器人的标记。
            auto eraseMention = [&m](const std::string& who) {
                if (who.empty()) return;
                for (const std::string& token : {"<@" + who + ">", "<@!" + who + ">"}) {
                    size_t pos = 0;
                    while ((pos = m.content.find(token, pos)) != std::string::npos) m.content.erase(pos, token.size());
                }
            };
            if(d.contains("mentions")&&d["mentions"].is_array()){m.extra["mentions"]=d["mentions"];for(const auto& mention:d["mentions"]){const auto id=mention.value("member_openid",mention.value("user_openid",mention.value("id",std::string()))); const bool mine=mention.value("is_you",false)||id==loginId_||id==appId_||mention.value("bot_appid",mention.value("bot_app_id",std::string()))==appId_; if(mine){m.atList.push_back(loginId_); eraseMention(id); eraseMention(loginId_); eraseMention(appId_);}else if(!id.empty())m.atList.push_back(id);}}
            // 有些 GROUP_AT_MESSAGE_CREATE 载荷不会带 mentions，只能安全地移除正文最前
            // 的一个 @ 标记；事件类型本身已证明它指向当前机器人。
            if(t=="GROUP_AT_MESSAGE_CREATE"){
                const auto begin=m.content.find_first_not_of(" \t\r\n");
                if(begin!=std::string::npos && m.content.compare(begin,2,"<@") == 0){
                    const auto end=m.content.find('>',begin+2);
                    if(end!=std::string::npos) m.content.erase(begin,end-begin+1);
                }
            }
            const auto first=m.content.find_first_not_of(" \t\r\n");
            if(first==std::string::npos)m.content.clear();
            else { const auto last=m.content.find_last_not_of(" \t\r\n"); m.content=m.content.substr(first,last-first+1); }
            m.rawContent=m.content;m.displayContent=m.content;
            DICE_LOG_INFO("QQOfficial '{}': inbound {} group={} sender={} atSelf={} textBytes={}", name_, t, m.targetId, m.senderId, !m.atList.empty(), m.content.size());
            DICE_LOG_INFO("收↩ [官方群 OpenID({})] {}({}): {}", m.targetId,
                          m.senderName.empty() ? std::string("—") : m.senderName,
                          m.senderId, m.content.empty() ? std::string("[富媒体或空消息]") : m.content);
        }
        else if(t=="AT_MESSAGE_CREATE"){m.type=MessageType::kChannel;m.senderId=author.value("id",std::string());m.senderName=author.value("username",m.senderId);m.targetId=d.value("channel_id",std::string());}
        else { dispatchEvent(t,d,eventId); return; }
        if(d.contains("attachments"))m.extra["attachments"]=d["attachments"]; if(d.contains("msg_elements"))m.extra["msg_elements"]=d["msg_elements"]; if(messageCb_)messageCb_(m);
    }
    void dispatchEvent(const std::string& type,const json& data,const std::string& eventId){
        BotEvent ev; ev.platform=platform(); ev.adapterId=id_; ev.selfId=loginId_; ev.timestamp=data.value("timestamp",static_cast<int64_t>(std::time(nullptr))); ev.extra={{"event_id",eventId},{"event_type",type},{"data",data}};
        if(type=="GROUP_ADD_ROBOT"||type=="GROUP_DEL_ROBOT"){ev.type=type=="GROUP_ADD_ROBOT"?EventType::kGroupIncrease:EventType::kGroupDecrease;ev.groupId=data.value("group_openid",std::string());ev.userId=loginId_;ev.operatorId=data.value("op_member_openid",std::string());rememberPassiveEvent(MessageType::kGroup,ev.groupId,eventId);}
        else if(type=="FRIEND_ADD"||type=="FRIEND_DEL"){ev.type=type=="FRIEND_ADD"?EventType::kFriendAdd:EventType::kOther;ev.userId=data.value("openid",std::string());if(type=="FRIEND_ADD")rememberPassiveEvent(MessageType::kPrivate,ev.userId,eventId);}
        else if(type=="GROUP_MSG_RECEIVE"||type=="GROUP_MSG_REJECT"){ev.groupId=data.value("group_openid",std::string());ev.operatorId=data.value("op_member_openid",std::string());if(type=="GROUP_MSG_RECEIVE")rememberPassiveEvent(MessageType::kGroup,ev.groupId,eventId);}
        else if(type=="C2C_MSG_RECEIVE"||type=="C2C_MSG_REJECT"){ev.userId=data.value("openid",std::string());if(type=="C2C_MSG_RECEIVE")rememberPassiveEvent(MessageType::kPrivate,ev.userId,eventId);}
        if(eventCb_)eventCb_(ev);
    }
    static std::string eventKey(MessageType type,const std::string& target){return std::to_string(static_cast<int>(type))+":"+target;}
    void rememberPassiveEvent(MessageType type,const std::string& target,const std::string& eventId){if(target.empty()||eventId.empty())return;std::lock_guard lock(replyMu_);pendingEvents_[eventKey(type,target)]={eventId,std::time(nullptr)};}
    std::string takePassiveEvent(MessageType type,const std::string& target){std::lock_guard lock(replyMu_);auto it=pendingEvents_.find(eventKey(type,target));if(it==pendingEvents_.end()||std::time(nullptr)-it->second.second>300)return {};auto id=it->second.first;pendingEvents_.erase(it);return id;}
    int nextReplySeq(const std::string& messageId){if(messageId.empty())return 0;std::lock_guard lock(replyMu_);auto& seq=replySeq_[messageId];if(seq<=0)seq=1;return seq++;}

    // ── 富媒体（图片）───────────────────────────────────────────
    struct SplitText { std::string text; std::vector<std::string> images; };
    /// 把 [img,file=..] / [CQ:image,file=..,url=..] 标记从文本中拆出（url 优先）。
    static SplitText splitImages(const std::string& text) {
        SplitText out;
        size_t i = 0;
        while (i < text.size()) {
            size_t tag = std::string::npos; size_t head = 0;
            const size_t a = text.find("[img,", i), b = text.find("[CQ:image,", i);
            if (a != std::string::npos && (b == std::string::npos || a < b)) { tag = a; head = 5; }
            else if (b != std::string::npos) { tag = b; head = 10; }
            if (tag == std::string::npos) { out.text += text.substr(i); break; }
            out.text += text.substr(i, tag - i);
            const size_t end = text.find(']', tag);
            if (end == std::string::npos) { out.text += text.substr(tag); break; }
            const std::string seg = text.substr(tag + head, end - tag - head);   // k=v,k=v
            auto param = [&seg](const std::string& key) -> std::string {
                size_t p = seg.rfind(key + "=", 0) == 0 ? 0 : seg.find("," + key + "=");
                if (p == std::string::npos) return {};
                p = (p == 0) ? key.size() + 1 : p + key.size() + 2;
                const size_t e = seg.find(',', p);
                return seg.substr(p, e == std::string::npos ? std::string::npos : e - p);
            };
            std::string ref = param("url");
            if (ref.empty()) ref = param("file");
            if (!ref.empty()) out.images.push_back(ref);
            i = end + 1;
        }
        return out;
    }

    /// 图片引用 → 公网 URL：外链原样；本机 /api/assets|/api/chat/images URL 还原为
    /// 本地路径后经图床发布器；发布器缺席/失败 → 空（调用方记日志）。
    std::string publicImageUrl(const std::string& ref) {
        std::string v = ref;
        if (v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0) {
            auto tailOf = [&v](const char* seg) -> std::string {
                const auto p = v.find(seg);
                return p == std::string::npos ? std::string() : v.substr(p + std::string(seg).size());
            };
            std::string n;
            if (!(n = tailOf("/api/assets/")).empty()) v = "data/assets/" + n;
            else if (!(n = tailOf("/api/chat/images/")).empty()) v = "data/chat/images/" + n;
            else return v;   // 真外链，腾讯可直接拉取
        }
        return imagePublisher_ ? imagePublisher_(v) : std::string();
    }

    /// 富媒体两跳：POST /files 换 file_info → POST /messages (msg_type=7)。
    void sendMediaTo(const Message& m, const std::string& imageRef) {
        if (m.type == MessageType::kChannel) { DICE_LOG_WARN("QQOfficial '{}': 频道富媒体暂未接入，跳过图片", name_); return; }
        const std::string url = publicImageUrl(imageRef);
        if (url.empty()) {
            DICE_LOG_WARN("QQOfficial '{}': 发图跳过——官方接口仅接受公网 URL；请在 系统设置→图床 配置 generic（图床上传）或 local（公网可达的 public_base）模式", name_);
            return;
        }
        if (accessToken_.empty()) { lastError_ = "QQ 官方机器人尚未取得 AccessToken"; return; }
        std::string target;
        if (m.extra.is_object()) target = m.extra.value("__identity_native_target", std::string());
        if (target.empty() && db_)
            target = identity::BindingStore::instance().officialTransport(*db_, appId_, m.targetId,
                m.type == MessageType::kPrivate ? identity::Kind::User : identity::Kind::Group);
        if (target.empty()) target = m.targetId;
        const bool priv = m.type == MessageType::kPrivate;
        const std::string filesPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/files";
        auto c = httpsClient("api.sgroup.qq.com");
        if (!c) { lastError_ = "无法解析 api.sgroup.qq.com"; return; }
        auto r = drogon::HttpRequest::newHttpRequest();
        r->setMethod(drogon::Post); r->setPath(filesPath);
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->addHeader("Host", "api.sgroup.qq.com");
        r->addHeader("Authorization", "QQBot " + accessToken_);
        r->setBody(json{{"file_type", 1}, {"url", url}, {"srv_send_msg", false}}.dump());
        auto self = shared_from_this();
        const std::string msgId = m.id; const MessageType mtype = m.type;
        c->sendRequest(r, [self, priv, target, msgId, mtype, filesPath](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                self->lastError_ = "QQ 官方富媒体上传失败";
                DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, filesPath,
                              resp ? static_cast<int>(resp->statusCode()) : 0, resp ? std::string(resp->body()) : std::string("网络请求未完成"));
                return;
            }
            auto j = json::parse(resp->body(), nullptr, false);
            const std::string fileInfo = j.is_object() ? j.value("file_info", std::string()) : std::string();
            if (fileInfo.empty()) { DICE_LOG_WARN("QQOfficial '{}': /files 未返回 file_info: {}", self->name_, std::string(resp->body())); return; }
            const std::string msgPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/messages";
            // 官方要求 msg_type=7 时 content 需非空（文档注明的占位空格）。
            json body = {{"content", " "}, {"msg_type", 7}, {"media", {{"file_info", fileInfo}}}};
            if (!msgId.empty()) { body["msg_id"] = msgId; const int seq = self->nextReplySeq(msgId); if (seq > 0) body["msg_seq"] = seq; }
            else { const auto ev = self->takePassiveEvent(mtype, target); if (!ev.empty()) body["event_id"] = ev; }
            auto c2 = httpsClient("api.sgroup.qq.com"); if (!c2) return;
            auto r2 = drogon::HttpRequest::newHttpRequest();
            r2->setMethod(drogon::Post); r2->setPath(msgPath);
            r2->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            r2->addHeader("Host", "api.sgroup.qq.com");
            r2->addHeader("Authorization", "QQBot " + self->accessToken_);
            r2->setBody(body.dump());
            c2->sendRequest(r2, [self, msgPath](drogon::ReqResult rr2, const drogon::HttpResponsePtr& resp2) {
                if (rr2 != drogon::ReqResult::Ok || !resp2 || resp2->statusCode() >= 300) {
                    self->lastError_ = "QQ 官方富媒体消息发送失败";
                    DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, msgPath,
                                  resp2 ? static_cast<int>(resp2->statusCode()) : 0, resp2 ? std::string(resp2->body()) : std::string("网络请求未完成"));
                }
            });
        });
    }
    /// 出站入口：拆出图片标记走富媒体，剩余文本走文字消息。
    void sendTo(const Message& m, const std::string& text) {
        auto parts = splitImages(text);
        for (size_t i = 0; i < parts.images.size() && i < 3; ++i) sendMediaTo(m, parts.images[i]);   // 限 3 张防刷频
        std::string plain = parts.text;
        const auto b = plain.find_first_not_of(" \t\r\n");
        plain = (b == std::string::npos) ? std::string() : plain.substr(b, plain.find_last_not_of(" \t\r\n") - b + 1);
        if (plain.empty()) { if (parts.images.empty()) sendTextTo(m, text); return; }   // 纯图不再发空文本
        sendTextTo(m, plain);
    }
    void sendTextTo(const Message&m,const std::string&text){ if(accessToken_.empty()){lastError_="QQ 官方机器人尚未取得 AccessToken";return;} std::string target; if(m.extra.is_object())target=m.extra.value("__identity_native_target",std::string()); if(target.empty()&&m.type!=MessageType::kChannel&&db_) target=identity::BindingStore::instance().officialTransport(*db_,appId_,m.targetId,m.type==MessageType::kPrivate?identity::Kind::User:identity::Kind::Group); if(target.empty())target=m.targetId; std::string path; if(m.type==MessageType::kPrivate)path="/v2/users/"+target+"/messages"; else if(m.type==MessageType::kGroup)path="/v2/groups/"+target+"/messages"; else path="/channels/"+target+"/messages"; auto c=httpsClient("api.sgroup.qq.com");if(!c){lastError_="无法解析 api.sgroup.qq.com";return;}auto r=drogon::HttpRequest::newHttpRequest();r->setMethod(drogon::Post);r->setPath(path);r->setContentTypeCode(drogon::CT_APPLICATION_JSON);r->addHeader("Host","api.sgroup.qq.com");r->addHeader("Authorization","QQBot "+accessToken_);json body={{"content",text}};if(m.type!=MessageType::kChannel)body["msg_type"]=0;if(!m.id.empty()){body["msg_id"]=m.id;const int seq=nextReplySeq(m.id);if(seq>0&&m.type!=MessageType::kChannel)body["msg_seq"]=seq;}else if(m.type!=MessageType::kChannel){const auto eventId=takePassiveEvent(m.type,target);if(!eventId.empty())body["event_id"]=eventId;}r->setBody(body.dump());c->sendRequest(r,[self=shared_from_this(),path](drogon::ReqResult rr,const drogon::HttpResponsePtr&resp){if(rr!=drogon::ReqResult::Ok||!resp||resp->statusCode()>=300){std::string detail="网络请求未完成";if(resp){const auto responseBody=resp->body();detail.assign(responseBody.data(),responseBody.size());}self->lastError_="QQ 官方消息发送失败";if(resp)DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}",self->name_,path,static_cast<int>(resp->statusCode()),detail);else DICE_LOG_WARN("QQOfficial '{}': POST {} failed: {} {}",self->name_,path,drogon::to_string(rr),detail);}}); }
    void fail(const std::string&e){lastError_=e;connecting_=false;connected_=false;DICE_LOG_ERROR("QQOfficial '{}': {}",name_,e);}
    inline static std::function<std::string(const std::string&)> imagePublisher_;   // 本地图 → 公网 URL（图床）
    std::string id_,name_,appId_,appSecret_,displayQQ_,accessToken_,loginId_,loginName_,sessionId_,gatewayUrl_,lastError_; Database* db_{identity::BindingStore::instance().database()}; std::atomic<bool> connected_{false},connecting_{false},stopping_{false}; int64_t seq_=-1; std::shared_ptr<QQGatewaySocket> gateway_; std::optional<trantor::TimerId> heartbeatTimer_,accessTokenTimer_; MessageCallback messageCb_; EventCallback eventCb_; std::mutex replyMu_; std::unordered_map<std::string,int> replySeq_; std::unordered_map<std::string,std::pair<std::string,std::time_t>> pendingEvents_;
};
}
