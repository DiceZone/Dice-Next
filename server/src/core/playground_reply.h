#pragma once
#include "adapter/adapter_interface.h"
#include "common/reply_preview.h"

// 指令测试台没有真实连接：回复按所选平台的出站规则转换，
// 显示的就是该平台用户实际看到的文字。
namespace dice::playground {

// 未配置该平台适配器时，按各适配器 preferredReplyFormat() 的规则推断。
// QQ 官方的 Markdown 还需要已审核的模板，这里无从得知，按纯文本处理。
inline ContentFormat defaultReplyFormat(const std::string& platform, PresentationStyle style) {
    if (style == PresentationStyle::kTraditional) return ContentFormat::kPlainText;
    return platform == "discord" || platform == "kook" ? ContentFormat::kMarkdown : ContentFormat::kPlainText;
}

inline std::string previewPlatform(const std::string& platform, MessageType type) {
    if (platform == "qq_official")
        return type == MessageType::kPrivate ? "qq_private"
             : type == MessageType::kChannel ? "qq_channel" : "qq_group";
    if (platform == "discord" || platform == "kook") return platform;
    return "plain";   // OneBot / Milky 等纯文本平台
}

// plainOnly：该平台此次不会发送富文本（适配器偏好纯文本或会话强制纯文本）。
inline outbound::Json render(const std::string& reply, ContentFormat format, const std::string& platform,
                             MessageType type, PresentationStyle style, bool plainOnly) {
    return outbound::replyPreview(reply, format, previewPlatform(platform, type), style, plainOnly);
}

} // namespace dice::playground
