#pragma once

#include <string>

namespace dice::markdown {

std::string toPlainText(const std::string& markdown);
bool hasFormatting(const std::string& text);
/// Escape arbitrary plain text before embedding it in a Markdown message.
/// OneBot/CQ media codes are preserved verbatim.
std::string escapeLiteral(const std::string& text);

} // namespace dice::markdown
