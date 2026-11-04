#pragma once

#include <string>
#include "qq_rich_reply.h"

namespace dice::qq_sn {

inline bool validStyle(const std::string& value) {
    return value == "small_red" || value == "quote" || value == "plain" || value == "custom" || value == "off";
}
inline std::string style(const std::string& value) {
    return validStyle(value) ? value : "small_red";
}

// 「彩色文字」预设（存储键沿用 small_red）的文字颜色，只接受 #RRGGBB。
inline constexpr const char* kDefaultColor = "#E5484D";
inline bool validColor(const std::string& value) {
    if (value.size() != 7 || value[0] != '#') return false;
    for (size_t i = 1; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
inline std::string color(const std::string& value) {
    if (!validColor(value)) return kDefaultColor;
    std::string out = value;
    for (auto& c : out) if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// Text inside QQ's math span must not introduce TeX commands, math delimiters,
// Markdown links, CQ codes or QQ interaction tags. Only our fixed commands are
// emitted; names, templates and character attributes remain literal text.
inline std::string texText(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        switch (c) {
        case '\\': out += "\\textbackslash{}"; break;
        case '{': out += "\\textbraceleft{}"; break;
        case '}': out += "\\textbraceright{}"; break;
        case '$': out += "\\textdollar{}"; break;
        case '%': out += "\\%"; break;
        case '#': out += "\\#"; break;
        case '&': out += "\\&"; break;
        case '_': out += "\\_"; break;
        case '^': out += "\\textasciicircum{}"; break;
        case '~': out += "\\textasciitilde{}"; break;
        case '<': out += "\\textless{}"; break;
        case '>': out += "\\textgreater{}"; break;
        case '`': out += "\\textquoteleft{}"; break;
        case '[': out += "{[}"; break;
        case ']': out += "{]}"; break;
        case '\r': case '\n': case '\t': out += ' '; break;
        default: if (c >= 0x20 && c != 0x7f) out += static_cast<char>(c); break;
        }
    }
    return out;
}

inline bool styled(const std::string& card, const std::string& mode) {
    return !card.empty() && (style(mode) == "small_red" || style(mode) == "custom");
}

inline bool validTemplate(const std::string& value) {
    return value.size() <= 2048 && (value.find("{sn}") != std::string::npos
        || value.find("{sn_tex}") != std::string::npos);
}

// Single pass: placeholder-like text in a player's name is never expanded again.
inline std::string renderTemplate(const std::string& format, const std::string& card) {
    const std::string source = validTemplate(format) ? format : "{sn}";
    std::string out;
    for (size_t i = 0; i < source.size();) {
        if (source.compare(i, 4, "{sn}") == 0) { out += qq_rich::literal(card); i += 4; }
        else if (source.compare(i, 8, "{sn_tex}") == 0) { out += texText(card); i += 8; }
        else out += source[i++];
        // Repeated placeholders can multiply even a short input. Preserve one
        // readable status line instead of sending an oversized decorated prefix.
        if (out.size() > 3000) return qq_rich::literal(card);
    }
    return out;
}

inline std::string prefix(const std::string& card, bool markdown,
                          const std::string& mode = "small_red", int fallbackLevel = 0,
                          const std::string& format = "> {sn}", const std::string& textColor = kDefaultColor) {
    if (card.empty() || style(mode) == "off") return {};
    if (!markdown) return card + "\n";
    if (style(mode) == "plain" || fallbackLevel > 0) return qq_rich::literal(card) + "\n\n";
    if (style(mode) == "quote") return "> " + qq_rich::literal(card) + "\n\n";
    if (style(mode) == "custom") return renderTemplate(format, card) + "\n\n";
    // 颜色已校验为 #RRGGBB，不会引入 TeX 命令或花括号。
    return "$\\scriptsize\\textcolor{" + color(textColor) + "}{\\text{" + texText(card) + "}}$\n\n";
}

} // namespace dice::qq_sn
