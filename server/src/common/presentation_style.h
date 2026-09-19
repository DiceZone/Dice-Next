#pragma once

#include "content_format.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dice {

// Reply wording and transport format are deliberately separate.  A persona
// chooses what the bot says; this value chooses how the same data is laid out.
enum class PresentationStyle : int {
    kTraditional = 0,
    kStandard = 1,
    kVisual = 2,
};

inline PresentationStyle presentationStyleFromString(
    std::string_view value, PresentationStyle fallback = PresentationStyle::kTraditional) {
    if (value == "traditional") return PresentationStyle::kTraditional;
    if (value == "standard" || value == "card") return PresentationStyle::kStandard;
    if (value == "visual") return PresentationStyle::kVisual;
    return fallback;
}

inline const char* presentationStyleName(PresentationStyle style) noexcept {
    switch (style) {
        case PresentationStyle::kStandard: return "standard";
        case PresentationStyle::kVisual: return "visual";
        default: return "traditional";
    }
}

namespace presentation {

// Platform-neutral interaction attached to an outbound reply. Adapters with
// native controls may render it as a command-input link or keyboard button;
// every other adapter keeps the readable command hint in Expanded::text.
struct Action {
    std::string label;
    std::string text;
};

struct Expanded {
    std::string text;
    bool usedMarkdown = false;
    std::vector<Action> actions;
};

inline std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

inline void appendAction(std::vector<Action>& actions, std::string label, std::string command) {
    label = trim(label);
    const bool keepArgumentGap = !command.empty() && (command.back() == ' ' || command.back() == '\t');
    command = trim(command);
    if (keepArgumentGap && !command.empty()) command += ' ';
    if (label.empty() || command.empty()) return;
    if (std::any_of(actions.begin(), actions.end(), [&](const Action& item) {
            return trim(item.text) == trim(command);
        })) return;
    actions.push_back({std::move(label), std::move(command)});
}

inline std::vector<std::pair<std::string, std::string>> expandCommandChoices(
    const std::string& command) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t start = 0;
    while (start < command.size()) {
        const size_t end = command.find_first_of(" \t", start);
        const size_t length = (end == std::string::npos ? command.size() : end) - start;
        const std::string token = command.substr(start, length);
        // Placeholders frequently contain '/' or '|'. They are fields to fill,
        // not choices that should become separate buttons.
        if (token.find_first_of("<[") == std::string::npos) {
            const size_t split = token.find_first_of("/|");
            if (split != std::string::npos && split > 0 && split + 1 < token.size()
                && token.find_first_of("/|", split + 1) == std::string::npos) {
                for (const auto& choice : {token.substr(0, split), token.substr(split + 1)}) {
                    out.push_back({command.substr(0, start) + choice + command.substr(start + length), choice});
                }
                return out;
            }
        }
        if (end == std::string::npos) break;
        start = command.find_first_not_of(" \t", end);
        if (start == std::string::npos) break;
    }
    out.push_back({command, std::string()});
    return out;
}

inline std::string commandPrefill(std::string command) {
    const size_t placeholder = command.find_first_of("<[");
    if (placeholder != std::string::npos) command.erase(placeholder);
    command = trim(command);
    // A trailing blank opens QQ's command input at the argument position. Do
    // not add one after compact syntaxes such as `.ak#<标题>` or `.draw<N>`.
    if (placeholder != std::string::npos && !command.empty()) {
        const unsigned char last = static_cast<unsigned char>(command.back());
        if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z')
            || (last >= '0' && last <= '9')) command += ' ';
    }
    return command;
}

inline void discoverUsageActions(const std::string& text, std::vector<Action>& actions) {
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const size_t lineEnd = text.find('\n', lineStart);
        const std::string line = trim(std::string_view(text).substr(
            lineStart, (lineEnd == std::string::npos ? text.size() : lineEnd) - lineStart));
        // Built-in usage entries use `.command ... // explanation`. Requiring
        // the command prefix and delimiter avoids turning ordinary prose or a
        // URL into an interaction.
        if (!line.empty() && (line.front() == '.' || line.front() == '!')) {
            const size_t delimiter = line.find(" // ");
            if (delimiter != std::string::npos) {
                const std::string commandTemplate = trim(std::string_view(line).substr(0, delimiter));
                const std::string description = trim(std::string_view(line).substr(delimiter + 4));
                for (auto& [choiceCommand, choice] : expandCommandChoices(commandTemplate)) {
                    std::string label = choice.empty() ? description : choice + " · " + description;
                    appendAction(actions, std::move(label), commandPrefill(std::move(choiceCommand)));
                }
            }
        }
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
    }
}

inline std::optional<int> parseInteger(std::string_view value) {
    if (value.empty()) return std::nullopt;
    int out = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) return std::nullopt;
    return out;
}

inline std::string statusBar(std::string_view label, int current, int maximum,
                             PresentationStyle style, ContentFormat output) {
    const std::string value = std::to_string(current) + "/" + std::to_string(maximum);
    if (style != PresentationStyle::kVisual || maximum <= 0) {
        if (style != PresentationStyle::kTraditional && output == ContentFormat::kMarkdown)
            return "**" + std::string(label) + "** " + value;
        return std::string(label) + " " + value;
    }
    const int64_t clamped = std::clamp<int64_t>(current, 0, maximum);
    const int filled = static_cast<int>((clamped * 10 + maximum / 2) / maximum);
    std::string bar = "[";
    for (int i = 0; i < 10; ++i) bar += i < filled ? "█" : "░";
    bar += "]";
    if (output == ContentFormat::kMarkdown)
        return "**" + std::string(label) + "** " + bar + " " + value;
    return std::string(label) + " " + bar + " " + value;
}

inline std::string action(std::string_view label, std::string_view command,
                          PresentationStyle style, ContentFormat output) {
    if (style == PresentationStyle::kVisual)
        return std::string(command) + "  // " + std::string(label);
    if (style == PresentationStyle::kStandard && output == ContentFormat::kMarkdown)
        return "**" + std::string(label) + "**：`" + std::string(command) + "`";
    return std::string(label) + "：" + std::string(command);
}

// Semantic components may be used by built-in, persona and advanced-reply
// templates alike.  Existing {variables} are expanded first, so a template can
// write [[bar:HP|{hp}|{hpmax}]] without teaching every adapter a new syntax.
inline Expanded expandComponents(const std::string& input, PresentationStyle style,
                                 ContentFormat output) {
    Expanded result;
    result.text.reserve(input.size());
    size_t pos = 0;
    while (pos < input.size()) {
        const size_t open = input.find("[[", pos);
        if (open == std::string::npos) { result.text.append(input, pos, std::string::npos); break; }
        result.text.append(input, pos, open - pos);
        const size_t close = input.find("]]", open + 2);
        if (close == std::string::npos) { result.text.append(input, open, std::string::npos); break; }
        const std::string token = input.substr(open + 2, close - open - 2);
        std::string replacement;
        if (token.rfind("bar:", 0) == 0) {
            const size_t first = token.find('|', 4), second = first == std::string::npos
                ? std::string::npos : token.find('|', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                const auto current = parseInteger(std::string_view(token).substr(first + 1, second - first - 1));
                const auto maximum = parseInteger(std::string_view(token).substr(second + 1));
                if (current && maximum)
                    replacement = statusBar(std::string_view(token).substr(4, first - 4),
                                            *current, *maximum, style, output);
            }
        } else if (token.rfind("action:", 0) == 0) {
            const size_t split = token.find('|', 7);
            if (split != std::string::npos) {
                appendAction(result.actions, token.substr(7, split - 7), token.substr(split + 1));
                replacement = action(std::string_view(token).substr(7, split - 7),
                                     std::string_view(token).substr(split + 1), style, output);
            }
        }
        if (replacement.empty()) result.text.append(input, open, close + 2 - open);
        else {
            result.text += replacement;
            result.usedMarkdown = result.usedMarkdown ||
                (output == ContentFormat::kMarkdown && style != PresentationStyle::kTraditional);
        }
        pos = close + 2;
    }
    // Visual-profile usage text already contains stable, translator-editable
    // `.command // explanation` lines. Reuse those lines as semantic actions
    // instead of maintaining a second hard-coded command list.
    if (style == PresentationStyle::kVisual) discoverUsageActions(result.text, result.actions);
    return result;
}

} // namespace presentation
} // namespace dice
