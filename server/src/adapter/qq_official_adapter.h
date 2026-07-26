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
        if(t=="C2C_MESSAGE_CREATE"){m.type=MessageType::kPrivate;m.senderId=author.value("user_openid",std::string());m.senderName=author.value("username",m.senderId);m.targetId=m.senderId;}
        else if(t=="GROUP_AT_MESSAGE_CREATE"||t=="GROUP_MESSAGE_CREATE"){m.type=MessageType::kGroup;m.senderId=author.value("member_openid",std::string());m.senderName=author.value("username",m.senderId);m.targetId=d.value("group_openid",std::string());m.extra["group_id"]=d.value("group_id",std::string());
            // GROUP_AT_MESSAGE_CREATE is delivered only to the bot which was @ed.
            // Its payload is allowed to omit mentions, so the event itself must be
            // treated as an explicit mention; this also lets @ wake a disabled bot.
            if(t=="GROUP_AT_MESSAGE_CREATE"&&!loginId_.empty())m.atList.push_back(loginId_);
            if(d.contains("mentions")&&d["mentions"].is_array()){m.extra["mentions"]=d["mentions"];for(const auto& mention:d["mentions"]){const auto id=mention.value("member_openid",mention.value("user_openid",mention.value("id",std::string()))); const bool mine=mention.value("is_you",false)||id==loginId_||id==appId_||mention.value("bot_appid",std::string())==appId_; if(mine)m.atList.push_back(loginId_);else if(!id.empty())m.atList.push_back(id);}}
            DICE_LOG_INFO("QQOfficial '{}': inbound {} group={} sender={} atSelf={} textBytes={}", name_, t, m.targetId, m.senderId, !m.atList.empty(), m.content.size());
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
    void sendTo(const Message&m,const std::string&text){ if(accessToken_.empty()){lastError_="QQ 官方机器人尚未取得 AccessToken";return;} std::string target; if(m.extra.is_object())target=m.extra.value("__identity_native_target",std::string()); if(target.empty()&&m.type!=MessageType::kChannel&&db_) target=identity::BindingStore::instance().officialTransport(*db_,appId_,m.targetId,m.type==MessageType::kPrivate?identity::Kind::User:identity::Kind::Group); if(target.empty())target=m.targetId; std::string path; if(m.type==MessageType::kPrivate)path="/v2/users/"+target+"/messages"; else if(m.type==MessageType::kGroup)path="/v2/groups/"+target+"/messages"; else path="/channels/"+target+"/messages"; auto c=httpsClient("api.sgroup.qq.com");if(!c){lastError_="无法解析 api.sgroup.qq.com";return;}auto r=drogon::HttpRequest::newHttpRequest();r->setMethod(drogon::Post);r->setPath(path);r->setContentTypeCode(drogon::CT_APPLICATION_JSON);r->addHeader("Host","api.sgroup.qq.com");r->addHeader("Authorization","QQBot "+accessToken_);json body={{"content",text}};if(m.type!=MessageType::kChannel)body["msg_type"]=0;if(!m.id.empty()){body["msg_id"]=m.id;const int seq=nextReplySeq(m.id);if(seq>0&&m.type!=MessageType::kChannel)body["msg_seq"]=seq;}else if(m.type!=MessageType::kChannel){const auto eventId=takePassiveEvent(m.type,target);if(!eventId.empty())body["event_id"]=eventId;}r->setBody(body.dump());c->sendRequest(r,[self=shared_from_this(),path](drogon::ReqResult rr,const drogon::HttpResponsePtr&resp){if(rr!=drogon::ReqResult::Ok||!resp||resp->statusCode()>=300){std::string detail="网络请求未完成";if(resp){const auto responseBody=resp->body();detail.assign(responseBody.data(),responseBody.size());}self->lastError_="QQ 官方消息发送失败";if(resp)DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}",self->name_,path,static_cast<int>(resp->statusCode()),detail);else DICE_LOG_WARN("QQOfficial '{}': POST {} failed: {} {}",self->name_,path,drogon::to_string(rr),detail);}}); }
    void fail(const std::string&e){lastError_=e;connecting_=false;connected_=false;DICE_LOG_ERROR("QQOfficial '{}': {}",name_,e);}
    std::string id_,name_,appId_,appSecret_,displayQQ_,accessToken_,loginId_,loginName_,sessionId_,gatewayUrl_,lastError_; Database* db_{identity::BindingStore::instance().database()}; std::atomic<bool> connected_{false},connecting_{false},stopping_{false}; int64_t seq_=-1; std::shared_ptr<QQGatewaySocket> gateway_; std::optional<trantor::TimerId> heartbeatTimer_,accessTokenTimer_; MessageCallback messageCb_; EventCallback eventCb_; std::mutex replyMu_; std::unordered_map<std::string,int> replySeq_; std::unordered_map<std::string,std::pair<std::string,std::time_t>> pendingEvents_;
};
}
