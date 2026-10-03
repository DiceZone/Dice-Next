#pragma once

#include "content_format.h"
#include "markdown.h"
#include <initializer_list>
#include <string>

namespace dice {
struct ReplyContent {
    std::string text;
    ContentFormat format = ContentFormat::kPlainText;
};

// Keep legacy plugin replies literal when they share a message with Markdown.
// All-plain messages remain unchanged, including CQ codes and dice operators.
inline ReplyContent joinReplyContent(std::initializer_list<ReplyContent> parts) {
    ReplyContent result;
    for (const auto& part : parts)
        if (!part.text.empty() && part.format == ContentFormat::kMarkdown)
            result.format = ContentFormat::kMarkdown;
    for (const auto& part : parts) {
        if (part.text.empty()) continue;
        if (!result.text.empty()) result.text += '\n';
        result.text += result.format == ContentFormat::kMarkdown && part.format == ContentFormat::kPlainText
            ? markdown::escapeLiteral(part.text) : part.text;
    }
    return result;
}
} // namespace dice
