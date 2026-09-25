#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>

namespace dice::deck_files {

/// Deck uploads are deliberately flat: accepting a basename extracted from an
/// arbitrary path would hide traversal attempts instead of rejecting them.
/// Keep UTF-8 names, but reject path syntax, Windows device names and names the
/// loader would not recognize.
inline bool isSafeJsonFilename(std::string_view name) {
    if (name.size() < 6 || name.size() > 240 || name.substr(name.size() - 5) != ".json") return false;
    if (name.front() == '.' || name.back() == '.' || name.back() == ' ') return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return false;
    }

    std::string base(name.substr(0, name.find('.')));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    static const std::unordered_set<std::string> reserved{
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    return !base.empty() && !reserved.contains(base);
}

}  // namespace dice::deck_files
