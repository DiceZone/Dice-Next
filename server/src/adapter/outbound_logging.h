#pragma once
#include "../common/log_context.h"
#include <optional>
#include <map>

namespace dice {
inline std::string logPayloadField(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key) || value[key].is_null()) return "";
    return value[key].is_string() ? value[key].get<std::string>()
        : value[key].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
inline std::string logPayloadText(const nlohmann::json& value, int depth = 0) {
    if (depth > 12) return "[嵌套内容]";
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) {
        std::string result;
        for (const auto& item : value) result += logPayloadText(item, depth + 1);
        return result;
    }
    if (!value.is_object()) return "";
    const auto type = value.contains("type") && value["type"].is_string() ? value["type"].get<std::string>() : "";
    const auto data = value.value("data", nlohmann::json::object());
    if (type == "text" && data.is_object()) return logPayloadField(data, "text");
    if (type == "at" || type == "mention") {
        auto id = logPayloadField(data, "qq");
        if (id.empty()) id = logPayloadField(data, "user_id");
        return "[提及：" + (id.empty() ? std::string("未提供") : id) + "]";
    }
    if (type == "node" && data.is_object()) return "\n[转发节点] " + logPayloadText(data.value("content", nlohmann::json()), depth + 1);
    if (type == "forward" && data.is_object()) return "[合并转发] " + logPayloadText(data.value("messages", nlohmann::json()), depth + 1);
    for (const auto* key : {"text", "content", "message", "segments"})
        if (value.contains(key)) return logPayloadText(value[key], depth + 1);
    if (!type.empty()) {
        const auto title = type == "image" ? "图片" : type == "record" || type == "audio" ? "语音"
            : type == "file" ? "文件" : type == "video" ? "视频" : type == "reply" ? "引用" : type.c_str();
        auto detail = logPayloadField(data, "name");
        if (detail.empty()) detail = logPayloadField(data, "file");
        if (detail.empty()) detail = logPayloadField(data, "url");
        if (detail.rfind("base64", 0) == 0 || detail.rfind("data:", 0) == 0) detail = "内嵌数据";
        return "[" + std::string(title) + (detail.empty() ? "" : "：" + detail) + "]";
    }
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
inline std::string logJsonId(const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null()) return "";
    return logPayloadField(value, key);
}
inline std::optional<OutboundAttempt> prepareActionLog(const std::string& adapter,
        const std::string& action, const nlohmann::json& params) {
    if (!params.is_object()) return std::nullopt;
    const auto gid = logJsonId(params, "group_id"), uid = logJsonId(params, "user_id");
    const auto target = !gid.empty() ? "群 " + gid : !uid.empty() ? "私聊 " + uid : "平台";
    if (action == "send_group_msg" || action == "send_private_msg" || action == "send_guild_channel_msg"
        || action == "send_group_message" || action == "send_private_message")
        return OutboundAttempt::prepare(adapter, target, logPayloadText(params.value("message", nlohmann::json())));
    if (action == "send_group_forward_msg")
        return OutboundAttempt::prepare(adapter, target, "[合并转发] " + logPayloadText(params.value("messages", nlohmann::json())));
    static const std::map<std::string, std::string> events = {
        {"set_group_kick", "移出群成员"}, {"kick_group_member", "移出群成员"},
        {"set_group_ban", "设置成员禁言"}, {"set_group_member_mute", "设置成员禁言"},
        {"set_group_leave", "退出群聊"}, {"quit_group", "退出群聊"}, {"delete_friend", "删除好友"},
        {"set_group_card", "修改群名片"}, {"set_group_member_card", "修改群名片"},
        {"set_group_name", "修改群名称"}, {"set_group_special_title", "修改成员头衔"},
        {"set_group_member_special_title", "修改成员头衔"}, {"group_poke", "戳一戳"},
        {"send_group_nudge", "戳一戳"}, {"send_friend_nudge", "戳一戳"},
        {"upload_group_file", "上传群文件"}, {"set_friend_add_request", "处理好友申请"},
        {"set_group_add_request", "处理入群申请"}, {"accept_friend_request", "同意好友申请"},
        {"reject_friend_request", "拒绝好友申请"}, {"accept_group_request", "同意入群申请"},
        {"reject_group_request", "拒绝入群申请"}, {"accept_group_invitation", "同意群邀请"},
        {"reject_group_invitation", "拒绝群邀请"}
    };
    const auto event = events.find(action);
    if (event == events.end()) return std::nullopt; // never log polling as an outgoing user message
    std::string content = event->second;
    // Explicit semantic fields only; no request flags, tokens or file bodies.
    for (const auto& [key, title] : std::vector<std::pair<const char*, const char*>>{
        {"user_id", "成员"}, {"duration", "秒数"}, {"card", "名片"}, {"group_name", "群名"},
        {"name", "文件名"}, {"special_title", "头衔"}, {"approve", "是否同意"}, {"reason", "原因"}})
        if (params.contains(key)) content += "；" + std::string(title) + "：" + logJsonId(params, key);
    return OutboundAttempt::prepare(adapter, target, content, "事件");
}
inline std::optional<OutboundAttempt> prepareRestEventLog(const std::string& adapter,
        const std::string& method, const std::string& path, const nlohmann::json& body) {
    if (method == "POST" && path == "/api/v3/guild/kickout")
        return OutboundAttempt::prepare(adapter, "服务器 " + logJsonId(body, "guild_id"),
            "移出服务器成员：" + logJsonId(body, "target_id"), "事件");
    constexpr std::string_view prefix = "/api/v10/guilds/";
    const auto member = path.find("/members/", prefix.size());
    if (path.rfind(prefix, 0) != 0 || member == std::string::npos || (method != "DELETE" && method != "PATCH"))
        return std::nullopt;
    const auto user = path.substr(member + std::string_view("/members/").size());
    const auto until = logJsonId(body, "communication_disabled_until");
    if (method == "PATCH" && !body.contains("communication_disabled_until")) return std::nullopt;
    const auto action = method == "DELETE" ? "移出服务器成员：" + user
        : until.empty() ? "解除成员超时禁言：" + user : "设置成员超时禁言：" + user + "；结束时间：" + until;
    return OutboundAttempt::prepare(adapter, "服务器 " + path.substr(prefix.size(), member - prefix.size()), action, "事件");
}
} // namespace dice
