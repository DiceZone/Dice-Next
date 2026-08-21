#pragma once

#include <string>

namespace dice {

/// Semantic format of an outbound text payload. Plain text is always treated
/// literally; Markdown is rendered natively where supported and downgraded at
/// adapters that only support traditional text.
enum class ContentFormat {
    kPlainText,
    kMarkdown,
};

inline const char* contentFormatName(ContentFormat format) noexcept {
    return format == ContentFormat::kMarkdown ? "markdown" : "plain";
}

inline ContentFormat contentFormatFromString(const std::string& value) noexcept {
    return value == "markdown" ? ContentFormat::kMarkdown : ContentFormat::kPlainText;
}

} // namespace dice
