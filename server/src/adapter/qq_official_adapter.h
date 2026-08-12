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
#include <cctype>
#include <climits>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <map>
#include <optional>
#include <set>
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
    const std::string& shareUrl() const noexcept { return shareUrl_; }
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
    void setGroupBan(const std::string& groupId, const std::string& userId, int durationSec) override {
        if (groupId.empty() || userId.empty()) return;
        std::string nativeGroup = groupId, nativeUser = userId;
        if (db_) {
            const auto g = identity::BindingStore::instance().officialTransport(*db_, appId_, groupId, identity::Kind::Group);
            const auto u = identity::BindingStore::instance().officialTransport(*db_, appId_, userId, identity::Kind::User);
            if (!g.empty()) nativeGroup = g;
            if (!u.empty()) nativeUser = u;
        }
        const bool remove = durationSec <= 0;
        json body = {{"members", json::array({json{
            {"op", remove ? "del" : "add"},
            {"member_openid", nativeUser},
            {"mute_expire_at", remove ? std::string() : rfc3339After(durationSec)}
        }})}};
        officialApi(drogon::Post, "/v2/groups/" + nativeGroup + "/restrict_chat_setting", body,
            [self = shared_from_this()](json result) {
                if (!result.value("ok", false)) self->lastError_ = result.value("message", std::string("设置官方群禁言失败"));
            });
    }
    void setGroupRequest(const std::string& flag, const std::string& subType,
                         bool approve, const std::string& reason = "") override {
        if (subType != "add") return;
        const auto parts = splitFlag(flag);
        if (parts.size() != 3) { lastError_ = "QQ 官方入群申请标识无效"; return; }
        json body{{"op", approve ? "approve" : "decline"}, {"join_request_id", parts[2]}};
        if (!approve && !reason.empty()) body["reject_reason"] = reason;
        officialApi(drogon::Post, "/v2/groups/" + parts[0] + "/approval_join_request/" + parts[1], body,
            [self = shared_from_this()](json result) {
                if (!result.value("ok", false)) self->lastError_ = result.value("message", std::string("审批官方群入群申请失败"));
            });
    }
    json capabilities() const override {
        json caps = IAdapter::capabilities();
        caps["ban"] = true;
        caps["qq_group_admin"] = true;
        caps["group_join_requests"] = true;
        caps["group_join_strategy"] = true;
        return caps;
    }
    void invokeActionAsync(const std::string& action, const json& params, ActionCallback cb) override {
        const std::string groupId = params.value("groupId", std::string());
        const std::string strategyId = params.value("strategyId", std::string());
        if (action == "qq_get_mute" && !groupId.empty()) {
            officialApi(drogon::Get, "/v2/groups/" + groupId + "/restrict_chat_setting", json::object(), std::move(cb));
        } else if (action == "qq_set_mute" && !groupId.empty()) {
            officialApi(drogon::Post, "/v2/groups/" + groupId + "/restrict_chat_setting",
                        json{{"members", params.value("members", json::array())}}, std::move(cb));
        } else if (action == "qq_join_requests" && !groupId.empty()) {
            json body;
            if (params.contains("cursor")) body["cursor"] = params["cursor"];
            if (params.contains("limit")) body["limit"] = params["limit"];
            officialApi(drogon::Get, "/v2/groups/" + groupId + "/join_request_list", body, std::move(cb));
        } else if (action == "qq_approve_join" && !groupId.empty()) {
            const std::string member = params.value("memberOpenId", std::string());
            if (member.empty()) { cb(apiResult(false, 0, "memberOpenId required")); return; }
            json body{{"op", params.value("op", std::string("approve"))}};
            if (params.contains("joinRequestId")) body["join_request_id"] = params["joinRequestId"];
            if (params.contains("rejectReason")) body["reject_reason"] = params["rejectReason"];
            if (params.contains("addToMemberBlacklist")) body["add_to_member_blacklist"] = params["addToMemberBlacklist"];
            officialApi(drogon::Post, "/v2/groups/" + groupId + "/approval_join_request/" + member, body, std::move(cb));
        } else if (action == "qq_list_join_strategies") {
            json body;
            if (params.contains("cursor")) body["cursor"] = params["cursor"];
            if (params.contains("limit")) body["limit"] = params["limit"];
            officialApi(drogon::Get, "/v2/groups/join_approval_strategy", body, std::move(cb));
        } else if (action == "qq_create_join_strategy") {
            officialApi(drogon::Post, "/v2/groups/join_approval_strategy", params.value("body", json::object()), std::move(cb));
        } else if (action == "qq_update_join_strategy" && !strategyId.empty()) {
            officialApi(drogon::Patch, "/v2/groups/join_approval_strategy/" + strategyId, params.value("body", json::object()), std::move(cb));
        } else if (action == "qq_delete_join_strategy" && !strategyId.empty()) {
            officialApi(drogon::Delete, "/v2/groups/join_approval_strategy/" + strategyId, json::object(), std::move(cb));
        } else if (action == "qq_execute_join_strategy" && !strategyId.empty()) {
            officialApi(drogon::Post, "/v2/groups/join_approval_strategy/" + strategyId + "/execute", json::object(), std::move(cb));
        } else if (action == "qq_update_join_whitelist" && !strategyId.empty()) {
            officialApi(drogon::Post, "/v2/groups/join_approval_strategy/" + strategyId + "/whitelist_users",
                        json{{"op", params.value("op", std::string("add"))},
                             {"whitelist_users", params.value("whitelistUsers", json::array())}}, std::move(cb));
        } else {
            cb(apiResult(false, 0, "unsupported QQ Official action or missing parameter"));
        }
    }
    void onMessage(MessageCallback cb) override { messageCb_ = std::move(cb); }
    void onEvent(EventCallback cb) override { eventCb_ = std::move(cb); }

    bool configure(const json& cfg) override {
        name_ = cfg.value("name", std::string("QQ 官方机器人"));
        appId_ = cfg.value("appId", std::string());
        appSecret_ = cfg.value("appSecret", std::string());
        displayQQ_ = cfg.value("qqNumber", std::string());
        forceVerifyImageResource_ = cfg.value("forceVerifyImageResource", cfg.value("force_verify_image_resource", false));
        setMessageFormatOverride(parseFormatOverride(cfg.value("message_format", std::string())));
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
    static json apiResult(bool ok, int status, const std::string& message = {}, json data = json::object()) {
        return json{{"ok", ok}, {"httpStatus", status}, {"message", message}, {"data", std::move(data)}};
    }
    static std::vector<std::string> splitFlag(const std::string& flag) {
        std::vector<std::string> out; size_t start = 0;
        while (start <= flag.size()) {
            const size_t p = flag.find('\x1f', start);
            out.push_back(flag.substr(start, p == std::string::npos ? std::string::npos : p - start));
            if (p == std::string::npos) break;
            start = p + 1;
        }
        return out;
    }
    static std::string rfc3339After(int durationSec) {
        const std::time_t when = std::time(nullptr) + (std::max)(0, durationSec);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &when);
#else
        gmtime_r(&when, &utc);
#endif
        std::ostringstream out;
        out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }
    void officialApi(drogon::HttpMethod method, const std::string& path, const json& body, ActionCallback cb) {
        if (accessToken_.empty()) { cb(apiResult(false, 0, "QQ 官方机器人尚未取得 AccessToken")); return; }
        auto client = httpsClient("api.bot.qq.com");
        if (!client) { cb(apiResult(false, 0, "无法解析 api.bot.qq.com")); return; }
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        request->setPath(path);
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        request->addHeader("Host", "api.bot.qq.com");
        request->addHeader("Authorization", "QQBot " + accessToken_);
        if (!body.is_null() && !body.empty()) request->setBody(body.dump());
        client->sendRequest(request, [self = shared_from_this(), method, path, cb = std::move(cb)](
            drogon::ReqResult result, const drogon::HttpResponsePtr& response) mutable {
            if (result != drogon::ReqResult::Ok || !response) {
                const std::string message = "QQ 官方接口请求未完成: " + path;
                DICE_LOG_WARN("QQOfficial '{}': {} ({})", self->name_, message, drogon::to_string(result));
                cb(apiResult(false, 0, message));
                return;
            }
            const int status = static_cast<int>(response->statusCode());
            const auto raw = response->body();
            json data = raw.empty() ? json::object() : json::parse(raw, nullptr, false);
            if (data.is_discarded()) data = json{{"raw", std::string(raw)}};
            if (status >= 300) {
                std::string message = "QQ 官方接口返回 HTTP " + std::to_string(status);
                if (data.is_object()) message = data.value("message", message);
                DICE_LOG_WARN("QQOfficial '{}': {} {} failed: HTTP {} {}", self->name_,
                              method == drogon::Get ? "GET" : method == drogon::Post ? "POST" : method == drogon::Patch ? "PATCH" : method == drogon::Delete ? "DELETE" : "HTTP",
                              path, status, std::string(raw));
                cb(apiResult(false, status, message, std::move(data)));
                return;
            }
            cb(apiResult(true, status, {}, std::move(data)));
        });
    }
    void fetchAccessToken() {
        auto self=shared_from_this(); auto c=httpsClient("api.bot.qq.com"); if(!c){fail("无法解析 api.bot.qq.com");return;} auto r=drogon::HttpRequest::newHttpRequest(); r->setMethod(drogon::Post); r->setPath("/app/getAppAccessToken"); r->setContentTypeCode(drogon::CT_APPLICATION_JSON); r->addHeader("Host","api.bot.qq.com"); r->setBody(json{{"appId",appId_},{"clientSecret",appSecret_}}.dump());
        c->sendRequest(r,[self](drogon::ReqResult rr,const drogon::HttpResponsePtr& resp){ try { if(self->stopping_) return; if(rr!=drogon::ReqResult::Ok||!resp||resp->statusCode()>=300) throw std::runtime_error("获取 AccessToken 失败"); auto j=json::parse(resp->body()); self->accessToken_=j.at("access_token").get<std::string>(); int expiresIn=7200; if(j.contains("expires_in")){const auto& v=j["expires_in"]; if(v.is_number_integer()||v.is_number_unsigned()) expiresIn=v.get<int>(); else if(v.is_string()) try{expiresIn=std::stoi(v.get<std::string>());}catch(...){}} self->scheduleTokenRefresh(expiresIn); if(!self->connected_) self->fetchBotProfile(); } catch(const std::exception& e){ self->fail(e.what()); }});
    }
    static std::string queryValue(const std::string& url,const std::string& name){const std::string key=name+"="; auto p=url.find(key); if(p==std::string::npos)return {}; p+=key.size(); auto e=url.find('&',p); return url.substr(p,e==std::string::npos?std::string::npos:e-p);}
    void fetchBotProfile(){
        auto self=shared_from_this(); auto c=httpsClient("api.bot.qq.com"); if(!c){openGateway();return;} auto r=drogon::HttpRequest::newHttpRequest(); r->setPath("/users/@me"); r->addHeader("Host","api.bot.qq.com"); r->addHeader("Authorization","QQBot "+accessToken_);
        c->sendRequest(r,[self](drogon::ReqResult rr,const drogon::HttpResponsePtr& resp){
            try{
                if(rr==drogon::ReqResult::Ok&&resp&&resp->statusCode()<300){auto j=json::parse(resp->body()); self->loginId_=j.value("id",self->loginId_); self->loginName_=j.value("username",self->loginName_); self->shareUrl_=j.value("share_url",std::string()); const std::string realQQ=queryValue(self->shareUrl_,"robot_uin"); if(!realQQ.empty()) self->displayQQ_=realQQ;}
                else DICE_LOG_WARN("QQOfficial '{}': 获取机器人资料失败，将尝试生成分享链接",self->name_);
            }catch(const std::exception&e){DICE_LOG_WARN("QQOfficial '{}': 解析机器人资料失败：{}",self->name_,e.what());}
            if(self->stopping_||self->connected_) return;
            if(self->displayQQ_.empty()||self->shareUrl_.empty()) self->fetchShareUrl(); else self->openGateway();
        });
    }
    void fetchShareUrl(){
        auto self=shared_from_this(); auto c=httpsClient("api.bot.qq.com"); if(!c){openGateway();return;} auto r=drogon::HttpRequest::newHttpRequest(); r->setMethod(drogon::Post); r->setPath("/v2/generate_url_link"); r->setContentTypeCode(drogon::CT_APPLICATION_JSON); r->addHeader("Host","api.bot.qq.com"); r->addHeader("Authorization","QQBot "+accessToken_); r->setBody(json{{"callback_data","dicenext"}}.dump());
        c->sendRequest(r,[self](drogon::ReqResult rr,const drogon::HttpResponsePtr& resp){
            try{if(rr==drogon::ReqResult::Ok&&resp&&resp->statusCode()<300){auto j=json::parse(resp->body()); self->shareUrl_=j.value("url_link",std::string()); const std::string realQQ=queryValue(self->shareUrl_,"robot_uin"); if(!realQQ.empty()) self->displayQQ_=realQQ;} else DICE_LOG_WARN("QQOfficial '{}': 生成机器人分享链接失败，将继续连接 Gateway",self->name_);}catch(const std::exception&e){DICE_LOG_WARN("QQOfficial '{}': 解析机器人分享链接失败：{}",self->name_,e.what());}
            if(!self->stopping_&&!self->connected_) self->openGateway();
        });
    }
    void openGateway() {
        auto self=shared_from_this(); auto c=httpsClient("api.bot.qq.com"); if(!c){fail("无法解析 api.bot.qq.com");return;} auto r=drogon::HttpRequest::newHttpRequest(); r->setPath("/gateway/bot"); r->addHeader("Host","api.bot.qq.com"); r->addHeader("Authorization","QQBot "+accessToken_);
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
            [self]{ if(!self->stopping_){ self->connected_=false; self->connecting_=false; self->scheduleGatewayReconnect(); }});
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
        ev.extra["official_bot_id"] = appId_;   // 供事件层按 bot 映射 OpenID → 公共号
        if(type=="GROUP_ADD_ROBOT"||type=="GROUP_DEL_ROBOT"){ev.type=type=="GROUP_ADD_ROBOT"?EventType::kGroupIncrease:EventType::kGroupDecrease;ev.groupId=data.value("group_openid",std::string());ev.userId=loginId_;ev.operatorId=data.value("op_member_openid",std::string());rememberPassiveEvent(MessageType::kGroup,ev.groupId,eventId);}
        else if(type=="FRIEND_ADD"||type=="FRIEND_DEL"){ev.type=type=="FRIEND_ADD"?EventType::kFriendAdd:EventType::kOther;ev.userId=data.value("openid",std::string());if(type=="FRIEND_ADD")rememberPassiveEvent(MessageType::kPrivate,ev.userId,eventId);}
        else if(type=="GROUP_MSG_RECEIVE"||type=="GROUP_MSG_REJECT"){ev.groupId=data.value("group_openid",std::string());ev.operatorId=data.value("op_member_openid",std::string());if(type=="GROUP_MSG_RECEIVE")rememberPassiveEvent(MessageType::kGroup,ev.groupId,eventId);}
        else if(type=="GROUP_JOIN_REQUEST"){
            ev.type=EventType::kGroupRequest;
            ev.groupId=data.value("group_openid",std::string());
            ev.userId=data.value("member_openid",std::string());
            ev.operatorId=data.value("invited_by",std::string());
            ev.subType="add";
            const std::string requestId=data.value("join_request_id",std::string());
            ev.flag=ev.groupId+'\x1f'+ev.userId+'\x1f'+requestId;
            const auto verify=data.value("verify_info",json::object());
            ev.comment=verify.value("verify_message",std::string());
            if(ev.comment.empty()&&verify.contains("review_qa_list")&&verify["review_qa_list"].is_array()){
                for(const auto& qa:verify["review_qa_list"]){
                    if(!ev.comment.empty())ev.comment+='\n';
                    ev.comment+=qa.value("question",std::string())+"："+qa.value("answer",std::string());
                }
            }
            ev.extra["qq_official_join_request"]=true;
            DICE_LOG_INFO("QQOfficial '{}': group join request group={} member={} request={}",name_,ev.groupId,ev.userId,requestId);
        }
        else if(type=="C2C_MSG_RECEIVE"||type=="C2C_MSG_REJECT"){ev.userId=data.value("openid",std::string());if(type=="C2C_MSG_RECEIVE")rememberPassiveEvent(MessageType::kPrivate,ev.userId,eventId);}
        if(eventCb_)eventCb_(ev);
    }
    static std::string eventKey(MessageType type,const std::string& target){return std::to_string(static_cast<int>(type))+":"+target;}
    void rememberPassiveEvent(MessageType type,const std::string& target,const std::string& eventId){if(target.empty()||eventId.empty())return;std::lock_guard lock(replyMu_);pendingEvents_[eventKey(type,target)]={eventId,std::time(nullptr)};}
    std::string takePassiveEvent(MessageType type,const std::string& target){std::lock_guard lock(replyMu_);auto it=pendingEvents_.find(eventKey(type,target));if(it==pendingEvents_.end()||std::time(nullptr)-it->second.second>300)return {};auto id=it->second.first;pendingEvents_.erase(it);return id;}
    int nextReplySeq(const std::string& messageId){if(messageId.empty())return 0;std::lock_guard lock(replyMu_);auto& seq=replySeq_[messageId];if(seq<=0)seq=1;return seq++;}

    // ── 富媒体（图片/文件/视频/语音）─────────────────────────────
    // QQ Bot 2.0 富媒体两条路：
    //   · 公网 URL 直传：POST /v2/{groups|users}/{id}/files {url,...} → file_info；
    //   · 本地文件分片：upload_prepare → PUT 预签名分片 → upload_part_finish → /files 合并。
    // 本地图片不再依赖图床，直接走官方分片上传。
    struct MediaItem { int kind = 4; std::string ref; std::string name; };   // 1=图 2=视频 3=语音 4=文件
    struct SplitText { std::string text; std::vector<MediaItem> media; };

    /// 按出现顺序拆出平台中立/OneBot 媒体码（url 优先于 file）。
    /// 没有实体引用的码（如入站群文件记录 [CQ:file,name=..,id=..]）原样保留在文本。
    static SplitText splitMedia(const std::string& text) {
        SplitText out;
        struct Tag { const char* prefix; size_t head; int kind; };
        static const Tag tags[] = {
            {"[img,", 5, 1},       {"[CQ:image,", 10, 1},
            {"[video,", 7, 2},     {"[CQ:video,", 10, 2},
            {"[voice,", 7, 3},     {"[CQ:record,", 11, 3}, {"[CQ:voice,", 10, 3},
            {"[file,", 6, 4},      {"[CQ:file,", 9, 4},
        };
        size_t i = 0;
        while (i < text.size()) {
            size_t tag = std::string::npos; size_t head = 0; int kind = 4;
            for (const auto& t : tags) {
                const size_t p = text.find(t.prefix, i);
                if (p != std::string::npos && (tag == std::string::npos || p < tag)) { tag = p; head = t.head; kind = t.kind; }
            }
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
            MediaItem item; item.kind = kind; item.name = param("name"); item.ref = param("url");
            if (item.ref.empty()) item.ref = param("file");
            if (item.ref.empty()) out.text += text.substr(tag, end - tag + 1);   // 保留不可发送的码
            else out.media.push_back(std::move(item));
            i = end + 1;
        }
        return out;
    }

    /// 媒体引用分类：0=远程公网 URL（/files+url 直传）；1=本地文件（分片上传）；2=无法发送。
    static int localFileKind(const std::string& ref, std::string& localPath) {
        std::string v = ref;
        for (auto& c : v) if (c == '\\') c = '/';
        auto tailOf = [&v](const char* seg) -> std::string {
            const auto p = v.find(seg);
            return p == std::string::npos ? std::string() : v.substr(p + std::string(seg).size());
        };
        std::string n;
        if (!(n = tailOf("/api/assets/")).empty()) { localPath = "data/assets/" + n; return 1; }
        if (!(n = tailOf("/api/chat/images/")).empty()) { localPath = "data/chat/images/" + n; return 1; }
        if (v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0) return 0;   // 真外链
        if (v.rfind("file://", 0) == 0) { localPath = v.substr(7); return 1; }
        if (v.rfind("base64://", 0) == 0) return 2;   // 官方接口不接受 base64
        localPath = v;
        return 1;
    }

    static std::string lowerExt(const std::string& name) {
        const size_t dot = name.find_last_of('.');
        std::string e = dot == std::string::npos ? std::string() : name.substr(dot + 1);
        for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e;
    }

    /// 扩展名 → 官方 file_type（1=图片 2=视频 3=语音 4=文件），未知格式保守降级为文件。
    static int fileTypeFor(int kind, const std::string& name) {
        const std::string e = lowerExt(name);
        if (kind == 1) {
            static const std::set<std::string> kImgs = {"png","jpg","jpeg","gif","webp","bmp","heic","heif","svg","ico","tif","tiff"};
            return kImgs.count(e) ? 1 : 4;
        }
        if (kind == 2) return e == "mp4" ? 2 : 4;
        if (kind == 3) {
            static const std::set<std::string> kAudio = {"silk","amr","mp3","wav","ogg","aac","m4a"};
            return kAudio.count(e) ? 3 : 4;
        }
        return 4;
    }

    static std::string mediaDefaultExt(int kind) {
        switch (kind) { case 1: return "png"; case 2: return "mp4"; case 3: return "silk"; default: return "bin"; }
    }

    /// 发送用文件名：优先码内 name，其次本地路径 basename；去掉非法字符并补默认扩展名。
    static std::string mediaFileName(const MediaItem& item, const std::string& localPath) {
        std::string name = item.name;
        if (name.empty()) {
            std::string v = localPath;
            for (auto& c : v) if (c == '\\') c = '/';
            const size_t s = v.find_last_of('/');
            name = s == std::string::npos ? v : v.substr(s + 1);
        }
        std::string clean;
        for (char c : name) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
            clean += c;
        }
        if (clean.empty()) clean = "file";
        if (lowerExt(clean).empty()) clean += "." + mediaDefaultExt(item.kind);
        return clean;
    }

    static std::string readLocalFile(const std::string& path) {
        std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        if (!f) return {};
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    static std::string hexDigest(const std::string& data, const EVP_MD* md) {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) return {};
        unsigned char buf[EVP_MAX_MD_SIZE]; unsigned int len = 0;
        const bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1
            && EVP_DigestUpdate(ctx, data.data(), data.size()) == 1
            && EVP_DigestFinal_ex(ctx, buf, &len) == 1;
        EVP_MD_CTX_free(ctx);
        if (!ok) return {};
        static const char kHex[] = "0123456789abcdef";
        std::string out; out.reserve(len * 2);
        for (unsigned int i = 0; i < len; ++i) { out += kHex[buf[i] >> 4]; out += kHex[buf[i] & 0xF]; }
        return out;
    }

    struct ParsedUrl { std::string scheme, host, path, query; uint16_t port = 443; };
    static bool parseUrl(const std::string& url, ParsedUrl& out) {
        const size_t schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos) return false;
        out.scheme = url.substr(0, schemeEnd);
        if (out.scheme != "http" && out.scheme != "https") return false;
        const size_t hostStart = schemeEnd + 3;
        const size_t pathStart = url.find('/', hostStart);
        std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
        const size_t colon = authority.rfind(':');
        if (colon == std::string::npos) { out.host = authority; out.port = out.scheme == "https" ? 443 : 80; }
        else {
            out.host = authority.substr(0, colon);
            try { out.port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1))); }
            catch (...) { return false; }
        }
        if (out.host.empty()) return false;
        const std::string rest = pathStart == std::string::npos ? "/" : url.substr(pathStart);
        const size_t q = rest.find('?');
        out.path = q == std::string::npos ? rest : rest.substr(0, q);
        out.query = q == std::string::npos ? std::string() : rest.substr(q + 1);
        return true;
    }

    static drogon::HttpClientPtr clientFor(const std::string& host, uint16_t port, bool tls) {
        const auto ip = resolveIpv4(host);
        return ip.empty() ? nullptr : drogon::HttpClient::newHttpClient(ip, port, tls);
    }

    std::string resolveTarget(const Message& m) {
        std::string target;
        if (m.extra.is_object()) target = m.extra.value("__identity_native_target", std::string());
        if (target.empty() && m.type != MessageType::kChannel && db_)
            target = identity::BindingStore::instance().officialTransport(*db_, appId_, m.targetId,
                m.type == MessageType::kPrivate ? identity::Kind::User : identity::Kind::Group);
        if (target.empty()) target = m.targetId;
        return target;
    }

    /// 兼容腾讯两种响应形态：{code,message,data:{...}} 或直接 {...}。
    static const json* dataObj(const json& j) {
        if (!j.is_object()) return nullptr;
        if (j.contains("data") && j["data"].is_object()) return &j["data"];
        return &j;
    }
    static std::string field(const json& j, const char* key) {
        const json* d = dataObj(j);
        return d ? d->value(key, std::string()) : std::string();
    }

    /// 腾讯接口部分数字字段以字符串返回（如 "1048576"），这里数字/字符串都接受。
    static size_t jsonSize(const json& v, size_t fallback) {
        if (v.is_number_unsigned()) return v.get<size_t>();
        if (v.is_number_integer()) { const long long x = v.get<long long>(); return x > 0 ? static_cast<size_t>(x) : 0; }
        if (v.is_string()) { try { return static_cast<size_t>(std::stoull(v.get_ref<const std::string&>())); } catch (...) {} }
        return fallback;
    }
    static int jsonInt(const json& v, int fallback) {
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_number_unsigned()) { const auto x = v.get<unsigned long long>(); return x <= static_cast<unsigned long long>(INT_MAX) ? static_cast<int>(x) : fallback; }
        if (v.is_string()) { try { return std::stoi(v.get_ref<const std::string&>()); } catch (...) {} }
        return fallback;
    }

    /// drogon 事件循环不会捕获回调异常，任何 std::exception 都会直接 terminate
    /// （表现为闪退且无日志）。所有富媒体异步回调统一经此兜底，把异常记进日志。
    template <typename Fn>
    void safeMediaStep(const char* step, Fn&& fn) {
        try { fn(); }
        catch (const std::exception& e) {
            lastError_ = std::string("QQ 官方富媒体异常: ") + e.what();
            DICE_LOG_ERROR("QQOfficial '{}': {} 抛出异常: {}", name_, step, e.what());
        }
        catch (...) {
            lastError_ = "QQ 官方富媒体未知异常";
            DICE_LOG_ERROR("QQOfficial '{}': {} 抛出未知异常", name_, step);
        }
    }

    /// 富媒体消息发送：拿到 file_info 后 POST /messages (msg_type=7)。
    void sendFileInfoTo(const Message& m, const std::string& target, const std::string& fileInfo) {
        if (fileInfo.empty()) { DICE_LOG_WARN("QQOfficial '{}': 富媒体上传未返回 file_info，跳过发送", name_); return; }
        if (accessToken_.empty()) { lastError_ = "QQ 官方机器人尚未取得 AccessToken"; return; }
        const bool priv = m.type == MessageType::kPrivate;
        const std::string msgPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/messages";
        // 官方要求 msg_type=7 时 content 需非空（文档注明的占位空格）。
        json body = {{"content", " "}, {"msg_type", 7}, {"media", {{"file_info", fileInfo}}}};
        if (!m.id.empty()) { body["msg_id"] = m.id; const int seq = nextReplySeq(m.id); if (seq > 0) body["msg_seq"] = seq; }
        else { const auto ev = takePassiveEvent(m.type, target); if (!ev.empty()) body["event_id"] = ev; }
        auto c2 = httpsClient("api.bot.qq.com"); if (!c2) return;
        auto r2 = drogon::HttpRequest::newHttpRequest();
        r2->setMethod(drogon::Post); r2->setPath(msgPath);
        r2->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r2->addHeader("Host", "api.bot.qq.com");
        r2->addHeader("Authorization", "QQBot " + accessToken_);
        r2->setBody(body.dump());
        auto self = shared_from_this();
        c2->sendRequest(r2, [self, msgPath](drogon::ReqResult rr2, const drogon::HttpResponsePtr& resp2) {
            self->safeMediaStep("发送富媒体消息回调", [&] {
                if (rr2 != drogon::ReqResult::Ok || !resp2 || resp2->statusCode() >= 300) {
                    self->lastError_ = "QQ 官方富媒体消息发送失败";
                    DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, msgPath,
                                  resp2 ? static_cast<int>(resp2->statusCode()) : 0, resp2 ? std::string(resp2->body()) : std::string("网络请求未完成"));
                    return;
                }
                DICE_LOG_INFO("QQOfficial '{}': 富媒体消息已发送: {}", self->name_, msgPath);
            });
        });
    }

    /// 公网 URL 直传：POST /files 换 file_info → 发消息。
    void uploadRemoteTo(const Message& m, const std::string& target, const MediaItem& item, const std::string& url) {
        if (accessToken_.empty()) { lastError_ = "QQ 官方机器人尚未取得 AccessToken"; return; }
        const bool priv = m.type == MessageType::kPrivate;
        const int ftype = fileTypeFor(item.kind, item.name.empty() ? url : item.name);
        const std::string filesPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/files";
        auto c = httpsClient("api.bot.qq.com");
        if (!c) { lastError_ = "无法解析 api.bot.qq.com"; return; }
        auto r = drogon::HttpRequest::newHttpRequest();
        r->setMethod(drogon::Post); r->setPath(filesPath);
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->addHeader("Host", "api.bot.qq.com");
        r->addHeader("Authorization", "QQBot " + accessToken_);
        r->setBody(json{{"file_type", ftype}, {"url", url}, {"srv_send_msg", false}}.dump());
        auto self = shared_from_this();
        c->sendRequest(r, [self, m, target, filesPath](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            self->safeMediaStep("URL 直传回调", [&] {
                if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                    self->lastError_ = "QQ 官方富媒体上传失败";
                    DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, filesPath,
                                  resp ? static_cast<int>(resp->statusCode()) : 0, resp ? std::string(resp->body()) : std::string("网络请求未完成"));
                    return;
                }
                auto j = json::parse(resp->body(), nullptr, false);
                const std::string fileInfo = field(j, "file_info");
                if (fileInfo.empty()) { DICE_LOG_WARN("QQOfficial '{}': /files 未返回 file_info: {}", self->name_, std::string(resp->body())); return; }
                DICE_LOG_INFO("QQOfficial '{}': URL 直传成功，获得 file_info，开始发送", self->name_);
                self->sendFileInfoTo(m, target, fileInfo);
            });
        });
    }

    struct UploadPart { int index = 0; std::string presignedUrl; size_t offset = 0; size_t size = 0; };

    /// 本地文件官方分片上传：prepare → 顺序 PUT 分片 → part_finish → /files 合并。
    void uploadLocalTo(const Message& m, const std::string& target, const MediaItem& item, const std::string& localPath) {
        safeMediaStep("uploadLocalTo 入口", [&] {
            if (accessToken_.empty()) { lastError_ = "QQ 官方机器人尚未取得 AccessToken"; return; }
            const std::string data = readLocalFile(localPath);
            if (data.empty()) { DICE_LOG_WARN("QQOfficial '{}': 本地媒体文件不存在或为空，跳过: {}", name_, localPath); return; }
            DICE_LOG_INFO("QQOfficial '{}': 本地媒体 {} ({} bytes, kind={})，开始官方分片上传", name_, localPath, data.size(), item.kind);
            const bool priv = m.type == MessageType::kPrivate;
            const std::string fname = mediaFileName(item, localPath);
            const int ftype = fileTypeFor(item.kind, fname);
            const std::string md5 = hexDigest(data, EVP_md5());
            const std::string sha1 = hexDigest(data, EVP_sha1());
            const size_t k10m = 10002432;   // 官方要求前 10,002,432 字节的 MD5
            const std::string md5_10m = hexDigest(data.substr(0, data.size() < k10m ? data.size() : k10m), EVP_md5());
            if (md5.empty() || sha1.empty() || md5_10m.empty()) { DICE_LOG_WARN("QQOfficial '{}': 计算本地媒体摘要失败，跳过: {}", name_, localPath); return; }
            const std::string prepPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/upload_prepare";
            auto c = httpsClient("api.bot.qq.com");
            if (!c) { lastError_ = "无法解析 api.bot.qq.com"; return; }
            auto r = drogon::HttpRequest::newHttpRequest();
            r->setMethod(drogon::Post); r->setPath(prepPath);
            r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            r->addHeader("Host", "api.bot.qq.com");
            r->addHeader("Authorization", "QQBot " + accessToken_);
            r->setBody(json{{"file_type", ftype}, {"file_size", data.size()}, {"file_name", fname},
                            {"md5", md5}, {"sha1", sha1}, {"md5_10m", md5_10m}}.dump());
            DICE_LOG_INFO("QQOfficial '{}': 发起 upload_prepare {} file_type={} file_name={}", name_, prepPath, ftype, fname);
            auto self = shared_from_this();
            c->sendRequest(r, [self, m, target, priv, data, fname, ftype, prepPath](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
                self->safeMediaStep("upload_prepare 回调", [&] {
                    if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                        self->lastError_ = "QQ 官方分片上传准备失败";
                        DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, prepPath,
                                      resp ? static_cast<int>(resp->statusCode()) : 0, resp ? std::string(resp->body()) : std::string("网络请求未完成"));
                        return;
                    }
                    auto j = json::parse(resp->body(), nullptr, false);
                    const json* d = dataObj(j);
                    if (!d) { DICE_LOG_WARN("QQOfficial '{}': upload_prepare 响应无效: {}", self->name_, std::string(resp->body())); return; }
                    const std::string uploadId = d->value("upload_id", std::string());
                    const size_t blockSize = d->contains("block_size") ? jsonSize((*d)["block_size"], size_t(1024 * 1024)) : size_t(1024 * 1024);
                    if (uploadId.empty() || !d->contains("parts") || !(*d)["parts"].is_array()) {
                        DICE_LOG_WARN("QQOfficial '{}': upload_prepare 缺少 upload_id/parts: {}", self->name_, std::string(resp->body())); return;
                    }
                    std::vector<UploadPart> parts;
                    size_t offset = 0;
                    for (const auto& pj : (*d)["parts"]) {
                        if (!pj.is_object()) continue;
                        UploadPart p;
                        p.index = pj.contains("index") ? jsonInt(pj["index"], 0) : 0;
                        p.presignedUrl = pj.value("presigned_url", std::string());
                        if (p.presignedUrl.empty() || offset >= data.size()) { parts.clear(); break; }
                        const size_t bs = pj.contains("block_size") ? jsonSize(pj["block_size"], blockSize) : blockSize;
                        p.offset = offset;
                        p.size = bs < data.size() - offset ? bs : data.size() - offset;
                        offset += p.size;
                        parts.push_back(std::move(p));
                    }
                    if (parts.empty()) { DICE_LOG_WARN("QQOfficial '{}': upload_prepare 返回的分片列表无效", self->name_); return; }
                    DICE_LOG_INFO("QQOfficial '{}': upload_prepare 成功 upload_id={} block_size={} parts={}", self->name_, uploadId, blockSize, parts.size());
                    self->uploadPartLoop(m, target, priv, data, uploadId, fname, ftype, std::move(parts), 0);
                });
            });
        });
    }

    void uploadPartLoop(const Message& m, const std::string& target, bool priv, const std::string& data,
                        const std::string& uploadId, const std::string& fname, int ftype,
                        std::vector<UploadPart> parts, size_t idx) {
        if (idx >= parts.size()) { finalizeUpload(m, target, priv, uploadId, fname, ftype); return; }
        DICE_LOG_INFO("QQOfficial '{}': 开始上传分片 {}/{} (index={})", name_, idx + 1, parts.size(), parts[idx].index);
        safeMediaStep("uploadPartLoop 入口", [&] {
            const UploadPart p = parts[idx];
            const std::string chunk = data.substr(p.offset, p.size);
            ParsedUrl pu;
            if (!parseUrl(p.presignedUrl, pu)) { DICE_LOG_WARN("QQOfficial '{}': 分片预签名地址无效，上传中止", name_); return; }
            auto c = clientFor(pu.host, pu.port, pu.scheme == "https");
            if (!c) { lastError_ = "无法解析分片上传域名 " + pu.host; return; }
            auto r = drogon::HttpRequest::newHttpRequest();
            r->setMethod(drogon::Put);
            r->setPathEncode(false);
            r->setPath(pu.query.empty() ? pu.path : pu.path + "?" + pu.query);
            r->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
            r->addHeader("Host", pu.host);
            r->setBody(chunk);
            auto self = shared_from_this();
            c->sendRequest(r, [self, m, target, priv, data, uploadId, fname, ftype, parts, idx, p](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
                self->safeMediaStep("PUT 分片回调", [&] {
                    if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                        self->lastError_ = "QQ 官方分片上传失败";
                        DICE_LOG_WARN("QQOfficial '{}': PUT 分片 {} 失败: HTTP {} {}", self->name_, p.index,
                                      resp ? static_cast<int>(resp->statusCode()) : 0, resp ? std::string(resp->body()) : std::string("网络请求未完成"));
                        return;
                    }
                    DICE_LOG_INFO("QQOfficial '{}': PUT 分片 {} 成功 ({} bytes)，确认分片", self->name_, p.index, p.size);
                    const std::string partMd5 = self->hexDigest(data.substr(p.offset, p.size), EVP_md5());
                    if (partMd5.empty()) { DICE_LOG_WARN("QQOfficial '{}': 分片摘要计算失败，上传中止", self->name_); return; }
                    const std::string finishPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/upload_part_finish";
                    auto c2 = httpsClient("api.bot.qq.com");
                    if (!c2) { self->lastError_ = "无法解析 api.bot.qq.com"; return; }
                    auto r2 = drogon::HttpRequest::newHttpRequest();
                    r2->setMethod(drogon::Post); r2->setPath(finishPath);
                    r2->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    r2->addHeader("Host", "api.bot.qq.com");
                    r2->addHeader("Authorization", "QQBot " + self->accessToken_);
                    r2->setBody(json{{"upload_id", uploadId}, {"part_index", p.index}, {"block_size", p.size}, {"md5", partMd5}}.dump());
                    c2->sendRequest(r2, [self, m, target, priv, data, uploadId, fname, ftype, parts, idx, p, finishPath](drogon::ReqResult rr2, const drogon::HttpResponsePtr& resp2) {
                        self->safeMediaStep("upload_part_finish 回调", [&] {
                            if (rr2 != drogon::ReqResult::Ok || !resp2 || resp2->statusCode() >= 300) {
                                self->lastError_ = "QQ 官方分片确认失败";
                                DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, finishPath,
                                              resp2 ? static_cast<int>(resp2->statusCode()) : 0, resp2 ? std::string(resp2->body()) : std::string("网络请求未完成"));
                                return;
                            }
                            DICE_LOG_INFO("QQOfficial '{}': 分片 {} 确认完成", self->name_, p.index);
                            self->uploadPartLoop(m, target, priv, data, uploadId, fname, ftype, std::move(parts), idx + 1);
                        });
                    });
                });
            });
        });
    }

    /// 全部分片就绪后合并：POST /files 换 file_info → 发消息。
    void finalizeUpload(const Message& m, const std::string& target, bool priv, const std::string& uploadId,
                        const std::string& fname, int ftype) {
        const std::string filesPath = (priv ? "/v2/users/" : "/v2/groups/") + target + "/files";
        auto c = httpsClient("api.bot.qq.com");
        if (!c) { lastError_ = "无法解析 api.bot.qq.com"; return; }
        auto r = drogon::HttpRequest::newHttpRequest();
        r->setMethod(drogon::Post); r->setPath(filesPath);
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->addHeader("Host", "api.bot.qq.com");
        r->addHeader("Authorization", "QQBot " + accessToken_);
        r->setBody(json{{"file_type", ftype}, {"file_name", fname}, {"upload_id", uploadId}, {"srv_send_msg", false}}.dump());
        auto self = shared_from_this();
        c->sendRequest(r, [self, m, target, filesPath](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            self->safeMediaStep("分片合并回调", [&] {
                if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                    self->lastError_ = "QQ 官方分片合并失败";
                    DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, filesPath,
                                  resp ? static_cast<int>(resp->statusCode()) : 0, resp ? std::string(resp->body()) : std::string("网络请求未完成"));
                    return;
                }
                auto j = json::parse(resp->body(), nullptr, false);
                const std::string fileInfo = field(j, "file_info");
                if (fileInfo.empty()) { DICE_LOG_WARN("QQOfficial '{}': /files 未返回 file_info: {}", self->name_, std::string(resp->body())); return; }
                DICE_LOG_INFO("QQOfficial '{}': 分片合并成功，获得 file_info，开始发送", self->name_);
                self->sendFileInfoTo(m, target, fileInfo);
            });
        });
    }

    /// 富媒体入口：远程 URL 直传，本地文件走分片上传。
    void sendMediaTo(const Message& m, const MediaItem& item) {
        try {
            if (m.type == MessageType::kChannel) { DICE_LOG_WARN("QQOfficial '{}': 频道富媒体暂未接入，跳过", name_); return; }
            if (accessToken_.empty()) { lastError_ = "QQ 官方机器人尚未取得 AccessToken"; return; }
            const std::string target = resolveTarget(m);
            if (target.empty()) { DICE_LOG_WARN("QQOfficial '{}': 无法解析发送目标，跳过媒体", name_); return; }
            std::string localPath;
            const int loc = localFileKind(item.ref, localPath);
            if (loc == 2) { DICE_LOG_WARN("QQOfficial '{}': 不支持的媒体引用（官方接口不接受 base64://），跳过: {}", name_, item.ref); return; }
            DICE_LOG_INFO("QQOfficial '{}': 发送媒体 kind={} ref={} target={} 路由={}", name_, item.kind, item.ref, target, loc == 0 ? "URL直传" : "本地分片");
            if (loc == 0) uploadRemoteTo(m, target, item, item.ref);
            else uploadLocalTo(m, target, item, localPath);
        } catch (const std::exception& e) {
            lastError_ = std::string("QQ 官方富媒体异常: ") + e.what();
            DICE_LOG_ERROR("QQOfficial '{}': sendMediaTo 异常: {}", name_, e.what());
        } catch (...) {
            lastError_ = "QQ 官方富媒体未知异常";
            DICE_LOG_ERROR("QQOfficial '{}': sendMediaTo 未知异常", name_);
        }
    }

    /// 出站入口：拆出媒体标记走富媒体（限 3 条防刷频），剩余文本走文字消息。
    void sendTo(const Message& m, const std::string& text) {
        auto parts = splitMedia(text);
        for (size_t i = 0; i < parts.media.size() && i < 3; ++i) sendMediaTo(m, parts.media[i]);
        std::string plain = parts.text;
        const auto b = plain.find_first_not_of(" \t\r\n");
        plain = (b == std::string::npos) ? std::string() : plain.substr(b, plain.find_last_not_of(" \t\r\n") - b + 1);
        if (plain.empty()) { if (parts.media.empty()) sendTextTo(m, text); return; }   // 纯媒体不再发空文本
        sendTextTo(m, plain);
    }
    /// QQ 官方机器人支持 Markdown，但它仍受机器人后台能力开关约束。卡片模式
    /// 下优先以 Markdown 发送；收到明确 HTTP 拒绝后自动用传统文本重试一次。
    void sendTextTo(const Message& m, const std::string& text, bool forceTraditional = false) {
        if (accessToken_.empty()) { lastError_ = "QQ 官方机器人尚未取得 AccessToken"; return; }
        std::string target;
        if (m.extra.is_object()) target = m.extra.value("__identity_native_target", std::string());
        if (target.empty() && m.type != MessageType::kChannel && db_)
            target = identity::BindingStore::instance().officialTransport(
                *db_, appId_, m.targetId, m.type == MessageType::kPrivate ? identity::Kind::User : identity::Kind::Group);
        if (target.empty()) target = m.targetId;

        std::string path;
        if (m.type == MessageType::kPrivate) path = "/v2/users/" + target + "/messages";
        else if (m.type == MessageType::kGroup) path = "/v2/groups/" + target + "/messages";
        else path = "/channels/" + target + "/messages";

        auto client = httpsClient("api.bot.qq.com");
        if (!client) { lastError_ = "无法解析 api.bot.qq.com"; return; }
        const bool useCard = effectiveCardMode() && !forceTraditional && m.type != MessageType::kChannel;

        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Post); request->setPath(path);
        request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        request->addHeader("Host", "api.bot.qq.com");
        request->addHeader("Authorization", "QQBot " + accessToken_);
        json body = useCard
            ? json{{"content", " "}, {"msg_type", 2}, {"markdown", {{"content", text}, {"force_verify_image_resource", forceVerifyImageResource_}}}}
            : json{{"content", text}};
        if (!useCard && m.type != MessageType::kChannel) body["msg_type"] = 0;
        if (!m.id.empty()) {
            body["msg_id"] = m.id;
            const int seq = nextReplySeq(m.id);
            if (seq > 0 && m.type != MessageType::kChannel) body["msg_seq"] = seq;
        } else if (m.type != MessageType::kChannel) {
            const auto eventId = takePassiveEvent(m.type, target);
            if (!eventId.empty()) body["event_id"] = eventId;
        }
        request->setBody(body.dump());
        client->sendRequest(request, [self = shared_from_this(), path, message = m, text, useCard](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
            const bool httpRejected = response && response->statusCode() >= 300;
            if (useCard && httpRejected) {
                DICE_LOG_WARN("QQOfficial '{}': Markdown/card message rejected by {}, retrying traditional text", self->name_, path);
                self->sendTextTo(message, text, true);
                return;
            }
            if (result != drogon::ReqResult::Ok || !response || httpRejected) {
                std::string detail = "网络请求未完成";
                if (response) { const auto responseBody = response->body(); detail.assign(responseBody.data(), responseBody.size()); }
                self->lastError_ = "QQ 官方消息发送失败";
                if (response) DICE_LOG_WARN("QQOfficial '{}': POST {} failed: HTTP {} {}", self->name_, path, static_cast<int>(response->statusCode()), detail);
                else DICE_LOG_WARN("QQOfficial '{}': POST {} failed: {} {}", self->name_, path, drogon::to_string(result), detail);
            }
        });
    }
    void fail(const std::string&e){lastError_=e;connecting_=false;connected_=false;DICE_LOG_ERROR("QQOfficial '{}': {}",name_,e);}
    inline static std::function<std::string(const std::string&)> imagePublisher_;   // 本地图 → 公网 URL（图床）
    std::string id_,name_,appId_,appSecret_,displayQQ_,shareUrl_,accessToken_,loginId_,loginName_,sessionId_,gatewayUrl_,lastError_; bool forceVerifyImageResource_{false}; Database* db_{identity::BindingStore::instance().database()}; std::atomic<bool> connected_{false},connecting_{false},stopping_{false}; int64_t seq_=-1; std::shared_ptr<QQGatewaySocket> gateway_; std::optional<trantor::TimerId> heartbeatTimer_,accessTokenTimer_; MessageCallback messageCb_; EventCallback eventCb_; std::mutex replyMu_; std::unordered_map<std::string,int> replySeq_; std::unordered_map<std::string,std::pair<std::string,std::time_t>> pendingEvents_;
};
}
