#pragma once

// Milky protocol adapter.  Milky is an HTTP API plus WebHook protocol, so the
// adapter deliberately keeps the transport independent from Drogon's event
// loop: API calls run through curl in short-lived worker threads and WebHook
// events enter through the public handler registered by api_service.h.

#include "adapter_interface.h"
#include "self_echo_filter.h"
#include "../common/logger.h"
#include "../common/subprocess.h"
#include "../core/identity/identity_binding.h"
#include "../service/image_send.h"

#include <drogon/utils/Utilities.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <initializer_list>

namespace dice {

class MilkyAdapter final : public IAdapter,
                           public std::enable_shared_from_this<MilkyAdapter> {
    struct MediaFile { std::string name, content, path, literal; };
    struct ParsedOutgoing { json segments = json::array(); std::vector<MediaFile> files; };
public:
    explicit MilkyAdapter(const std::string& adapterId) : id_(adapterId), name_(adapterId) {}

    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string platform() const override { return "milky"; }
    std::string version() const override { return "1.0.0"; }

    json capabilities() const override {
        return {{"friends", true}, {"friend_delete", true}, {"kick", true}, {"ban", true},
                {"poke", true}, {"forward", true}, {"group_file", true}, {"group_card", true},
                {"member_list", true}, {"group_leave", true}};
    }

    bool configure(const json& cfg) override {
        name_ = cfg.value("name", id_);
        endpoint_ = cfg.value("endpoint", cfg.value("apiEndpoint", std::string()));
        while (!endpoint_.empty() && endpoint_.back() == '/') endpoint_.pop_back();
        if (endpoint_.size() >= 4 && endpoint_.compare(endpoint_.size() - 4, 4, "/api") == 0)
            endpoint_.resize(endpoint_.size() - 4);
        accessToken_ = cfg.value("accessToken", cfg.value("access_token", std::string()));
        webhookBaseUrl_ = cfg.value("webhookBaseUrl", cfg.value("webhook_base_url", std::string()));
        while (!webhookBaseUrl_.empty() && webhookBaseUrl_.back() == '/') webhookBaseUrl_.pop_back();
        webhookToken_ = cfg.value("webhookToken", cfg.value("webhook_token", std::string()));
        setMessageFormatOverride(parseFormatOverride(cfg.value("message_format", std::string())));
        if (endpoint_.empty()) { lastError_ = "Milky API endpoint is required"; return false; }
        if (accessToken_.empty()) { lastError_ = "Milky access token is required"; return false; }
        return true;
    }

    bool start() override {
        if (connected_) return true;
        stopping_ = false;
        json result = invokeAction("get_login_info", json::object(), 10000);
        if (!apiOk(result)) {
            std::lock_guard<std::mutex> lk(stateMutex_);
            lastError_ = result.is_object()
                ? result.value("message", std::string("Milky get_login_info failed"))
                : std::string("Milky get_login_info timed out");
            connected_ = false;
            return false;
        }
        const json d = result.value("data", json::object());
        loginId_ = field(d, "uin");
        if (loginId_.empty()) loginId_ = field(d, "user_id");
        loginName_ = field(d, "nickname");
        connected_ = true;
        lastError_.clear();
        refreshGroupList();
        return true;
    }

    void stop() override { stopping_ = true; connected_ = false; }
    bool isConnected() const override { return connected_.load(); }
    std::string lastError() const override {
        std::lock_guard<std::mutex> lk(stateMutex_);
        return lastError_;
    }
    std::string connectionStatus() const override { return connected_ ? "connected" : "disconnected"; }

    std::string getLoginId() const override { return loginId_; }
    std::string getLoginName() const override { return loginName_; }
    std::string webhookToken() const { return webhookToken_; }
    std::string webhookUrl() const {
        return webhookBaseUrl_.empty() ? std::string() : webhookBaseUrl_ + "/milky/webhook/" + id_;
    }

    void sendMessage(const Message& msg) override {
        ParsedOutgoing parsed = parseOutgoing(prepOutgoing(msg.targetId, msg.content, ContentFormat::kPlainText));
        sendSegments(msg.type == MessageType::kPrivate ? "send_private_message" : "send_group_message",
                     msg.type == MessageType::kPrivate ? json{{"user_id", parseId(msg.targetId)}}
                                                       : json{{"group_id", parseId(msg.targetId)}},
                     msg.type == MessageType::kGroup ? msg.targetId : std::string(), std::move(parsed));
    }

    void sendReply(const Message& original, const std::string& text) override {
        sendReplyFormatted(original, text, ContentFormat::kPlainText);
    }

    void sendReplyFormatted(const Message& original, const std::string& text,
                            ContentFormat format) override {
        ParsedOutgoing parsed = parseOutgoing(prepOutgoing(original.targetId, text, format));
        json message = json::array();
        if (!original.id.empty()) message.push_back({{"type", "reply"}, {"data", {{"message_seq", parseId(original.id)}}}});
        for (const auto& s : parsed.segments) message.push_back(s);
        if (original.type == MessageType::kPrivate)
            for (const auto& f : parsed.files) message.push_back({{"type", "text"}, {"data", {{"text", f.literal}}}});
        json p = original.type == MessageType::kPrivate
            ? json{{"user_id", parseId(original.targetId)}, {"message", message}}
            : json{{"group_id", parseId(original.targetId)}, {"message", message}};
        sendAction(original.type == MessageType::kPrivate ? "send_private_message" : "send_group_message", p,
                   [self = shared_from_this(), gid = original.type == MessageType::kGroup ? original.targetId : std::string(), files = std::move(parsed.files)](json) {
                       if (gid.empty()) return;
                       for (const auto& f : files) self->uploadGroupFile(gid, f.name, f.content, f.path);
                   });
    }

    void onMessage(MessageCallback cb) override { messageCallbacks_.push_back(std::move(cb)); }
    void onEvent(EventCallback cb) override { eventCallbacks_.push_back(std::move(cb)); }

    void sendGroupMessage(const std::string& groupId, const std::string& text) override {
        sendGroupMessageFormatted(groupId, text, ContentFormat::kPlainText);
    }
    void sendGroupMessageFormatted(const std::string& groupId, const std::string& text,
                                   ContentFormat format) override {
        ParsedOutgoing parsed = parseOutgoing(prepOutgoing(groupId, text, format));
        sendSegments("send_group_message", {{"group_id", parseId(groupId)}}, groupId, std::move(parsed));
    }
    void sendGroupMessageCQ(const std::string& groupId, const std::string& text) override {
        sendGroupMessageFormatted(groupId, text, ContentFormat::kPlainText);
    }
    void sendPrivateMessage(const std::string& userId, const std::string& text) override {
        sendPrivateMessageFormatted(userId, text, ContentFormat::kPlainText);
    }
    void sendPrivateMessageFormatted(const std::string& userId, const std::string& text,
                                     ContentFormat format) override {
        ParsedOutgoing parsed = parseOutgoing(prepOutgoing(userId, text, format));
        sendSegments("send_private_message", {{"user_id", parseId(userId)}}, {}, std::move(parsed));
    }

    bool sendGroupForwardMsg(const std::string& groupId,
                             const std::vector<std::string>& nodes) override {
        return sendGroupForwardMsgFormatted(groupId, nodes, ContentFormat::kPlainText);
    }
    bool sendGroupForwardMsgFormatted(const std::string& groupId,
                                      const std::vector<std::string>& nodes,
                                      ContentFormat format) override {
        if (nodes.empty()) return false;
        json messages = json::array();
        for (const auto& node : nodes) {
            ParsedOutgoing parsed = parseOutgoing(prepOutgoing(groupId, node, format));
            messages.push_back({{"user_id", parseId(loginId_)}, {"sender_name", loginName_.empty() ? std::string("Dice") : loginName_},
                                {"segments", parsed.segments}});
        }
        json forward = json::array();
        forward.push_back({{"type", "forward"}, {"data", {{"messages", messages}}}});
        sendAction("send_group_message", {{"group_id", parseId(groupId)}, {"message", forward}}, {});
        return true;
    }

    void setFriendRequest(const std::string& flag, bool approve,
                          const std::string& remark = "") override {
        json f = json::parse(flag, nullptr, false);
        if (!f.is_object() || f.value("kind", "") != "friend") return;
        json p{{"initiator_uid", f.value("initiator_uid", std::string())}, {"is_filtered", f.value("is_filtered", false)}};
        if (approve) { sendAction("accept_friend_request", p, {}); }
        else { p["reason"] = remark; sendAction("reject_friend_request", p, {}); }
    }
    void setGroupRequest(const std::string& flag, const std::string& subType,
                         bool approve, const std::string& reason = "") override {
        json f = json::parse(flag, nullptr, false);
        if (!f.is_object() || f.value("kind", "") != "group") return;
        const std::string kind = f.value("request_type", subType);
        json p{{"group_id", parseId(f.value("group_id", std::string()))}, {"notification_seq", parseId(f.value("notification_seq", std::string()))},
               {"is_filtered", f.value("is_filtered", false)}};
        if (kind == "invitation") {
            p["invitation_seq"] = p["notification_seq"];
            p.erase("notification_seq");
            p.erase("is_filtered");
            sendAction(approve ? "accept_group_invitation" : "reject_group_invitation", p, {});
        } else {
            p["notification_type"] = kind == "invited_join_request" ? "invited_join_request" : "join_request";
            if (!approve) p["reason"] = reason;
            sendAction(approve ? "accept_group_request" : "reject_group_request", p, {});
        }
    }

    std::string getGroupName(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        auto it = groupNames_.find(groupId);
        return it == groupNames_.end() ? groupId : it->second;
    }
    std::vector<std::string> getGroupMemberList(const std::string& groupId) const override {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lk(cacheMutex_);
        auto it = memberLists_.find(groupId);
        if (it == memberLists_.end() || !it->second.is_array()) return out;
        for (const auto& m : it->second) { auto uid = field(m, "user_id"); if (!uid.empty()) out.push_back(uid); }
        return out;
    }
    bool isGroupAdmin(const std::string& groupId, const std::string& userId) const override {
        auto role = memberRole(groupId, userId); return role == "admin" || role == "owner";
    }
    bool isGroupOwner(const std::string& groupId, const std::string& userId) const override { return memberRole(groupId, userId) == "owner"; }

    void setGroupKick(const std::string& groupId, const std::string& userId) override {
        sendAction("kick_group_member", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}, {"reject_add_request", false}}, {});
    }
    void setGroupBan(const std::string& groupId, const std::string& userId, int durationSec) override {
        sendAction("set_group_member_mute", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}, {"duration", std::max(0, durationSec)}}, {});
    }
    void leaveGroup(const std::string& groupId) override { sendAction("quit_group", {{"group_id", parseId(groupId)}}, {}); }
    void setGroupCard(const std::string& groupId, const std::string& userId, const std::string& card) override {
        sendAction("set_group_member_card", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}, {"card", card}}, {});
    }
    void setGroupName(const std::string& groupId, const std::string& name) override {
        sendAction("set_group_name", {{"group_id", parseId(groupId)}, {"new_group_name", name}}, {});
    }
    void setGroupSpecialTitle(const std::string& groupId, const std::string& userId, const std::string& title) override {
        sendAction("set_group_member_special_title", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}, {"special_title", title}}, {});
    }
    void sendGroupPoke(const std::string& groupId, const std::string& userId) override {
        sendAction("send_group_nudge", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}}, {});
    }
    void deleteFriend(const std::string& userId) override { sendAction("delete_friend", {{"user_id", parseId(userId)}}, {}); }

    std::vector<std::pair<std::string, std::string>> getGroupList() const override {
        std::lock_guard<std::mutex> lk(cacheMutex_); std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [id, name] : groupNames_) out.push_back({id, name}); return out;
    }
    int getFriendCount() const override { std::lock_guard<std::mutex> lk(cacheMutex_); return static_cast<int>(friendList_.size()); }
    std::vector<std::string> getFriendList() const override { std::lock_guard<std::mutex> lk(cacheMutex_); return friendList_; }
    int getGroupMemberCount(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(cacheMutex_); auto it = groupCounts_.find(groupId); return it == groupCounts_.end() ? 0 : it->second;
    }
    std::string getSelfRole(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(cacheMutex_); auto it = selfRoles_.find(groupId); return it == selfRoles_.end() ? std::string() : it->second;
    }
    json getMembers(const std::string& groupId) const override {
        std::lock_guard<std::mutex> lk(cacheMutex_); auto it = memberLists_.find(groupId); return it == memberLists_.end() ? json::array() : it->second;
    }

    void refreshGroupList() override {
        invokeActionAsync("get_group_list", {}, [self = shared_from_this()](json r) { self->cacheGroupList(r); });
        invokeActionAsync("get_friend_list", {}, [self = shared_from_this()](json r) {
            const json d = r.value("data", json::object());
            if (!apiOk(r) || !d.value("friends", json()).is_array()) return;
            std::lock_guard<std::mutex> lk(self->cacheMutex_); self->friendList_.clear();
            for (const auto& f : d["friends"]) { auto id = field(f, "user_id"); if (!id.empty()) self->friendList_.push_back(id); }
        });
    }
    void refreshMembers(const std::string& groupId) override {
        invokeActionAsync("get_group_member_list", {{"group_id", parseId(groupId)}}, [self = shared_from_this(), groupId](json r) {
            const json d = r.value("data", json::object());
            if (!apiOk(r) || !d.value("members", json()).is_array()) return;
            std::lock_guard<std::mutex> lk(self->cacheMutex_); self->memberLists_[groupId] = d["members"];
            for (const auto& m : d["members"]) { auto uid = field(m, "user_id"); if (!uid.empty()) self->memberRoles_[groupId + "\x1f" + uid] = m.value("role", "member"); }
        });
        refreshSelfRole(groupId);
    }
    void refreshSelfRole(const std::string& groupId) override {
        if (loginId_.empty()) return;
        invokeActionAsync("get_group_member_info", {{"group_id", parseId(groupId)}, {"user_id", parseId(loginId_)}}, [self = shared_from_this(), groupId](json r) {
            const json d = r.value("data", json::object()); const json member = d.value("member", json::object());
            if (!apiOk(r) || !member.is_object()) return;
            std::lock_guard<std::mutex> lk(self->cacheMutex_); self->selfRoles_[groupId] = member.value("role", "member");
        });
    }
    void refreshMemberRole(const std::string& groupId, const std::string& userId) override {
        invokeActionAsync("get_group_member_info", {{"group_id", parseId(groupId)}, {"user_id", parseId(userId)}}, [self = shared_from_this(), groupId, userId](json r) {
            const json d = r.value("data", json::object()); const json member = d.value("member", json::object());
            if (!apiOk(r) || !member.is_object()) return;
            std::lock_guard<std::mutex> lk(self->cacheMutex_); self->memberRoles_[groupId + "\x1f" + userId] = member.value("role", "member");
        });
    }

    void requestGroupHistory(const std::string& groupId, int count) override {
        invokeActionAsync("get_history_messages", {{"message_scene", "group"}, {"peer_id", parseId(groupId)}, {"limit", count > 0 ? count : 50}},
                          [self = shared_from_this(), groupId](json r) {
                              if (!apiOk(r)) return; const json d = r.value("data", json::object());
                              const json* arr = d.is_array() ? &d : (d.contains("messages") && d["messages"].is_array() ? &d["messages"] : nullptr);
                              if (!arr) return; BotEvent e; e.type = EventType::kGroupHistory; e.platform = self->platform(); e.adapterId = self->id_; e.selfId = self->loginId_; e.groupId = groupId; e.extra = {{"messages", json::array()}};
                              for (const auto& m : *arr) e.extra["messages"].push_back(self->historyMessage(m));
                              for (auto& cb : self->eventCallbacks_) cb(e);
                          });
    }

    void uploadGroupFile(const std::string& groupId, const std::string& name,
                         const std::string& content, const std::string& localPath = "") override {
        std::string uri;
        if (!content.empty()) uri = "base64://" + drogon::utils::base64Encode(reinterpret_cast<const unsigned char*>(content.data()), content.size());
        else if (!localPath.empty()) {
            uri = (localPath.rfind("file:", 0) == 0 || localPath.rfind("base64://", 0) == 0 ||
                   localPath.rfind("http://", 0) == 0 || localPath.rfind("https://", 0) == 0)
                ? localPath : "file://" + localPath;
        }
        if (uri.empty()) return;
        sendAction("upload_group_file", {{"group_id", parseId(groupId)}, {"file_uri", uri}, {"file_name", name}}, {});
    }

    json invokeAction(const std::string& action, const json& params, int timeoutMs = 8000) override {
        auto promise = std::make_shared<std::promise<json>>(); auto future = promise->get_future();
        std::thread([self = shared_from_this(), action, params, promise]() { promise->set_value(self->performAction(action, params)); }).detach();
        if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) return json();
        try { return future.get(); } catch (...) { return json(); }
    }
    void invokeActionAsync(const std::string& action, const json& params, ActionCallback cb) override {
        std::thread([self = shared_from_this(), action, params, cb = std::move(cb)]() mutable { cb(self->performAction(action, params)); }).detach();
    }

    // Called by the /milky/webhook/{adapterId} route.
    bool handleWebhook(const json& event) {
        if (!event.is_object()) return false;
        const std::string type = event.value("event_type", event.value("type", std::string()));
        const std::string eventSelfId = field(event, "self_id");
        if (!eventSelfId.empty() && loginId_.empty()) loginId_ = eventSelfId;
        json data = event.value("data", json::object());
        if (event.contains("time")) data["__event_time"] = event["time"];
        if (type == "message_receive") { handleMessage(data); return true; }
        handleEvent(type, data); return true;
    }

    // Public for focused adapter tests and for diagnostics in the WebUI.
    static json buildSegmentsForTest(const std::string& text) { return parseOutgoingStatic(text).segments; }

private:

    static std::string field(const json& j, const char* key) {
        if (!j.is_object() || !j.contains(key) || j[key].is_null()) return {};
        if (j[key].is_string()) return j[key].get<std::string>();
        if (j[key].is_number_integer()) return std::to_string(j[key].get<int64_t>());
        return {};
    }
    static int64_t parseId(const std::string& value) {
        try { return std::stoll(identity::BindingStore::rawQQ(value)); } catch (...) { return 0; }
    }
    static bool apiOk(const json& r) { return r.is_object() && r.value("status", std::string("failed")) == "ok" && r.value("retcode", -1) == 0; }
    static std::string unescape(std::string s) {
        auto repl = [&s](const std::string& a, const std::string& b) { size_t p = 0; while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); } };
        repl("&#44;", ","); repl("&#91;", "["); repl("&#93;", "]"); repl("&amp;", "&"); return s;
    }
    static std::map<std::string, std::string> params(const std::string& body) {
        std::map<std::string, std::string> out; size_t p = 0;
        while (p < body.size()) { size_t q = body.find(',', p); std::string item = body.substr(p, q == std::string::npos ? std::string::npos : q - p); size_t eq = item.find('='); if (eq != std::string::npos) out[item.substr(0, eq)] = unescape(item.substr(eq + 1)); if (q == std::string::npos) break; p = q + 1; }
        return out;
    }
    static std::string valueAny(const std::map<std::string, std::string>& p, std::initializer_list<const char*> keys) {
        for (auto k : keys) { auto it = p.find(k); if (it != p.end() && !it->second.empty()) return it->second; } return {};
    }
    static std::string imageUri(const std::string& ref) {
        if (ref.empty()) return {};
        return imgsend::resolve(ref, "milky", "");
    }
    static ParsedOutgoing parseOutgoingStatic(const std::string& text) {
        ParsedOutgoing out; size_t pos = 0, plain = 0;
        auto flush = [&](size_t end) { if (end > plain) out.segments.push_back({{"type", "text"}, {"data", {{"text", text.substr(plain, end - plain)}}}}); };
        while (pos < text.size()) {
            if (text[pos] != '[') { ++pos; continue; }
            size_t end = text.find(']', pos + 1); if (end == std::string::npos) break;
            std::string token = text.substr(pos + 1, end - pos - 1), type, body;
            if (token.rfind("CQ:", 0) == 0) { size_t c = token.find(','); type = token.substr(3, c == std::string::npos ? std::string::npos : c - 3); body = c == std::string::npos ? std::string() : token.substr(c + 1); }
            else if (token.rfind("img,", 0) == 0 || token.rfind("图片:", 0) == 0 || token.rfind("图:", 0) == 0) { type = "image"; body = token.rfind("img,", 0) == 0 ? token.substr(4) : "file=" + token.substr(token.find(':') + 1); }
            else if (token.rfind("voice,", 0) == 0 || token.rfind("record,", 0) == 0) { type = "record"; body = token.substr(token.find(',') + 1); }
            else if (token.rfind("文件:", 0) == 0) { type = "file"; body = "file=" + token.substr(token.find(':') + 1); }
            else if (token.rfind("video,", 0) == 0) { type = "video"; body = token.substr(6); }
            else if (token.rfind("file,", 0) == 0) { type = "file"; body = token.substr(5); }
            else { pos = end + 1; continue; }
            flush(pos); auto p = params(body);
            if (type == "voice") type = "record";
            if (type == "text") { out.segments.push_back({{"type", "text"}, {"data", {{"text", valueAny(p, {"text"})}}}}); }
            else if (type == "at" || type == "mention") { auto uid = valueAny(p, {"qq", "user_id", "user"}); if (uid == "all") out.segments.push_back({{"type", "mention_all"}, {"data", json::object()}}); else if (parseId(uid) > 0) out.segments.push_back({{"type", "mention"}, {"data", {{"user_id", parseId(uid)}}}}); else out.segments.push_back({{"type", "text"}, {"data", {{"text", text.substr(pos, end - pos + 1)}}}}); }
            else if (type == "mention_all") out.segments.push_back({{"type", "mention_all"}, {"data", json::object()}});
            else if (type == "face") { auto id = valueAny(p, {"id", "face_id"}); if (!id.empty()) out.segments.push_back({{"type", "face"}, {"data", {{"face_id", id}}}}); else out.segments.push_back({{"type", "text"}, {"data", {{"text", text.substr(pos, end - pos + 1)}}}}); }
            else if (type == "image" || type == "record" || type == "video") { auto ref = valueAny(p, {"url", "file", "path", "uri"}); auto uri = imageUri(ref); if (!uri.empty()) { json d{{"uri", uri}}; if (type == "video") { auto thumb = imageUri(valueAny(p, {"thumb_uri", "thumb"})); if (!thumb.empty()) d["thumb_uri"] = thumb; } out.segments.push_back({{"type", type}, {"data", d}}); } else out.segments.push_back({{"type", "text"}, {"data", {{"text", text.substr(pos, end - pos + 1)}}}}); }
            else if (type == "reply") { auto seq = parseId(valueAny(p, {"id", "message_id", "message_seq"})); if (seq > 0) out.segments.push_back({{"type", "reply"}, {"data", {{"message_seq", seq}}}}); }
            else if (type == "file") { auto ref = valueAny(p, {"url", "file", "path", "uri"}); if (!ref.empty()) out.files.push_back({valueAny(p, {"name", "filename"}), {}, ref, text.substr(pos, end - pos + 1)}); else out.segments.push_back({{"type", "text"}, {"data", {{"text", text.substr(pos, end - pos + 1)}}}}); }
            else { out.segments.push_back({{"type", "text"}, {"data", {{"text", text.substr(pos, end - pos + 1)}}}}); }
            pos = end + 1; plain = pos;
        }
        flush(text.size()); return out;
    }
    ParsedOutgoing parseOutgoing(const std::string& text) const { return parseOutgoingStatic(text); }
    std::string prepOutgoing(const std::string& target, const std::string& text, ContentFormat format) {
        std::string out = guardCrossBot(format == ContentFormat::kMarkdown ? text : text);
        SelfEchoFilter::instance().mark(platform() + ":" + target, normalizeEcho(out)); return out;
    }
    void sendSegments(const std::string& action, json p, const std::string& groupId, ParsedOutgoing parsed) {
        p["message"] = parsed.segments;
        if (groupId.empty())
            for (const auto& f : parsed.files) p["message"].push_back({{"type", "text"}, {"data", {{"text", f.literal}}}});
        sendAction(action, p, [self = shared_from_this(), groupId, files = std::move(parsed.files)](json) {
            if (groupId.empty()) return; for (const auto& f : files) self->uploadGroupFile(groupId, f.name.empty() ? "file" : f.name, f.content, f.path);
        });
    }

    struct ApiSpec { std::string path; json body; };
    static ApiSpec spec(const std::string& action, json p) {
        static const std::map<std::string, std::string> paths = {
            {"get_login_info", "get_login_info"}, {"get_impl_info", "get_impl_info"}, {"get_user_profile", "get_user_profile"},
            {"get_friend_list", "get_friend_list"}, {"get_group_list", "get_group_list"}, {"get_group_info", "get_group_info"},
            {"get_group_member_list", "get_group_member_list"}, {"get_group_member_info", "get_group_member_info"},
            {"send_private_message", "send_private_message"}, {"send_group_message", "send_group_message"},
            {"get_history_messages", "get_history_messages"}, {"get_forwarded_messages", "get_forwarded_messages"},
            {"send_friend_nudge", "send_friend_nudge"}, {"send_group_nudge", "send_group_nudge"}, {"delete_friend", "delete_friend"},
            {"accept_friend_request", "accept_friend_request"}, {"reject_friend_request", "reject_friend_request"},
            {"accept_group_request", "accept_group_request"}, {"reject_group_request", "reject_group_request"},
            {"accept_group_invitation", "accept_group_invitation"}, {"reject_group_invitation", "reject_group_invitation"},
            {"kick_group_member", "kick_group_member"}, {"set_group_member_mute", "set_group_member_mute"}, {"quit_group", "quit_group"},
            {"set_group_member_card", "set_group_member_card"}, {"set_group_name", "set_group_name"}, {"set_group_member_special_title", "set_group_member_special_title"},
            {"get_group_files", "get_group_files"}, {"get_group_root_files", "get_group_files"}, {"get_group_files_by_folder", "get_group_files"}, {"get_group_file_download_url", "get_group_file_download_url"},
            {"get_group_file_url", "get_group_file_download_url"}, {"upload_group_file", "upload_group_file"}
        };
        auto it = paths.find(action); if (it == paths.end()) return {};
        if (action == "get_group_root_files" && !p.contains("parent_folder_id")) p["parent_folder_id"] = "/";
        if (action == "get_group_files_by_folder") { p["parent_folder_id"] = p.value("folder_id", std::string("/")); p.erase("folder_id"); }
        return {"/api/" + it->second, std::move(p)};
    }
    json performAction(const std::string& action, const json& params) {
        ApiSpec s = spec(action, params); if (s.path.empty()) return json{{"status", "failed"}, {"retcode", -1}, {"message", "unsupported Milky action"}};
        json result = httpPost(s.path, s.body);
        if (apiOk(result) && action == "get_group_file_url" && result["data"].is_object() && result["data"].contains("download_url"))
            result["data"]["url"] = result["data"]["download_url"];
        if (!apiOk(result)) { std::lock_guard<std::mutex> lk(stateMutex_); lastError_ = result.value("message", std::string("Milky API request failed")); }
        return result;
    }
    void sendAction(const std::string& action, const json& p, ActionCallback cb) {
        invokeActionAsync(action, p, [cb = std::move(cb)](json r) { if (cb) cb(std::move(r)); });
    }
    static std::string jsonBody(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }
    json httpPost(const std::string& path, const json& body) const {
        namespace fs = std::filesystem;
        static std::atomic<uint64_t> requestSeq{0};
        const auto tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++requestSeq);
        const fs::path bodyFile = fs::temp_directory_path() / ("dice-milky-body-" + tag + ".json");
        const fs::path cfgFile = fs::temp_directory_path() / ("dice-milky-curl-" + tag + ".conf");
        { std::ofstream out(bodyFile, std::ios::binary); out << jsonBody(body); }
        auto esc = [](const std::string& s) { std::string out; out.reserve(s.size() + 8); for (char c : s) { if (c == '\\' || c == '"') out += '\\'; out += c; } return out; };
        { std::ofstream out(cfgFile, std::ios::binary); out << "url = \"" << esc(endpoint_ + path) << "\"\nrequest = POST\nsilent\nshow-error\nconnect-timeout = 5\nmax-time = 20\nproto = \"=http,https\"\nheader = \"Content-Type: application/json\"\nheader = \"Authorization: Bearer " << esc(accessToken_) << "\"\ndata-binary = \"@" << esc(bodyFile.string()) << "\"\nwrite-out = \"\\n__DICE_STATUS__%{http_code}\"\n"; }
        auto r = proc::curlConfig(cfgFile, 4 * 1024 * 1024); std::error_code ec; fs::remove(bodyFile, ec); fs::remove(cfgFile, ec);
        const std::string marker = "__DICE_STATUS__"; size_t at = r.output.rfind(marker); int status = 0; std::string content = r.output;
        if (at != std::string::npos) { try { status = std::stoi(r.output.substr(at + marker.size())); } catch (...) {} content.resize(at > 0 && r.output[at - 1] == '\n' ? at - 1 : at); }
        if (!r.ok() || status >= 400 || content.empty()) return json{{"status", "failed"}, {"retcode", status ? status : -1}, {"message", r.output}};
        auto j = json::parse(content, nullptr, false); return j.is_discarded() ? json{{"status", "failed"}, {"retcode", status}, {"message", "invalid Milky JSON response"}} : j;
    }

    void cacheGroupList(const json& r) {
        const json d = r.value("data", json::object());
        if (!apiOk(r) || !d.value("groups", json()).is_array()) return; std::lock_guard<std::mutex> lk(cacheMutex_); groupNames_.clear(); groupCounts_.clear();
        for (const auto& g : d["groups"]) { auto id = field(g, "group_id"); if (id.empty()) continue; groupNames_[id] = g.value("group_name", id); groupCounts_[id] = g.value("member_count", 0); }
    }
    std::string memberRole(const std::string& gid, const std::string& uid) const {
        std::lock_guard<std::mutex> lk(cacheMutex_); auto it = memberRoles_.find(gid + "\x1f" + uid); return it == memberRoles_.end() ? std::string() : it->second;
    }

    void handleMessage(const json& d) {
        Message m; m.platform = platform(); m.adapterId = id_; m.selfId = loginId_; m.id = field(d, "message_seq"); m.timestamp = d.value("time", static_cast<int64_t>(std::time(nullptr))); m.senderId = field(d, "sender_id");
        const std::string scene = d.value("message_scene", "group"); m.type = scene == "friend" ? MessageType::kPrivate : MessageType::kGroup; m.targetId = field(d, "peer_id");
        if (d.contains("group") && d["group"].is_object()) { m.targetId = field(d["group"], "group_id"); m.extra["groupName"] = d["group"].value("group_name", ""); }
        if (d.contains("group_member") && d["group_member"].is_object()) { const auto& s = d["group_member"]; m.senderName = s.value("nickname", s.value("card", "")); m.extra["role"] = s.value("role", "member"); m.extra["card"] = s.value("card", ""); }
        if (m.senderName.empty() && d.contains("friend") && d["friend"].is_object()) m.senderName = d["friend"].value("nickname", "");
        std::string raw, display, clean;
        const auto segs = d.value("segments", json::array()); if (segs.is_array()) for (const auto& s : segs) {
            const std::string t = s.value("type", ""); const auto x = s.value("data", json::object());
            if (t == "text" || t == "markdown") { auto v = x.value("text", x.value("content", "")); raw += v; display += v; clean += v; }
            else if (t == "mention") { auto uid = field(x, "user_id"); raw += "[CQ:at,qq=" + uid + "]"; display += "@" + x.value("name", uid); m.atList.push_back(uid); }
            else if (t == "mention_all") { raw += "[CQ:at,qq=all]"; display += "@全体成员"; m.atList.push_back("all"); }
            else if (t == "face") { raw += "[CQ:face,id=" + field(x, "face_id") + "]"; display += "[表情]"; }
            else if (t == "market_face") { std::string url = x.value("url", std::string()); raw += url.empty() ? "[表情]" : "[CQ:image,file=" + url + "]"; display += "[表情]"; }
            else if (t == "reply") { raw += "[CQ:reply,id=" + field(x, "message_seq") + "]"; display += "[回复]"; }
            else if (t == "image") { std::string ref = x.value("temp_url", std::string()); if (ref.empty()) ref = x.value("resource_id", std::string()); if (ref.empty()) ref = x.value("uri", std::string()); raw += "[CQ:image,file=" + ref + "]"; display += "[图片]"; }
            else if (t == "record") { std::string ref = x.value("temp_url", std::string()); if (ref.empty()) ref = x.value("resource_id", std::string()); raw += "[CQ:record,file=" + ref + "]"; display += "[语音]"; }
            else if (t == "video") { std::string ref = x.value("temp_url", std::string()); if (ref.empty()) ref = x.value("resource_id", std::string()); raw += "[CQ:video,file=" + ref + "]"; display += "[视频]"; }
            else if (t == "file") { raw += "[CQ:file,name=" + x.value("file_name", "") + ",id=" + x.value("file_id", "") + "]"; display += "[文件]"; }
            else if (t == "forward") { raw += "[CQ:forward,id=" + x.value("forward_id", "") + "]"; display += "[合并转发]"; }
            else if (t == "light_app" || t == "xml" || t == "json") display += "[卡片]";
        }
        m.rawContent = raw.empty() ? clean : raw; m.displayContent = display.empty() ? clean : display; m.content = clean; m.extra["raw"] = d; m.extra["message_scene"] = scene;
        if (m.senderId == loginId_ && SelfEchoFilter::instance().consume(platform() + ":" + m.targetId, normalizeEcho(m.content))) return;
        for (auto& cb : messageCallbacks_) cb(m);
    }

    void handleEvent(const std::string& type, const json& d) {
        BotEvent e; e.platform = platform(); e.adapterId = id_; e.selfId = loginId_; e.timestamp = d.value("__event_time", static_cast<int64_t>(std::time(nullptr))); e.extra = d;
        if (type == "bot_offline") { connected_ = false; return; }
        if (type == "group_name_change") {
            const std::string gid = field(d, "group_id");
            const std::string name = d.value("new_group_name", std::string());
            if (!gid.empty() && !name.empty()) { std::lock_guard<std::mutex> lk(cacheMutex_); groupNames_[gid] = name; }
            return;
        }
        if (type == "group_admin_change") {
            const std::string gid = field(d, "group_id"), uid = field(d, "user_id");
            if (!gid.empty() && !uid.empty()) { std::lock_guard<std::mutex> lk(cacheMutex_); memberRoles_[gid + "\x1f" + uid] = d.value("is_set", false) ? "admin" : "member"; }
            return;
        }
        if (type == "message_recall") { if (d.value("message_scene", std::string()) != "group") return; e.type = EventType::kGroupRecall; e.groupId = field(d, "peer_id"); e.extra["message_id"] = field(d, "message_seq"); }
        else if (type == "friend_request") { e.type = EventType::kFriendRequest; e.userId = field(d, "initiator_id"); e.comment = d.value("comment", ""); e.flag = json{{"kind", "friend"}, {"initiator_uid", d.value("initiator_uid", "")}, {"is_filtered", d.value("is_filtered", false)}}.dump(); }
        else if (type == "group_join_request" || type == "group_invited_join_request") { e.type = EventType::kGroupRequest; e.groupId = field(d, "group_id"); e.userId = type == "group_join_request" ? field(d, "initiator_id") : field(d, "target_user_id"); e.operatorId = field(d, "initiator_id"); e.comment = d.value("comment", ""); e.subType = type == "group_join_request" ? "join_request" : "invited_join_request"; e.flag = json{{"kind", "group"}, {"request_type", e.subType}, {"group_id", e.groupId}, {"notification_seq", field(d, "notification_seq")}, {"is_filtered", d.value("is_filtered", false)}}.dump(); }
        else if (type == "group_invitation") { e.type = EventType::kGroupRequest; e.groupId = field(d, "group_id"); e.userId = field(d, "initiator_id"); e.operatorId = e.userId; e.subType = "invite"; e.flag = json{{"kind", "group"}, {"request_type", "invitation"}, {"group_id", e.groupId}, {"notification_seq", field(d, "invitation_seq")}}.dump(); }
        else if (type == "group_member_increase" || type == "group_member_decrease") { e.type = type == "group_member_increase" ? EventType::kGroupIncrease : EventType::kGroupDecrease; e.groupId = field(d, "group_id"); e.userId = field(d, "user_id"); e.operatorId = field(d, "operator_id"); if (e.operatorId.empty()) e.operatorId = field(d, "invitor_id"); }
        else if (type == "group_nudge") { e.type = EventType::kPoke; e.groupId = field(d, "group_id"); e.userId = field(d, "receiver_id"); e.operatorId = field(d, "sender_id"); }
        else if (type == "group_file_upload") { e.type = EventType::kGroupUpload; e.groupId = field(d, "group_id"); e.userId = field(d, "user_id"); e.extra["file"] = {{"id", d.value("file_id", "")}, {"name", d.value("file_name", "")}, {"size", d.value("file_size", static_cast<int64_t>(0))}, {"busid", ""}}; }
        else return;
        for (auto& cb : eventCallbacks_) cb(e);
    }

    json historyMessage(const json& m) const {
        Message converted; converted.extra = json::object(); converted.platform = platform(); converted.adapterId = id_; converted.selfId = loginId_; converted.id = field(m, "message_seq"); converted.senderId = field(m, "sender_id"); converted.targetId = field(m, "peer_id"); converted.senderName = field(m.value("group_member", json::object()), "nickname");
        // Reuse the inbound segment converter so logs retain CQ-compatible media.
        handleHistorySegments(m.value("segments", json::array()), converted);
        return {{"message_id", converted.id}, {"user_id", converted.senderId}, {"raw_message", converted.rawContent}, {"time", m.value("time", 0)}, {"sender", {{"nickname", converted.senderName}, {"card", converted.extra.value("card", std::string())}, {"role", converted.extra.value("role", std::string("member"))}}}};
    }
    static void handleHistorySegments(const json& segs, Message& m) {
        std::string raw, clean; if (segs.is_array()) for (const auto& s : segs) { auto t = s.value("type", ""); auto d = s.value("data", json::object()); if (t == "text") { auto v = d.value("text", ""); raw += v; clean += v; } else if (t == "image") raw += "[CQ:image,file=" + d.value("temp_url", d.value("resource_id", "")) + "]"; else if (t == "face") raw += "[CQ:face,id=" + field(d, "face_id") + "]"; else if (t == "mention") raw += "[CQ:at,qq=" + field(d, "user_id") + "]"; }
        m.rawContent = raw.empty() ? clean : raw; m.content = clean;
    }

    std::string id_, name_, endpoint_, accessToken_, webhookBaseUrl_, webhookToken_;
    std::string loginId_, loginName_;
    std::atomic<bool> connected_{false}, stopping_{false};
    mutable std::mutex stateMutex_, cacheMutex_;
    std::string lastError_;
    std::vector<MessageCallback> messageCallbacks_; std::vector<EventCallback> eventCallbacks_;
    std::map<std::string, std::string> groupNames_, selfRoles_, memberRoles_;
    std::map<std::string, int> groupCounts_;
    std::map<std::string, json> memberLists_;
    std::vector<std::string> friendList_;
};

} // namespace dice
