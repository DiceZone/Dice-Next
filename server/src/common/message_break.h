#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dice::message_break {

// Original Dice!'s case-insensitive global speech {FormFeed} emits U+000C.
// Resolve only authored template tokens, never reparse interpolated user data.
inline bool isFormFeed(std::string_view name) {
    constexpr std::string_view expected = "formfeed";
    if (name.size() != expected.size()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c != expected[i]) return false;
    }
    return true;
}

// Explicit page boundaries are separate from optional automatic length-based
// splitting. Discard empty pages so leading/trailing/repeated separators do not
// submit empty messages to platform APIs. Preserve all other text byte-for-byte.
inline std::vector<std::string> split(const std::string& text) {
    if (text.find('\f') == std::string::npos) return {text};
    std::vector<std::string> pages;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find('\f', begin);
        const auto page = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (page.find_first_not_of(" \t\r\n\v") != std::string::npos) pages.push_back(page);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return pages;
}

} // namespace dice::message_break
