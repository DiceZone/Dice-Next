#pragma once
#include "notice_content.h"
#include "../adapter/group_notice_semantics.h"
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <limits>
#include <tuple>

namespace dice::notice {
// Subscription keys stay stable; the human title describes the actual result.
struct EventText {
    std::string operation, title, body;
    int level = 2;
};
inline std::string groupIdentity(const std::string& name, const std::string& id) {
    return "群" + identity(name, id, "");
}
inline std::string operatorIdentity(const ProcessingOrigin& origin) {
    if (!origin.userId.empty()) return identity(origin.userName, origin.userId);
    return origin.entry.find("网页") != std::string::npos || origin.entry.find("Web") != std::string::npos
        ? "面板管理员" : "操作人（未提供可核实的昵称和帐号）";
}
inline std::string durationText(const nlohmann::json& value) {
    if (!value.is_number_integer() || value < 0 || value > (std::numeric_limits<int64_t>::max)()) return "（平台未提供有效时长）";
    auto seconds = value.get<int64_t>();
    if (!seconds) return "（平台未提供有效时长）";
    std::string out;
    for (const auto& [unit, label] : std::vector<std::pair<int64_t, const char*>>{
             {86400, "天"}, {3600, "小时"}, {60, "分钟"}, {1, "秒"}}) {
        if (seconds >= unit) { out += std::to_string(seconds / unit) + label; seconds %= unit; }
    }
    return out;
}
struct LeaveIntent {
    std::string operation = "leave", cause = "主动退群", reason, groupName;
    ProcessingOrigin actor;
};
// A bounded, short-lived correlation record, not evidence that a platform
// accepted the request. A kick that names someone other than the bot still
// overrides it, as does disband. A kick/removed echo with no other operator
// confirms the request: OneBot implementations often report set_group_leave
// that way, and that echo must not be recorded as a kick.
class LeaveIntents {
public:
    using Clock = std::chrono::steady_clock;
    using Key = std::tuple<std::string, std::string, std::string>;
    static LeaveIntents& instance() { static LeaveIntents value; return value; }
    uint64_t remember(const std::string& adapter, const std::string& platform,
                      const std::string& group, LeaveIntent intent, Clock::time_point at = Clock::now()) {
        std::lock_guard lock(mutex_); prune(at);
        if (items_.size() >= 512) {
            const auto oldest = std::min_element(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return a.second.at < b.second.at; });
            items_.erase(oldest);
        }
        const auto id = ++sequence_;
        items_[{adapter, platform, group}] = {id, at, std::move(intent)}; return id;
    }
    void cancel(uint64_t id) {
        std::lock_guard lock(mutex_);
        for (auto it = items_.begin(); it != items_.end(); ++it) if (it->second.id == id) { items_.erase(it); return; }
    }
    std::optional<LeaveIntent> take(const std::string& adapter, const std::string& platform,
                                   const std::string& group, Clock::time_point at = Clock::now()) {
        std::lock_guard lock(mutex_); prune(at);
        auto it = items_.find({adapter, platform, group});
        if (it == items_.end()) return std::nullopt;
        auto result = std::move(it->second.intent); items_.erase(it); return result;
    }
    std::optional<LeaveIntent> peek(const std::string& adapter, const std::string& platform,
                                   const std::string& group, Clock::time_point at = Clock::now()) {
        std::lock_guard lock(mutex_); prune(at);
        auto it = items_.find({adapter, platform, group});
        if (it == items_.end()) return std::nullopt;
        return it->second.intent;
    }
private:
    struct Entry { uint64_t id; Clock::time_point at; LeaveIntent intent; };
    void prune(Clock::time_point at) {
        for (auto it = items_.begin(); it != items_.end();) {
            if (at - it->second.at >= std::chrono::minutes(5)) it = items_.erase(it); else ++it;
        }
    }
    std::mutex mutex_; std::map<Key, Entry> items_; uint64_t sequence_ = 0;
};
inline std::string requestedBy(const LeaveIntent& intent) {
    if (intent.cause == "指令 .dismiss") return operatorIdentity(intent.actor) + "发送退群指令";
    if (intent.cause == "面板退群") return operatorIdentity(intent.actor) + "通过管理面板";
    return readable(intent.cause, 300);
}
inline EventText leaveRequested(const std::string& groupId, const LeaveIntent& intent) {
    auto body = requestedBy(intent) + "请求骰娘退出" + groupIdentity(intent.groupName, groupId) +
        "，待平台确认。";
    if (!intent.reason.empty()) body += "\n原因：" + readable(intent.reason, 800);
    return {intent.operation, "退群请求已提交", std::move(body), 2};
}
inline bool namesAnotherOperator(const BotEvent& event) {
    if (event.operatorId.empty() || event.operatorId == "0") return false;
    return event.selfId.empty() || event.operatorId != event.selfId;
}
inline bool reportedAsKick(const std::string& subType) {
    return subType == "kick" || subType == "kick_me" || subType == "removed";
}
inline std::optional<LeaveIntent> pendingLeave(const BotEvent& event) {
    auto intent = LeaveIntents::instance().peek(event.adapterId, event.platform, event.groupId);
    if (intent || !event.extra.is_object()) return intent;
    const auto native = event.extra.find("__identity_native_target");
    if (native == event.extra.end() || !native->is_string()) return std::nullopt;
    return LeaveIntents::instance().peek(event.adapterId, event.platform, native->get<std::string>());
}
// QQ often echoes the bot's own set_group_leave as kick/kick_me/removed with
// operator 0 or the bot itself. Once a leave request is on record, that echo
// is the confirmation, and plugins should see a leave rather than a kick.
inline void reconcileOwnLeaveEcho(BotEvent& event) {
    if (event.type != EventType::kGroupDecrease) return;
    if (event.selfId.empty() || event.userId != event.selfId || event.groupId.empty()) return;
    if (!reportedAsKick(event.subType) || namesAnotherOperator(event) || !pendingLeave(event)) return;
    if (!event.extra.is_object()) event.extra = nlohmann::json::object();
    event.extra["platform_departure"] = event.subType;
    event.subType = "leave";
}
inline EventText groupDeparture(const BotEvent& event, const std::string& groupName,
                               const std::string& operatorName, const std::optional<LeaveIntent>& intent = {}) {
    const auto label = identity(groupName.empty() && intent ? intent->groupName : groupName, event.groupId, "");
    const auto group = "群" + label;
    const bool actorKnown = !event.operatorId.empty() && event.operatorId != "0";
    const auto actor = actorKnown ? identity(operatorName, event.operatorId) : std::string();
    auto confirmRequested = [&]() {
        auto body = "平台已确认骰娘退出" + group + "。";
        body += intent->operation == "dismiss" ? "由退群指令优雅退群。" : "此前由" + requestedBy(*intent) + "发起退群。";
        if (!intent->reason.empty()) body += "\n原因：" + readable(intent->reason, 800);
        return EventText{"group_left", "骰娘退群已确认", std::move(body), 2};
    };
    if (reportedAsKick(event.subType)) {
        if (intent && !namesAnotherOperator(event)) return confirmRequested();
        return {"group_left", "骰娘被踢出群聊", label + (actorKnown ? "，操作者：" + actor : ""), 2};
    }
    if (event.subType == "disband")
        return {"group_left", "群聊已解散", label + (actorKnown ? "，群主：" + actor : ""), 2};
    if (event.subType == "leave") {
        if (intent) return confirmRequested();
        return {"group_left", "骰娘主动退出群聊", "骰娘主动退出" + group + "；未关联到本程序近期提交的退群请求，可能来自其他客户端或已失效的历史请求。", 2};
    }
    auto body = "骰娘已离开" + group + "；平台未提供明确的离群原因，无法确认是被踢还是主动退出。";
    if (actorKnown) body += "\n平台上报的操作者：" + actor + "。";
    if (intent) body += "\n此前曾由" + requestedBy(*intent) + "提交退群请求，但不能据此认定本次离群原因。";
    return {"group_left", "骰娘离群（原因未知）", std::move(body), 2};
}
inline EventText groupModeration(const BotEvent& event, const std::string& groupName,
                                const std::string& operatorName) {
    const auto group = groupIdentity(groupName, event.groupId);
    const bool actorKnown = !event.operatorId.empty() && event.operatorId != "0";
    const auto by = actorKnown ? "被" + identity(operatorName, event.operatorId) : "被";
    const auto unknown = actorKnown ? std::string() : "（平台未提供操作者）";
    if (event.type == EventType::kGroupAdmin) {
        if (event.subType != "set" && event.subType != "unset")
            return {"group_admin", "骰娘群管理权限变更", "骰娘在" + group + "中的管理员权限发生变化；平台未提供设置或取消的明确类型。", 2};
        const bool set = event.subType == "set";
        const auto adminBy = actorKnown ? by : "被群主";
        return {"group_admin", set ? "骰娘获得群管理权限" : "骰娘群管理权限被取消",
            "骰娘在" + group + "中" + adminBy + (set ? "设为管理员" : "取消了管理员权限") + "。", 2};
    }
    if (event.subType != "ban" && event.subType != "lift_ban" && event.subType != "whole_ban" && event.subType != "whole_lift_ban")
        return {"group_ban", "群禁言状态变更", group + "的禁言状态发生变化；平台未提供禁言或解除禁言的明确类型。", 2};
    const bool whole = event.userId == "0" || event.subType == "whole_ban" || event.subType == "whole_lift_ban";
    const bool lift = event.subType == "lift_ban" || event.subType == "whole_lift_ban";
    if (whole) return {"group_ban", lift ? "群全体禁言已解除" : "群已开启全体禁言",
        group + (actorKnown ? "已由" + identity(operatorName, event.operatorId) : "已") + (lift ? "关闭了全体禁言" : "开启了全体禁言") + unknown + "。", 2};
    if (lift) return {"group_ban", "骰娘禁言已解除", "骰娘在" + group + "中" + by + "解除了禁言" + unknown + "。", 2};
    const auto duration = event.extra.is_object() ? event.extra.value("duration", nlohmann::json()) : nlohmann::json();
    return {"group_ban", "骰娘被禁言", "骰娘在" + group + "中" + by + "禁言" + durationText(duration) + unknown + "。", 2};
}
inline std::string joinInviter(const BotEvent& event) {
    // An approval operator is not necessarily the person who invited the bot.
    auto id = noticeId(event.extra, "invitor_id");
    if (id.empty()) id = noticeId(event.extra, "inviter_id");
    if (id.empty() && event.subType == "invite") id = event.operatorId;
    return id == "0" || id == event.selfId ? std::string() : id;
}
inline EventText groupJoined(const BotEvent& event, const std::string& groupName, const std::string& inviterId, const std::string& inviterName) {
    return {"group_joined", "骰娘已入群", "新加入群：" + identity(groupName, event.groupId, "") +
        (inviterId.empty() ? "，邀请人未知。" : "，邀请人：" + identity(inviterName, inviterId)), 2};
}
inline EventText listChanged(int type, int list, const std::string& id, const std::string& name,
                             bool added, const std::string& reason = "") {
    const std::string label = list == 0 ? "黑名单" : list == 1 ? "白名单" : "骰娘名单";
    return {"blacklist", label + "变更", "已将" + (type == 1 ? groupIdentity(name, id) : identity(name, id)) +
        (added ? "加入" : "移出") + label + "。" + (reason.empty() ? "" : "\n原因：" + readable(reason, 800)), 4};
}
inline EventText ruleChanged(const std::string& name, const std::string& verb, size_t channels) {
    return {"notice_manage", "通知订阅变更", "已" + verb + "通知规则「" + readable(name.empty() ? "未命名规则" : name, 240) +
        "」" + (verb == "删除" ? "。" : "，包含 " + std::to_string(channels) + " 个通知渠道。"), 1};
}
inline EventText scheduleChanged(const ScheduledTaskRow& task, const std::string& verb) {
    std::string timing = task.triggerType == "interval" ? "每 " + std::to_string(task.intervalMin) + " 分钟"
        : task.triggerType == "once" ? task.onceDate + " " + task.cronTime : "每天 " + task.cronTime;
    if (!task.days.empty()) timing += "（星期限制：" + task.days + "）";
    return {"schedule_manage", "定时任务变更", "已" + verb + "定时任务「" + readable(task.name, 240) + "」" +
        (verb == "删除" ? "。" : "；" + (task.enabled ? std::string("已启用") : "已停用") + "，执行时间：" + readable(timing, 160) + "（服务器时区）。"), 1};
}
inline EventText requestDecision(const BotEvent& event, const std::string& userName, const std::string& groupName,
                                 const std::string& outcome, const std::string& reason) {
    const bool friendRequest = event.type == EventType::kFriendRequest;
    const bool invitation = event.subType == "invite";
    const auto subject = identity(userName, event.userId);
    auto body = friendRequest ? "收到" + subject + "的好友申请。" : invitation
        ? "收到" + subject + "邀请骰娘加入" + groupIdentity(groupName, event.groupId) + "。"
        : "收到" + subject + "加入" + groupIdentity(groupName, event.groupId) + "的申请。";
    body += "\n处理：" + outcome + (reason.empty() ? "。" : "（" + readable(reason, 400) + "）。");
    if (!event.comment.empty()) body += "\n附言：" + readable(event.comment, 1000);
    return {friendRequest ? "friend_req" : invitation ? "group_invite" : "group_request",
        friendRequest ? "好友申请" : invitation ? "加群邀请" : "群成员入群申请", std::move(body), 2};
}
} // namespace dice::notice
