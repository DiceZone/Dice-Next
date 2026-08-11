#pragma once

#include <string>

namespace dice::markdown {

std::string toPlainText(const std::string& markdown);
bool hasFormatting(const std::string& text);

} // namespace dice::markdown
