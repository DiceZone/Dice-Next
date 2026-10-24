#pragma once
#include "../adapter/adapter_manager.h"
#include "../common/log_context.h"
#include <algorithm>
#include <string>

namespace dice::notice {
// Bounds are UTF-8 safe. Notification text is diagnostic plain text, not a way
// for a user/plugin/release note to inject CQ actions into a subscribed group.
inline std::string readable(std::string_view value, size_t limit = 6000) {
    std::string text = sanitizeLogText(value);
    text = nlohmann::json::parse(
               nlohmann::json(text).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))
               .get<std::string>();
    // Log files indent continuations to prevent forged log headers. A chat
    // notification is already a single message; don't copy the log gutter.
    for (size_t at = 0; (at = text.find("\n    | ", at)) != std::string::npos; ++at)
        text.erase(at + 1, 6);
    for (size_t at = 0; (at = text.find("[CQ:", at)) != std::string::npos; at += 6)
        text.replace(at, 1, "［");
    if (text.size() > limit) {
        auto end = limit;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
            --end;
        text.resize(end);
        text += "\n…（内容过长，完整记录请查看后台日志／发布页）";
    }
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](unsigned char c) { return (c < 32 && c != '\n' && c != '\t') || c == 127; }),
               text.end());
    return text;
}
inline std::string identity(const std::string &name, const std::string &id,
                            const std::string &fallback = "用户") {
    auto n = readable(name, 240), i = readable(id, 180);
    std::replace(n.begin(), n.end(), '\n', ' ');
    std::replace(i.begin(), i.end(), '\n', ' ');
    return n.empty() || n == i || n == "用户" || n == "群" || n == "群组"
               ? fallback + (i.empty() ? "（平台未提供帐号）" : "(" + i + ")")
               : n + (i.empty() ? "" : "(" + i + ")");
}
inline void enrichOrigin(ProcessingOrigin &origin, AdapterManager &adapters) {
    adapters.enrichLogOrigin(origin, true);
}
inline ProcessingOrigin messageOrigin(const Message &message, AdapterManager &adapters) {
    return adapters.logOrigin(message);
}
inline ProcessingOrigin eventOrigin(const BotEvent &event, AdapterManager &adapters) {
    return adapters.logOrigin(event);
}
inline std::string eventTitle(const std::string &op) {
    static const std::vector<std::pair<std::string, std::string>> titles{
        {"trust", "信任等级变更"},
        {"alias", "帐号别名变更"},
        {"friend_req", "收到好友申请"},
        {"friend_add", "新好友已添加"},
        {"group_invite", "收到加群邀请"},
        {"leave", "退群请求"},
        {"dismiss", "退群操作"},
        {"group_left", "骰娘离开群聊"},
        {"group_joined", "骰娘加入群聊"},
        {"group_request", "群成员入群申请"},
        {"group_ban", "骰娘／全群禁言变更"},
        {"group_admin", "骰娘群管理权限变更"},
        {"group_size_leave", "群规模超限退群"},
        {"group_auto_clean", "自动清理群聊"},
        {"usergroup_leave", "用户群要求触发退群"},
        {"leave_error", "退群请求失败"},
        {"group_control", "群指令开关变更"},
        {"schedule_manage", "定时任务变更"},
        {"notice_manage", "通知订阅变更"},
        {"blacklist_leave", "黑名单触发退群"},
        {"nonfriend_leave", "非好友邀请触发退群"},
        {"keyword_leave", "群名规则触发退群"},
        {"schedule", "定时任务"},
        {"global", "全局设置变更"},
        {"censor", "敏感词检测"},
        {"update_available", "发现新版本"},
        {"update_result", "更新进度与结果"},
        {"admin", "管理权限变更"},
        {"blacklist", "黑白名单变更"},
        {"error", "运行错误"},
        {"update_error", "程序更新失败"},
        {"test", "测试通知"}};
    for (const auto &[key, title] : titles)
        if (key == op)
            return title;
    return op.empty() ? "系统通知" : "通知 · " + readable(op, 120);
}
inline std::string adapterLabel(const ProcessingOrigin &o) {
    const auto platform = o.platform == "onebot_v11"    ? "OneBot"
                          : o.platform == "qq_official" ? "QQ官方"
                                                        : o.platform;
    if (!o.accountId.empty())
        return readable(platform, 80) + ":" + readable(o.accountId, 180);
    return o.accountName.empty() ? readable(platform, 80)
                                 : readable(platform, 80) + "「" + readable(o.accountName, 240) + "」";
}
inline std::string contextText(const ProcessingOrigin &o, const std::string &body = "") {
    std::vector<std::string> parts;
    if (!o.groupId.empty() && body.find(o.groupId) == std::string::npos)
        parts.push_back("群" + identity(o.groupName, o.groupId, ""));
    if (!o.userId.empty() && o.userId != "0" && o.userId != o.accountId && body.find(o.userId) == std::string::npos)
        parts.push_back("用户" + identity(o.userName, o.userId, ""));
    if (!o.targetId.empty() && o.targetId != o.groupId && o.targetId != o.userId &&
        body.find(o.targetId) == std::string::npos)
        parts.push_back(o.targetId == "*" ? "所有符合条件的群组"
                                          : std::string(o.targetType == "group" ? "目标群" : "目标用户") +
                                                identity(o.targetName, o.targetId, ""));
    if ((!o.accountId.empty() || !o.accountName.empty()) &&
        (o.accountId.empty() || body.find(o.accountId) == std::string::npos))
        parts.push_back("适配器 " + adapterLabel(o));
    std::string text;
    for (const auto &part : parts) {
        if (!text.empty())
            text += "，";
        text += part;
    }
    return text;
}
inline std::string triggerText(std::string text) {
    auto lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (const auto *command : {".master", "。master", "!master", "！master", ".email", "。email", "!email",
                                "！email", ".auth", ".password"})
        if (lowered.find(command) != std::string::npos)
            return "[身份验证内容已隐藏]";
    return readable(text, 800);
}
inline std::string operationActor(const ProcessingContext& processing) {
    for (auto it = processing.sources.rbegin(); it != processing.sources.rend(); ++it)
        if (it->kind.find("插件") != std::string::npos)
            return it->name.empty() ? "插件" : "插件「" + readable(it->name, 240) + "」";
    const auto& origin = processing.origin;
    if (!origin.userId.empty()) return identity(origin.userName, origin.userId);
    if (origin.entry.find("网页") != std::string::npos || origin.entry.find("Web") != std::string::npos)
        return "面板操作人（未提供可核实的昵称和帐号）";
    return "系统";
}
inline std::string levelNames(int mask) {
    std::string result;
    for (const auto& [bit, label] : std::vector<std::pair<int, const char*>>{
             {1, "例行"}, {2, "重要"}, {4, "关键"}, {8, "错误"}})
        if (mask & bit) result += (result.empty() ? "" : "、") + std::string(label);
    return result.empty() ? "无" : result;
}
inline std::string notificationText(int /*level*/, const std::string &op, const std::string &message,
                                    const ProcessingContext &processing, const std::string & /*time*/,
                                    const std::string& title = "") {
    // ErrorReport already contains its business title and useful context.
    if (op == "error")
        return readable(message, 6000);
    const bool management = op == "trust" || op == "alias" || op == "global" || op == "admin" || op == "blacklist" || op == "schedule_manage" || op == "notice_manage" || op == "group_control";
    std::string text = "[" + (title.empty() ? eventTitle(op) : readable(title, 120)) + "] " +
        (management ? operationActor(processing) + "：" : "") + readable(message, 10000);
    const auto context = contextText(processing.origin, text);
    if (!context.empty())
        text += "\n" + context;
    return text;
}
struct ErrorReport {
    nlohmann::json record;
    ProcessingContext processing;
    size_t suppressed = 0;
    std::string text() const {
        auto value = [](const nlohmann::json &object, const char *key) {
            const auto i = object.is_object() ? object.find(key) : object.end();
            return i != object.end() && i->is_string() ? i->get<std::string>() : std::string();
        };
        const auto details = record.is_object() && record.contains("details") && record["details"].is_object()
                                 ? record["details"]
                                 : nlohmann::json::object();
        auto message = value(record, "message");
        // Older send errors included the complete failed message and IDs.
        // Never quote that body into another notification.
        const auto content = message.find("；实际内容：");
        if (content != std::string::npos)
            message.resize(content);
        const auto correlation = message.find("(msgID:");
        if (correlation != std::string::npos)
            message.resize(correlation);
        auto original = value(details, "error");
        if (original.empty())
            original = value(details, "plugin_text");
        if (original.empty())
            original = message;
        if (original.empty())
            original = "程序未返回具体错误。";
        const ProcessingSource *plugin = nullptr;
        for (const auto &source : processing.sources)
            if (source.kind.find("插件") != std::string::npos)
                plugin = &source;
        const auto module = value(record, "module");
        const auto operation = value(details, "operation");
        const bool delivery = operation == "发送消息" || operation == "发送通知" || operation == "执行事件";
        std::string text;
        if (plugin && !delivery) {
            text = "[插件错误] ";
            if (operation.empty() && !plugin->name.empty() &&
                original.find(plugin->name) != std::string::npos)
                text += readable(original, 3500);
            else {
                text += plugin->name.empty() ? "插件" : "插件「" + readable(plugin->name, 240) + "」";
                if (!operation.empty())
                    text += readable(operation, 120) + "失败";
                if (!plugin->file.empty()) {
                    const auto at = plugin->file.find_last_of("/\\");
                    const auto filename = plugin->file.substr(at == std::string::npos ? 0 : at + 1);
                    if (!filename.empty() && original.find(filename) == std::string::npos &&
                        plugin->name != filename)
                        text += "（" + readable(plugin->file, 500) + "）";
                }
                text += "：" + readable(original, 3500);
            }
        } else {
            const auto category = module.find("插件") != std::string::npos   ? "插件"
                                  : module == "消息"                         ? "消息"
                                  : module == "通知"                         ? "通知"
                                  : module == "事件"                         ? "操作"
                                  : module == "自动更新"                     ? "更新"
                                  : module.find("定时") != std::string::npos ? "定时任务"
                                  : module == "配置"                         ? "配置"
                                                                             : "运行";
            text =
                "[" + std::string(category) + "错误] " + readable(message.empty() ? original : message, 3500);
            // Include the first distinct underlying error, not a diagnostic
            // field dump. Full structured records remain in the local log.
            for (const auto *key : {"error", "reason", "exception", "cause", "stderr"}) {
                const auto detail = value(details, key);
                if (!detail.empty() && text.find(readable(detail, 1200)) == std::string::npos) {
                    text += "；" + readable(detail, 1200);
                    break;
                }
            }
            const auto path = value(details, "path");
            if (!path.empty() && text.find(path) == std::string::npos)
                text += "（" + readable(path, 500) + "）";
            if (plugin && !plugin->name.empty())
                text += "\n来源：插件「" + readable(plugin->name, 240) + "」";
        }
        if (plugin && !delivery) {
            const auto reason = value(details, "reason");
            if (!reason.empty() && text.find(readable(reason, 1000)) == std::string::npos)
                text += "；" + readable(reason, 1000);
        }
        const auto context = contextText(processing.origin, text);
        if (!context.empty())
            text += "\n" + context;
        if (suppressed)
            text += "（相同错误另出现 " + std::to_string(suppressed) + " 次，已合并）";
        return text;
    }
};
} // namespace dice::notice
