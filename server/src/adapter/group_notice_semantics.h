#pragma once
#include "adapter_interface.h"

namespace dice {
inline std::string noticeId(const json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return {};
    const auto& id = value[key];
    if (id.is_string()) return id.get<std::string>();
    if (id.is_number_integer()) return id.dump();
    return {};
}
inline bool oneBotModerationEvent(BotEvent& event, const std::string& type, const json& data) {
    if (!data.is_object()) return false;
    if (type != "group_ban" && type != "group_admin") return false;
    event.type = type == "group_ban" ? EventType::kGroupBan : EventType::kGroupAdmin;
    event.groupId = noticeId(data, "group_id"); event.userId = noticeId(data, "user_id");
    event.operatorId = noticeId(data, "operator_id");
    const auto sub = data.find("sub_type");
    event.subType = sub != data.end() && sub->is_string() ? sub->get<std::string>() : "unknown";
    event.extra = data;
    return true;
}
inline bool milkyModerationEvent(BotEvent& event, const std::string& type, const json& data) {
    if (!data.is_object()) return false;
    if (type != "group_mute" && type != "group_whole_mute" && type != "group_admin_change") return false;
    event.groupId = noticeId(data, "group_id"); event.userId = noticeId(data, "user_id");
    event.operatorId = noticeId(data, "operator_id"); event.extra = data; event.subType = "unknown";
    if (type == "group_admin_change") {
        event.type = EventType::kGroupAdmin;
        const auto set = data.find("is_set");
        if (set != data.end() && set->is_boolean()) event.subType = set->get<bool>() ? "set" : "unset";
    } else {
        event.type = EventType::kGroupBan;
        if (type == "group_whole_mute") {
            event.userId = "0";
            const auto mute = data.find("is_mute");
            if (mute != data.end() && mute->is_boolean()) event.subType = mute->get<bool>() ? "whole_ban" : "whole_lift_ban";
        } else {
            const auto duration = data.find("duration");
            if (duration != data.end() && duration->is_number_integer() && *duration >= 0)
                event.subType = *duration == 0 ? "lift_ban" : "ban";
        }
    }
    return true;
}
inline std::string milkyDepartureType(const std::string& type, const json& data) {
    if (type == "group_disband") return "disband";
    if (type != "group_member_decrease") return {};
    const auto actor = noticeId(data, "operator_id");
    return actor.empty() || actor == "0" ? "leave" : "kick";
}
} // namespace dice
