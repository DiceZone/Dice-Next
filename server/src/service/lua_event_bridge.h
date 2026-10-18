#pragma once
#include "../core/command_router.h"

namespace dice {
// The host owns the Lua virtual-event scope. This shared dispatcher is also
// exercised offline with fake adapters; no separate test-only routing logic.
template<class Fallback, class PlatformResolver>
void dispatchLegacyLuaEvent(AdapterManager& adapters, CommandRouter& router, const LuaEventOrigin& origin,
        const std::string& text, const std::string& gid, const std::string& uid,
        Fallback&& fallback, PlatformResolver&& resolvePlatform) {
    static thread_local int depth = 0;
    if (depth >= 4) { DICE_LOG_WARN("Lua eventMsg 嵌套已达上限，本次调用已忽略。"); return; }
    struct Guard { int& value; Guard(int& v) : value(v) { ++value; } ~Guard() { --value; } } guard(depth);
    std::string platform = origin.platform.empty() ? resolvePlatform(gid, uid) : origin.platform;
    AdapterPtr selected;
    for (const auto& adapter : adapters.allAdapters()) {
        if (!adapter || !adapter->isConnected() || (!origin.adapterId.empty() && adapter->id() != origin.adapterId)
            || (!platform.empty() && adapter->platform() != platform)) continue;
        selected = adapter; platform = adapter->platform(); break;
    }
    if (!selected) { DICE_LOG_WARN("Lua eventMsg 的来源账号不可用，本次调用已忽略。"); return; }
    if (router.handleLegacyLuaNoticeEvent(text, platform, selected->id())) return;
    const bool pv = gid.empty() || gid == "0";
    const auto self = selected->getLoginId();
    const bool selfCall = pv && !self.empty() && self != "0" && uid == self;
    if (selfCall) if (const auto ban = parseLegacyLuaGroupBan(text)) {
        const auto error = router.legacyLuaSelfBan(*ban, origin, selected);
        if (!error.empty()) DICE_LOG_WARN("Lua 插件「{}」的旧版禁言未执行：{}", origin.pluginId, error);
        else DICE_LOG_INFO("Lua 插件「{}」已提交自禁言请求：群 {}，用户 {}，{} 分钟。", origin.pluginId, ban->groupId, ban->userId, ban->minutes);
        return;
    }
    const auto& target = pv ? uid : gid;
    if (target.empty() || target == "0" || ((platform == "onebot_v11" || platform == "milky") && !positiveQQId(target))) {
        DICE_LOG_WARN("Lua 插件「{}」的 eventMsg 目标无效，未向平台发送消息。", origin.pluginId); return;
    }
    Message message;
    message.platform = platform; message.adapterId = selected->id(); message.selfId = self;
    message.senderId = uid; message.senderName = router.lookupNick(platform, uid);
    message.type = pv ? MessageType::kPrivate : MessageType::kGroup;
    message.targetId = target; message.content = text; message.rawContent = text; message.displayContent = text;
    if (!pv && (router.isBlocked(message) || router.isGroupDisabled(message) || !router.groupFeatureEnabled(message, "plugin"))) return;
    std::string reply = router.handleMessage(message);
    auto sources = router.lastProcessingSources();
    if (!reply.empty() && sources.empty()) sources = {{"内置指令", text.substr(0, text.find_first_of(" \t")), "", ""}};
    if (reply.empty() && !router.isGroupDisabled(message)) { std::string source; reply = fallback(message, source, &sources); }
    if (reply.empty()) return;
    // Original Dice! executes virtual private self-calls without sending a receipt.
    // Do not set fromSelf or grant administrative rights to their other commands.
    if (selfCall) { DICE_LOG_DEBUG("Lua 虚拟私聊自调用已处理，回执不向平台发送：{}", reply); return; }
    reply = router.applySelf(message, reply);
    ProcessingScope processing(ProcessingContext{sources, activeProcessingContext.label() + "调用 eventMsg：" + text, "虚拟指令处理完成"});
    if (pv) selected->sendPrivateMessage(uid, reply);
    else selected->sendGroupMessageCQ(gid, reply);
}
}
