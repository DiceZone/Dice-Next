#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace dice::shortcut {

inline constexpr size_t kMaxNameBytes = 80;
inline constexpr size_t kMaxCommandBytes = 4096;
inline constexpr size_t kMaxEntries = 100;

inline std::string trim(std::string value) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && space(value.back())) value.pop_back();
    size_t first = 0;
    while (first < value.size() && space(value[first])) ++first;
    return value.substr(first);
}
inline std::pair<std::string, std::string> split(const std::string& value) {
    const auto text = trim(value);
    auto end = text.find_first_of(" \t\r\n");
    if (end == std::string::npos) return {text, {}};
    return {text.substr(0, end), trim(text.substr(end))};
}
// Reserve only the actual invocation syntax, not .ai/.ak/.admin or a plugin's
// longer ASCII command word. The familiar .&name compact spelling is supported.
inline std::optional<std::string> invocation(const std::string& body) {
    if (body.empty()) return std::nullopt;
    if (body[0] == '&') return trim(body.substr(1));
    if ((body[0] == 'a' || body[0] == 'A') &&
        (body.size() == 1 || std::isspace(static_cast<unsigned char>(body[1])) ||
         static_cast<unsigned char>(body[1]) >= 0x80)) return trim(body.substr(1));
    return std::nullopt;
}
struct ManagementArgs { bool personal = false; std::string action, tail; };
inline ManagementArgs management(std::string raw) {
    ManagementArgs out;
    // --my is a management flag, never part of the saved command. Keep other
    // flags, punctuation, quoted arguments and internal spacing untouched.
    std::string cleaned;
    char quote = 0;
    for (size_t i = 0; i < raw.size();) {
        const char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) { cleaned += raw.substr(i, 2); i += 2; continue; }
        if (c == '\'' || c == '"') { if (!quote) quote = c; else if (quote == c) quote = 0; }
        const bool boundary = i == 0 || std::isspace(static_cast<unsigned char>(raw[i - 1]));
        if (!quote && boundary && raw.compare(i, 4, "--my") == 0 &&
            (i + 4 == raw.size() || std::isspace(static_cast<unsigned char>(raw[i + 4])))) {
            out.personal = true; i += 4; continue;
        }
        cleaned += c; ++i;
    }
    auto [action, tail] = split(cleaned);
    out.action = std::move(action); out.tail = std::move(tail);
    return out;
}
inline bool validName(const std::string& name) {
    return !name.empty() && name.size() <= kMaxNameBytes && name.rfind("--", 0) != 0 &&
        name.find_first_of(" \t\r\n") == std::string::npos &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
inline bool validCommand(const std::string& command) {
    return !command.empty() && command.size() <= kMaxCommandBytes &&
        command.find_first_of("\r\n\0", 0, 3) == std::string::npos;
}

} // namespace dice::shortcut
