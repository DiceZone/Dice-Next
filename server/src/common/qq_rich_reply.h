#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dice::qq_rich {

struct Vital {
    std::string label; // Fixed HP/SAN/MP labels, never user-supplied TeX.
    int current = 0;
    std::optional<int> maximum;
};
struct Change { std::string label; std::optional<int> before; int after = 0; };
struct Command { std::string label; std::string text; };
struct Card {
    std::string original; // Exact command reply: logs, AI and fallback keep this text.
    std::optional<std::string> body; // Compact body only when the built-in receipt is fully represented.
    std::string title;
    std::string footer;
    std::string changeLabel;
    std::vector<Vital> vitals;
    std::vector<Change> changes;
    std::vector<Command> commands;
    bool statusOnly = false;
};

inline std::string style(const std::string& value) {
    return value == "markdown" || value == "math" ? value : "off";
}
inline std::string interaction(const std::string& value) {
    return value == "links" || value == "buttons" ? value : "off";
}

// Escape *all* dynamic card text, including QQ tags, CQ codes and math delimiters.
// No user text is ever interpolated into a TeX expression or action attribute.
inline std::string literal(const std::string& input) {
    std::string out;
    for (unsigned char c : input) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '[') out += "&#91;";
        else if (c == ']') out += "&#93;";
        else if (c == '\\') out += "&#92;";
        else if (c == '$') out += "&#36;";
        else {
            if (std::string("*_`#~|").find(static_cast<char>(c)) != std::string::npos) out += '\\';
            if (c >= 0x20 || c == '\n') out += static_cast<char>(c);
        }
    }
    return out;
}
inline std::string urlEncode(const std::string& input) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
inline int filledCells(const Vital& vital) {
    if (!vital.maximum || *vital.maximum <= 0) return 0;
    const auto current = std::clamp<int64_t>(vital.current, 0, *vital.maximum);
    return static_cast<int>(current * 10 / *vital.maximum);
}
inline std::string valueText(const Vital& vital) {
    return std::to_string(vital.current) + (vital.maximum ? "/" + std::to_string(*vital.maximum) : "");
}
inline std::string bar(const Vital& vital, bool math) {
    if (!vital.maximum || *vital.maximum <= 0) return "";
    const int fill = filledCells(vital);
    if (math) {
        const char* color = vital.label == "SAN" ? "#0EA5E9" : vital.label == "MP" ? "#8B5CF6" : "#10B981";
        // Fixed narrow width; calculate from real values instead of rounding to ten cells.
        const auto current = std::clamp<int64_t>(vital.current, 0, *vital.maximum);
        const auto pixels = (current * 100 + *vital.maximum / 2) / *vital.maximum;
        return "$\\textcolor{" + std::string(color) + "}{\\rule{" + std::to_string(pixels)
            + "px}{7px}}\\textcolor{#64748B}{\\rule{" + std::to_string(100 - pixels) + "px}{7px}}$";
    }
    std::string out;
    for (int i = 0; i < 10; ++i) out += i < fill ? "■" : "□";
    return out;
}
inline std::string signedDelta(const Change& change) {
    if (!change.before) return "";
    const int64_t delta = static_cast<int64_t>(change.after) - *change.before;
    return (delta >= 0 ? "+" : "") + std::to_string(delta);
}
inline std::string statusText(const Card& card) {
    std::string out = card.title;
    for (const auto& vital : card.vitals)
        out += "\n" + vital.label + " " + bar(vital, false) + " " + valueText(vital);
    if (!card.footer.empty()) out += "\n" + card.footer;
    return out;
}
inline bool validCommand(const Command& command) {
    return !command.label.empty() && !command.text.empty() && command.label.size() <= 100
        && command.text.size() <= 100 && command.text.find_first_of("\r\n") == std::string::npos
        && command.label.find_first_of("\r\n") == std::string::npos;
}
inline std::string render(const Card& card, bool math, bool links) {
    std::string out = "**" + literal(card.title) + "**\n\n";
    for (const auto& vital : card.vitals)
        out += "**" + literal(vital.label) + "** " + bar(vital, math) + " " + valueText(vital) + "  \n";
    if (!card.changes.empty()) {
        out += "\n**" + literal(card.changeLabel) + "**  \n";
        for (size_t i = 0; i < std::min<size_t>(card.changes.size(), 12); ++i) {
            const auto& change = card.changes[i];
            out += literal(change.label) + ": ";
            if (math && change.before) {
                // Only generated integers enter TeX. Names and arbitrary text stay outside it.
                const auto color = change.after < *change.before ? "#EF4444" : "#10B981";
                out += "$" + std::to_string(*change.before) + "\\xrightarrow{" + signedDelta(change)
                    + "}\\textcolor{" + color + "}{" + std::to_string(change.after) + "}$";
            } else {
                out += (change.before ? std::to_string(*change.before) : "—") + std::string(" → **")
                    + std::to_string(change.after) + "**";
                if (change.before) out += " (" + signedDelta(change) + ")";
            }
            out += "  \n";
        }
    }
    // Preserve explanations, errors, auto-card recalculations and custom replies.
    const auto& detail = card.body ? *card.body : card.original;
    if (!card.statusOnly && !detail.empty()) out += "\n" + literal(detail) + "\n";
    if (!card.footer.empty()) out += "\n" + literal(card.footer) + "\n";
    size_t linkCount = 0;
    if (links) for (size_t i = 0; i < std::min<size_t>(card.commands.size(), 5); ++i) {
        const auto& command = card.commands[i];
        if (validCommand(command)) {
            out += (linkCount++ % 2 == 0 ? "  \n" : " · ");
            out += "<qqbot-cmd-input text=\"" + urlEncode(command.text)
                + "\" show=\"" + urlEncode(command.label) + "\" reference=\"false\" />";
        }
    }
    return out;
}
inline nlohmann::json keyboard(const Card& card, const std::string& nativeUser) {
    using J = nlohmann::json;
    // Never broaden a missing/unknown recipient to permission.type=2 (everyone).
    if (nativeUser.empty()) return J();
    J buttons = J::array();
    for (const auto& command : card.commands) {
        if (buttons.size() == 5) break;
        if (!validCommand(command)) continue;
        buttons.push_back({{"id", "dice-" + std::to_string(buttons.size())},
            {"render_data", {{"label", command.label}, {"visited_label", command.label}, {"style", 0}}},
            {"action", {{"type", 2}, {"data", command.text}, {"enter", false}, {"reply", false},
                {"permission", {{"type", 0}, {"specify_user_ids", J::array({nativeUser})}}},
                {"unsupport_tips", command.text}}}});
    }
    if (buttons.empty()) return J();
    J rows = J::array();
    for (size_t i = 0; i < buttons.size(); i += 2) {
        J pair = J::array({buttons[i]});
        if (i + 1 < buttons.size()) pair.push_back(buttons[i + 1]);
        rows.push_back({{"buttons", pair}});
    }
    return J{{"content", {{"rows", rows}}}};
}
} // namespace dice::qq_rich
