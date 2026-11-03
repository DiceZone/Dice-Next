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

// Internal encoding for evaluated action fields. Keep raw command data out of
// Markdown interpolation and prevent substituted text from closing a component.
inline std::string encodeActionField(std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : value) { out += digits[ch >> 4]; out += digits[ch & 15]; }
    return out;
}
inline std::optional<std::string> decodeActionField(std::string_view value) {
    if (value.size() % 2) return std::nullopt;
    const auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0'
        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string out;
    for (size_t i = 0; i < value.size(); i += 2) {
        const int hi = digit(value[i]), lo = digit(value[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

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
        if (!line.empty() && (line.front() == '.' || line.front() == '!'
            || line.front() == '/' || line.rfind("。", 0) == 0)) {
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

// AI can change a caption, but the executable command remains the one captured
// from the template. Never infer replacements (or extra buttons) from its prose.
inline void refreshActionLabels(const std::string& text, std::vector<Action>& actions) {
    std::vector<Action> discovered;
    discoverUsageActions(text, discovered);
    for (auto& action : actions) {
        const auto found = std::find_if(discovered.begin(), discovered.end(), [&](const auto& candidate) {
            return trim(candidate.text) == trim(action.text);
        });
        if (found != discovered.end()) action.label = found->label;
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

// {hp} inside [[bar:...]] is a character-card value, not a command argument.
// A name that appears only there must not be reported as an unknown template variable.
inline bool variableOnlyInBar(const std::string& text, const std::string& name) {
    if (name.empty() || name.find('{') != std::string::npos || name.find('}') != std::string::npos) return false;
    const std::string token = "{" + name + "}";
    bool saw = false;
    for (size_t pos = 0; (pos = text.find(token, pos)) != std::string::npos; ) {
        saw = true;
        const auto open = text.rfind("[[", pos);
        const auto close = text.find("]]", pos);
        if (open == std::string::npos || close == std::string::npos || text.find("]]", open + 2) != close
            || text.compare(open + 2, 4, "bar:") != 0) return false;
        pos += token.size();
    }
    return saw;
}

// Replace a whole bar field that is still `{attribute}` using the caller's card lookup.
// Missing attributes stay visible so a bar is not drawn from an invented maximum.
template <typename Lookup>
inline std::string resolveBarPlaceholders(const std::string& input, Lookup&& lookup) {
    std::string out;
    out.reserve(input.size());
    size_t pos = 0;
    while (pos < input.size()) {
        const size_t open = input.find("[[", pos);
        if (open == std::string::npos) { out.append(input, pos, std::string::npos); break; }
        out.append(input, pos, open - pos);
        const size_t close = input.find("]]", open + 2);
        if (close == std::string::npos) { out.append(input, open, std::string::npos); break; }
        std::string token = input.substr(open + 2, close - open - 2);
        if (token.rfind("bar:", 0) == 0) {
            std::string rebuilt;
            size_t start = 0;
            int field = 0;
            while (true) {
                const size_t bar = token.find('|', start);
                std::string part = token.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (field > 0) {
                    size_t begin = 0, end = part.size();
                    while (begin < end && part[begin] == ' ') ++begin;
                    while (end > begin && part[end - 1] == ' ') --end;
                    if (end > begin + 1 && part[begin] == '{' && part[end - 1] == '}') {
                        std::string name = part.substr(begin + 1, end - begin - 2);
                        size_t nameBegin = 0, nameEnd = name.size();
                        while (nameBegin < nameEnd && name[nameBegin] == ' ') ++nameBegin;
                        while (nameEnd > nameBegin && name[nameEnd - 1] == ' ') --nameEnd;
                        name = name.substr(nameBegin, nameEnd - nameBegin);
                        if (!name.empty() && name.find('{') == std::string::npos && name.find('}') == std::string::npos)
                            if (auto value = lookup(std::string_view(name))) part = std::to_string(*value);
                    }
                }
                if (field) rebuilt += '|';
                rebuilt += part;
                ++field;
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            token = std::move(rebuilt);
        }
        out += "[[";
        out += token;
        out += "]]";
        pos = close + 2;
    }
    return out;
}

// Preview has no character card. The documented example still has to render a bar.
inline std::optional<int> previewBarSample(std::string_view name) {
    std::string key(name);
    for (char& c : key)
        if (static_cast<unsigned char>(c) < 128)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key.find("max") != std::string::npos || name.find("上限") != std::string::npos
        || name.find("最大") != std::string::npos) return 10;
    return 6;
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
        } else if (token.rfind("action:", 0) == 0 || token.rfind("action-data:", 0) == 0) {
            const bool encoded = token.rfind("action-data:", 0) == 0;
            const size_t start = encoded ? 12 : 7;
            const size_t split = token.find('|', start);
            if (split != std::string::npos) {
                auto label = encoded ? decodeActionField(std::string_view(token).substr(start, split - start))
                                     : std::optional<std::string>(token.substr(start, split - start));
                auto command = encoded ? decodeActionField(std::string_view(token).substr(split + 1))
                                       : std::optional<std::string>(token.substr(split + 1));
                if (label && command) {
                    appendAction(result.actions, *label, *command);
                    replacement = action(*label, *command, style, output);
                }
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
    if (style == PresentationStyle::kVisual && result.actions.empty())
        discoverUsageActions(result.text, result.actions);
    return result;
}

} // namespace presentation
} // namespace dice
