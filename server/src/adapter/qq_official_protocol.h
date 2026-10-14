#pragma once

#include "../common/utils.h"
#include "../message/qq_official_message.h"
#include <nlohmann/json.hpp>
#include <ctime>
#include <climits>
#include <string>

namespace dice::qqprotocol {
using json = nlohmann::json;

inline std::string text(const json& object, const char* field) {
    if (!object.is_object()) return {};
    const auto it = object.find(field);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}
inline json object(const json& value, const char* field) {
    if (!value.is_object()) return json::object();
    const auto it = value.find(field);
    return it != value.end() && it->is_object() ? *it : json::object();
}
inline int integer(const json& value, const char* field, int fallback = 0) {
    if (!value.is_object()) return fallback;
    const auto it = value.find(field);
    if (it == value.end() || !it->is_number_integer()) return fallback;
    const auto number = it->get<int64_t>();
    return number >= INT_MIN && number <= INT_MAX ? static_cast<int>(number) : fallback;
}

// Events mix Unix seconds and RFC3339 strings. Preserve timezone offsets;
// invalid provider data must not throw out an otherwise useful event.
inline int64_t timestamp(const json& value, int64_t fallback) {
    if (value.is_number_integer()) {
        const auto seconds = value.get<int64_t>();
        return seconds >= 0 ? seconds : fallback;
    }
    if (!value.is_string()) return fallback;
    const auto iso = value.get<std::string>();
    if (iso.size() < 20 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':') return fallback;
    auto digits = [&](size_t pos, size_t count) {
        int number = 0;
        for (size_t i = 0; i < count; ++i) {
            if (pos + i >= iso.size() || iso[pos + i] < '0' || iso[pos + i] > '9') return -1;
            number = number * 10 + iso[pos + i] - '0';
        }
        return number;
    };
    const int year = digits(0, 4), month = digits(5, 2), day = digits(8, 2);
    const int hour = digits(11, 2), minute = digits(14, 2), second = digits(17, 2);
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23
        || minute < 0 || minute > 59 || second < 0 || second > 60) return fallback;
    size_t zone = 19;
    if (iso[zone] == '.') {
        const auto begin = ++zone;
        while (zone < iso.size() && iso[zone] >= '0' && iso[zone] <= '9') ++zone;
        if (zone == begin) return fallback;
    }
    int offset = 0;
    if (zone + 1 == iso.size() && iso[zone] == 'Z') {}
    else if (zone + 6 == iso.size() && (iso[zone] == '+' || iso[zone] == '-') && iso[zone + 3] == ':') {
        const int hours = digits(zone + 1, 2), minutes = digits(zone + 4, 2);
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return fallback;
        offset = (hours * 3600 + minutes * 60) * (iso[zone] == '+' ? 1 : -1);
    } else return fallback;
    std::tm date{};
    date.tm_year = year - 1900; date.tm_mon = month - 1; date.tm_mday = day;
    date.tm_hour = hour; date.tm_min = minute; date.tm_sec = (std::min)(59, second);
#ifdef _WIN32
    const auto epoch = _mkgmtime(&date);
#else
    const auto epoch = timegm(&date);
#endif
    if (epoch < 0 || date.tm_year != year - 1900 || date.tm_mon != month - 1 || date.tm_mday != day) return fallback;
    return static_cast<int64_t>(epoch) - offset + (second == 60 ? 1 : 0);
}
inline int64_t eventTime(const json& event, int64_t fallback) {
    const auto it = event.is_object() ? event.find("timestamp") : event.end();
    return it == event.end() ? fallback : timestamp(*it, fallback);
}
inline std::string shareUrl(const json& response) {
    const auto data = object(response, "data");
    for (const auto& url : {text(data, "url"), text(response, "url_link"), text(response, "share_url")}) {
        const auto checked = qqmsg::imageUrl(url);
        if (!checked.empty()) return checked;
    }
    return {};
}
inline std::string sceneValue(const json& message, const std::string& key) {
    const auto scene = object(message, "message_scene");
    const auto ext = scene.find("ext");
    if (ext == scene.end() || !ext->is_array()) return {};
    const auto prefix = key + '=';
    for (const auto& value : *ext) if (value.is_string()) {
        const auto item = value.get<std::string>();
        if (item.rfind(prefix, 0) == 0) return item.substr(prefix.size());
    }
    return {};
}
inline int intents(bool members) { return (1 << 25) | (1 << 26) | (1 << 30) | (members ? (1 << 24) : 0); }
inline json actions() {
    return json::array({"qq_get_mute", "qq_set_mute", "qq_join_requests", "qq_approve_join", "qq_list_join_strategies",
        "qq_create_join_strategy", "qq_update_join_strategy", "qq_delete_join_strategy", "qq_execute_join_strategy", "qq_update_join_whitelist",
        "qq_group_info", "qq_bot_state", "qq_group_members", "qq_group_member", "qq_blacklist", "qq_set_blacklist", "qq_remove_members",
        "qq_recall", "qq_stream_message", "qq_get_menu", "qq_set_menu", "qq_list_panels", "qq_create_panel", "qq_get_panel", "qq_update_panel",
        "qq_delete_panel", "qq_panel_target", "qq_me", "qq_my_guilds", "qq_guild", "qq_guild_channels", "qq_create_channel", "qq_channel",
        "qq_update_channel", "qq_delete_channel", "qq_interaction", "qq_generate_share_url", "qq_send_message", "qq_input_notify", "qq_wakeup_message",
        "qq_media_files", "qq_upload_prepare", "qq_upload_part_finish", "qq_gateway"});
}

inline std::string validate(const std::string& action, const json& params) {
    if (!params.is_object()) return "params 必须为 JSON 对象。";
    for (const char* field : {"groupId", "userOpenId", "memberOpenId", "strategyId", "panelId", "messageId", "guildId", "channelId", "interactionId", "cursor", "scope", "op", "callbackData"}) {
        const auto it = params.find(field);
        if (it != params.end() && (!it->is_string() || it->get_ref<const std::string&>().size() > 4096)) return std::string(field) + " 必须为字符串，且长度不超过 4096 字节。";
    }
    if (params.contains("body") && !params["body"].is_object()) return "body 必须为 JSON 对象。";
    for (const char* field : {"seconds", "msg_seq"}) if (params.contains(field) && !params[field].is_number_integer()) return std::string(field) + " 必须为整数。";
    if (params.contains("msg_id") && params.contains("event_id")) return "msg_id 与 event_id 不能同时提供。";
    if ((action == "qq_set_blacklist" || action == "qq_remove_members") && !params.contains("memberOpenIds")) return "memberOpenIds 必填。";
    for (const char* field : {"memberOpenIds", "userOpenIds", "groupOpenIds"}) if (params.contains(field)) {
        const auto& values = params[field];
        if (!values.is_array() || values.empty() || values.size() > 20) return std::string(field) + " 每次须提供 1–20 个标识。";
        for (const auto& id : values) if (!id.is_string() || id.get_ref<const std::string&>().empty()) return std::string(field) + " 只能包含非空字符串。";
    }
    if (action == "qq_set_mute") {
        const auto members = params.find("members");
        if (members == params.end() || !members->is_array() || members->empty() || members->size() > 20) return "每次禁言操作须提供 1–20 名成员。";
        for (const auto& member : *members) {
            const auto op = text(member, "op");
            if (text(member, "member_openid").empty() || (op != "add" && op != "update" && op != "del")) return "禁言成员须提供 member_openid 和有效的 add/update/del 操作。";
            if (op != "del" && text(member, "mute_expire_at").empty()) return "增加或更新禁言必须提供 mute_expire_at。";
        }
    }
    if (params.contains("limit")) {
        const auto limit = integer(params, "limit", -1);
        const int maximum = action == "qq_join_requests" ? 50 : 100;
        if (limit < 1 || limit > maximum) return "limit 超出官方接口允许的范围。";
    }
    for (const char* field : {"addToMemberBlacklist"}) if (params.contains(field) && !params[field].is_boolean()) return std::string(field) + " 必须为布尔值。";
    const auto op = text(params, "op");
    if ((action == "qq_set_blacklist" || action == "qq_update_join_whitelist" || action == "qq_panel_target") && !op.empty() && op != "add" && op != "del") return "op 仅支持 add/del。";
    if (action == "qq_approve_join" && !op.empty() && op != "approve" && op != "decline") return "审批仅支持 approve/decline。";
    if (action == "qq_generate_share_url") {
        const auto callback = text(params, "callbackData");
        size_t characters = 0;
        for (unsigned char value : callback) if ((value & 0xc0) != 0x80) ++characters;
        if (characters > 32) return "callbackData 最多 32 个字符。";
    }
    return {};
}
} // namespace dice::qqprotocol
