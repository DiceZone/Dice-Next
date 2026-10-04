#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dice::tray_settings {
inline constexpr size_t maxCharacters = 10;
inline constexpr bool supported =
#ifdef _WIN32
    true;
#else
    false;
#endif

inline bool whitespace(uint32_t ch) {
    return ch == 0x20 || (ch >= 0x09 && ch <= 0x0d) || ch == 0xa0 || ch == 0x1680 ||
        (ch >= 0x2000 && ch <= 0x200a) || ch == 0x2028 || ch == 0x2029 || ch == 0x202f || ch == 0x205f || ch == 0x3000 || ch == 0xfeff;
}

// Count Unicode scalar values, not UTF-8 bytes (or Windows UTF-16 code units).
inline bool normalize(std::string_view input, std::string& text, std::string& error) {
    struct Character { uint32_t code; size_t begin; size_t end; };
    std::vector<Character> chars;
    for (size_t i = 0; i < input.size();) {
        const size_t begin = i;
        const auto first = static_cast<unsigned char>(input[i++]);
        uint32_t code = first;
        size_t extra = 0;
        if (first < 0x80) {} else if (first >= 0xc2 && first <= 0xdf) { code &= 0x1f; extra = 1; }
        else if (first >= 0xe0 && first <= 0xef) { code &= 0x0f; extra = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { code &= 0x07; extra = 3; }
        else { error = "托盘文字必须是有效的 UTF-8 文本"; return false; }
        for (size_t n = 0; n < extra; ++n) {
            if (i >= input.size() || (static_cast<unsigned char>(input[i]) & 0xc0) != 0x80) {
                error = "托盘文字必须是有效的 UTF-8 文本"; return false;
            }
            code = (code << 6) | (static_cast<unsigned char>(input[i++]) & 0x3f);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            error = "托盘文字必须是有效的 UTF-8 文本"; return false;
        }
        chars.push_back({code, begin, i});
    }
    size_t begin = 0, end = chars.size();
    while (begin < end && whitespace(chars[begin].code)) ++begin;
    while (end > begin && whitespace(chars[end - 1].code)) --end;
    if (end - begin > maxCharacters) { error = "托盘文字最多 10 个字符（不含自动追加的端口）"; return false; }
    for (size_t i = begin; i < end; ++i) {
        if (chars[i].code < 0x20 || (chars[i].code >= 0x7f && chars[i].code <= 0x9f) ||
            chars[i].code == 0x2028 || chars[i].code == 0x2029) {
            error = "托盘文字不能包含换行或控制字符"; return false;
        }
    }
    text = begin == end ? std::string() : std::string(input.substr(chars[begin].begin, chars[end - 1].end - chars[begin].begin));
    error.clear();
    return true;
}

inline std::string tooltip(const std::string& text, uint16_t port) {
    return (text.empty() ? "Dice!Next" : text) + "(" + std::to_string(port) + ")";
}
} // namespace dice::tray_settings
