#pragma once
// 聊天记录是否写入 chat.db。默认不写：模拟聊天只用内存里的近期消息（GroupChatLog）。
// 骰主在设置里开启「保存聊天记录」（可按平台/账号覆盖），或该群启用了 AI 对话、NPC
// 或记忆（它们要读历史上下文、折叠记忆）时才持久化。

#include "../config/config_manager.h"
#include "../config/scoped_settings.h"
#include "ai_chat.h"
#include "ai_memory.h"
#include "ai_npc.h"

#include <string>

namespace dice::chat_persistence {

inline bool configured(ConfigManager& cfg, const std::string& platform, const std::string& adapterId) {
    const auto settings = scoped_settings::resolveSection(cfg.getAll(), "dice", platform, adapterId);
    const auto it = settings.find("chat_persist");
    return it != settings.end() && it->is_boolean() && it->get<bool>();
}

/// 该群的 AI 功能需要聊天历史。私聊范围（private:<id>）不涉及群 AI 记忆。
template <typename Router>
bool aiNeedsHistory(ConfigManager& cfg, const Router& router, const std::string& platform,
                    const std::string& groupId, const std::string& adapterId) {
    if (groupId.empty() || groupId.rfind("private:", 0) == 0) return false;
    const bool ai = aichat::enabled(cfg) || ainpc::enabled(cfg)
        || aimemory::shortEnabled(cfg) || aimemory::longEnabled(cfg);
    return ai && router.aiEnabledForGroup(platform, groupId, adapterId)
        && router.aiWhitelistOk(platform, groupId, true);
}

template <typename Router>
bool enabledFor(ConfigManager& cfg, const Router& router, const std::string& platform,
                const std::string& adapterId, const std::string& scope) {
    return configured(cfg, platform, adapterId) || aiNeedsHistory(cfg, router, platform, scope, adapterId);
}

} // namespace dice::chat_persistence
