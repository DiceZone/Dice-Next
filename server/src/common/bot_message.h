#pragma once

#include <string>

namespace dice {

inline constexpr const char* kDefaultBotMessage =
    "Dice!Next 测试群：933145116\n"
    "如有故障或建议欢迎加群反馈\n"
    "使用 .help 查看帮助";

// Migration appends the distribution's default notice after the owner's text.
// Keep this out of the runtime renderer: owners may edit or clear their BotMsg.
inline std::string appendDefaultBotMessage(const std::string& original) {
    std::string text;
    for (size_t i = 0; i < original.size(); ++i) {
        if (original[i] == '\r') {
            text += '\n';
            if (i + 1 < original.size() && original[i + 1] == '\n') ++i;
        } else text += original[i];
    }
    const std::string defaults = kDefaultBotMessage;
    size_t pos = 0;
    while ((pos = text.find(defaults, pos)) != std::string::npos) {
        const size_t end = pos + defaults.size();
        if ((pos == 0 || text[pos - 1] == '\n') && (end == text.size() || text[end] == '\n')) {
            text.erase(pos, defaults.size() + (end < text.size() ? 1 : 0));
        } else pos = end;
    }
    const size_t first = text.find_first_not_of('\n');
    if (first == std::string::npos) return defaults;
    text = text.substr(first, text.find_last_not_of('\n') - first + 1);
    return text + "\n" + defaults;
}

} // namespace dice
