#pragma once

#include <cctype>
#include <optional>
#include <sstream>
#include <string>

namespace dice {

struct BotControlCommand {
    std::string action;
    std::string target;
};

inline std::string trimCommandText(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

inline std::string lowerCommandText(std::string value) {
    for (auto& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

inline bool commandTokenIsDigits(const std::string& value) {
    if (value.empty()) return false;
    for (const char ch : value)
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    return true;
}

// Parse exactly the forms accepted by the built-in bot control command:
// bot, boton/botoff, bot on/off, plus one optional numeric account target.
inline std::optional<BotControlCommand> parseBotControlCommand(const std::string& body) {
    const std::string normalized = trimCommandText(body);
    const std::string lowered = lowerCommandText(normalized);
    if (lowered.rfind("bot", 0) != 0) return std::nullopt;

    BotControlCommand result;
    std::istringstream input(trimCommandText(normalized.substr(3)));
    std::string token;
    while (input >> token) {
        const std::string loweredToken = lowerCommandText(token);
        if (loweredToken == "on" || loweredToken == "off") {
            if (!result.action.empty()) return std::nullopt;
            result.action = loweredToken;
        } else if (commandTokenIsDigits(token)) {
            if (!result.target.empty()) return std::nullopt;
            result.target = token;
        } else {
            return std::nullopt;
        }
    }
    return result;
}

// `.` and `。` remain emergency prefixes for only bot controls and dismiss,
// even when an owner removes them from the configurable prefix list.
inline std::optional<std::string> forcedSafetyCommandBody(const std::string& text) {
    const std::string normalized = trimCommandText(text);
    std::size_t prefixLength = 0;
    if (normalized.rfind(".", 0) == 0) prefixLength = 1;
    else if (normalized.rfind("\xE3\x80\x82", 0) == 0) prefixLength = 3;  // 。
    else return std::nullopt;

    const std::string body = trimCommandText(normalized.substr(prefixLength));
    if (lowerCommandText(body) == "dismiss" || parseBotControlCommand(body)) return body;
    return std::nullopt;
}

}  // namespace dice
