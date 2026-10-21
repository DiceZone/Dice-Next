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
        {"leave", "已退出群聊"},
        {"dismiss", "退群操作"},
        {"group_left", "骰娘离开群聊"},
        {"blacklist_leave", "黑名单触发退群"},
        {"nonfriend_leave", "非好友邀请触发退群"},
        {"keyword_leave", "群名规则触发退群"},
        {"group_size_leave", "群规模超限退群"},
        {"schedule", "定时任务"},
        {"global", "全局设置变更"},
        {"censor", "敏感词检测"},
        {"update_available", "发现新版本"},
        {"update_result", "更新进度与结果"},
        {"admin", "管理权限变更"},
        {"blacklist", "黑名单变更"},
        {"error", "运行错误"},
        {"update_error", "程序更新失败"},
        {"test", "测试通知"}};
    for (const auto &[key, title] : titles)
        if (key == op)
            return title;
    return op.empty() ? "系统通知" : "通知 · " + readable(op, 120);
}
inline std::string contextText(const ProcessingOrigin &o) {
    std::string text;
    if (!o.adapterId.empty() || !o.accountId.empty())
        text += "\n来源帐号：" + readable(o.platform, 100) + " · " +
                identity(o.accountName, o.accountId, "机器人") +
                (o.adapterId.empty() ? "" : "〔适配器 " + readable(o.adapterId, 100) + "〕");
    if (!o.entry.empty())
        text += "\n触发入口：" + readable(o.entry, 160);
    if (!o.userId.empty())
        text += "\n操作／触发者：" + identity(o.userName, o.userId);
    if (!o.groupId.empty())
        text += "\n群组／频道：" + identity(o.groupName, o.groupId, "群组");
    if (!o.messageId.empty())
        text += "\n触发消息 ID：" + readable(o.messageId, 180);
    if (!o.targetId.empty())
        text += "\n操作对象：" + (o.targetId == "*" ? std::string("所有符合条件的群组")
                                                    : identity(o.targetName, o.targetId,
                                                               o.targetType == "group" ? "群组" : "用户"));
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
inline std::string notificationText(int level, const std::string &op, const std::string &message,
                                    const ProcessingContext &processing, const std::string &time) {
    std::string text = "【" + eventTitle(op) + "】\n时间：" + time + "\n级别：" +
                       (level & 8   ? "错误"
                        : level & 4 ? "关键"
                        : level & 2 ? "重要"
                                    : "例行");
    text += contextText(processing.origin);
    if (op != "censor" && !processing.sources.empty()) {
        text += "\n处理来源：" + readable(processing.label(), 700);
        size_t files = 0;
        for (const auto &source : processing.sources)
            if (!source.file.empty() && files++ < 4)
                text += "\n脚本／资源：" + readable(source.file, 500);
    }
    if (op != "censor" && !processing.trigger.empty())
        text += "\n触发内容：" + triggerText(processing.trigger);
    return text + "\n\n" + readable(message, 10000);
}
struct ErrorReport {
    nlohmann::json record;
    ProcessingContext processing;
    size_t suppressed = 0;
    std::string text() const {
        auto value = [&](const char *key) {
            auto i = record.find(key);
            return i != record.end() && i->is_string() ? i->get<std::string>() : std::string();
        };
        std::string text = "模块：" + readable(value("module"), 160);
        if (!value("timestamp").empty())
            text += "\n错误发生于：" + readable(value("timestamp"), 80);
        text += "\n错误详情：\n" + (value("message").empty()
                                        ? "日志未提供错误正文，请凭关联日志 ID 检查诊断记录。"
                                        : readable(value("message")));
        const auto details = record.value("details", nlohmann::json::object());
        for (const auto &[key, label] :
             std::vector<std::pair<std::string, std::string>>{{"reason", "原因"},
                                                              {"error", "错误"},
                                                              {"exception", "异常"},
                                                              {"cause", "底层原因"},
                                                              {"stderr", "进程错误输出"},
                                                              {"status", "状态"},
                                                              {"code", "错误码"},
                                                              {"http_status", "HTTP 状态码"},
                                                              {"business_code", "业务错误码"},
                                                              {"request_id", "管理请求 ID"},
                                                              {"operation", "操作"},
                                                              {"path", "请求／资源路径"}}) {
            auto it = details.find(key);
            if (it != details.end() && !it->is_null())
                text += "\n" + label + "：" +
                        readable(it->is_string() ? it->get<std::string>() : it->dump(), 1600);
        }
        if (!value("id").empty())
            text += "\n关联日志 ID：" + readable(value("id"), 180);
        if (record.contains("msg_id") && record["msg_id"].is_string())
            text += "\n发送关联 msgID：" + readable(record["msg_id"].get<std::string>(), 180);
        if (suppressed)
            text += "\n相同错误在上次通知后另出现 " + std::to_string(suppressed) +
                    " 次，已合并通知；详细次数和内容可查后台日志。";
        return text;
    }
};
} // namespace dice::notice
