#pragma once

#include <dicescript/dicescript.h>

#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>

namespace dice::roll_command {

struct ParsedInput {
    std::string expression;
    std::string reason;
};

inline size_t scanLegacyTokenBytes(const std::string& input) {
    static constexpr std::array<std::string_view, 8> extendedTokens = {
        "优势", "優勢", "劣势", "劣勢", "＋", "－", "＊", "／"
    };

    size_t pos = 0;
    while (pos < input.size()) {
        const unsigned char ch = static_cast<unsigned char>(input[pos]);
        const bool asciiToken = std::isalnum(ch) ||
            ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '^' ||
            ch == '%' || ch == '?' || ch == ':' ||
            ch == '<' || ch == '>' || ch == '&' || ch == '|' || ch == '!' ||
            ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',' || ch == '#';
        if (asciiToken) {
            ++pos;
            continue;
        }

        bool matched = false;
        for (const std::string_view token : extendedTokens) {
            if (input.compare(pos, token.size(), token.data(), token.size()) == 0) {
                pos += token.size();
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }
    return pos;
}

/// Find the leading roll expression while leaving a following reason untouched.
///
/// DiceScript deliberately supports Unicode identifiers, so `d测试` is a valid
/// identifier to that language. In a Dice! roll command, however, the historic
/// compact spelling `.rd测试` means "roll the default die, reason: 测试". Resolve
/// that one ambiguity with the legacy token scanner before asking DiceScript for
/// its longest prefix. Known Unicode dice modifiers remain expressions.
inline std::string readDiceToken(const std::string& input) {
    if (input.size() > 1 && (input[0] == 'd' || input[0] == 'D') &&
        static_cast<unsigned char>(input[1]) >= 0x80u) {
        return input.substr(0, scanLegacyTokenBytes(input));
    }

    if (!input.empty()) {
        dicescript_runtime_options options{};
        dicescript_default_runtime_options(&options);
        options.enable_dice_coc = 1;
        options.enable_dice_fate = 1;
        options.enable_dice_wod = 1;
        options.enable_dice_double_cross = 1;
        std::unique_ptr<dicescript_context, decltype(&dicescript_context_destroy)> context(
            dicescript_context_create(&options), &dicescript_context_destroy);
        dicescript_script_result parsed{};
        if (context && dicescript_context_validate_expression_prefix(
                context.get(), input.c_str(), &parsed) &&
            parsed.consumed_bytes != 0) {
            size_t consumed = parsed.consumed_bytes;
            if (consumed > input.size()) consumed = input.size();
            // A dependency regression must never leave a partial UTF-8 code unit
            // in the expression passed to an engine.
            while (consumed > 0 && consumed < input.size() &&
                   (static_cast<unsigned char>(input[consumed]) & 0xc0u) == 0x80u)
                --consumed;
            if (consumed != 0) return input.substr(0, consumed);
        }
    }

    return input.substr(0, scanLegacyTokenBytes(input));
}

inline std::string trimAsciiWhitespace(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() &&
           std::isspace(static_cast<unsigned char>(input[begin])))
        ++begin;
    size_t end = input.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(input[end - 1])))
        --end;
    return input.substr(begin, end - begin);
}

inline bool hasNonDigit(const std::string& input) {
    if (input.empty()) return false;
    for (const char ch : input)
        if (!std::isdigit(static_cast<unsigned char>(ch))) return true;
    return false;
}

/// Split the already-trimmed text after `.r` into expression and reason. This
/// runs before expression-engine selection, so switching modes cannot change the
/// compact-reason interpretation.
inline ParsedInput parse(const std::string& input) {
    // Dice! multi-roll syntax is a command-level prefix, not an expression
    // operator. Parse it before DiceScript can stop at `N#` and misclassify
    // the following dice expression as the reason.
    size_t countEnd = 0;
    while (countEnd < input.size() &&
           std::isdigit(static_cast<unsigned char>(input[countEnd])))
        ++countEnd;
    if (countEnd > 0 && countEnd < input.size() && input[countEnd] == '#') {
        size_t expressionBegin = countEnd + 1;
        while (expressionBegin < input.size() &&
               std::isspace(static_cast<unsigned char>(input[expressionBegin])))
            ++expressionBegin;
        const std::string tail = input.substr(expressionBegin);
        const std::string tailToken = readDiceToken(tail);
        ParsedInput result;
        result.expression = input.substr(0, countEnd + 1);
        if (hasNonDigit(tailToken)) {
            result.expression += tailToken;
            result.reason = trimAsciiWhitespace(tail.substr(tailToken.size()));
        } else {
            result.reason = trimAsciiWhitespace(tail);
        }
        return result;
    }

    const std::string diceToken = readDiceToken(input);
    ParsedInput result;
    if (hasNonDigit(diceToken)) {
        result.expression = diceToken;
        result.reason = trimAsciiWhitespace(input.substr(diceToken.size()));
    } else {
        result.reason = input;
    }
    if (result.expression == "d" || result.expression == "D")
        result.expression.clear();
    return result;
}

}  // namespace dice::roll_command
