#pragma once

#include <dicescript/dicescript.h>

#include <array>
#include <cctype>
#include <limits>
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
inline std::string readDiceToken(const std::string& input, bool compact = false) {
    if (input.size() > 1 && (input[0] == 'd' || input[0] == 'D') &&
        static_cast<unsigned char>(input[1]) >= 0x80u) {
        return input.substr(0, scanLegacyTokenBytes(input));
    }
    // In the compact command spelling, ASCII text immediately after a bare d is
    // the reason (`.rdtest` = default die, reason "test"), not a DiceScript
    // identifier named dtest. Fate's dF token remains an expression.
    if (compact && input.size() > 1 && (input[0] == 'd' || input[0] == 'D') &&
        std::isalpha(static_cast<unsigned char>(input[1])) &&
        std::tolower(static_cast<unsigned char>(input[1])) != 'f') {
        return input.substr(0, 1);
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

/// DiceScript's detail omits the final total for composite expressions. Keep a
/// simple die trace concise (`2[2d1=1+1]` already starts with its result), but
/// append the numeric value to a composite trace (`2[2d1=1+1]+3=5`).
inline std::string renderNumericDetail(const std::string& value,
                                       const std::string& rawDetail) {
    const std::string detail = rawDetail.empty() ? value : rawDetail;
    if (detail.empty() || value.empty() || detail == value) return detail;
    bool valueWithAnnotations = detail.rfind(value + "[", 0) == 0;
    int depth = 0;
    if (valueWithAnnotations) {
        for (size_t i = value.size(); i < detail.size(); ++i) {
            if (detail[i] == '[') ++depth;
            else if (detail[i] == ']') {
                if (--depth < 0) { valueWithAnnotations = false; break; }
            } else if (depth == 0 &&
                       !std::isspace(static_cast<unsigned char>(detail[i]))) {
                valueWithAnnotations = false;
                break;
            }
        }
        if (depth != 0) valueWithAnnotations = false;
    }
    return valueWithAnnotations ? detail : detail + "=" + value;
}

inline bool isNumericResult(dicescript_value_type type) {
    return type == DICESCRIPT_VALUE_INT || type == DICESCRIPT_VALUE_FLOAT;
}

/// Split the compact leading syntax used by .ww/.dx from an optional reason.
/// Named parts (a/c/+) are consumed only when followed by a number, so a reason
/// such as "attack" is not mistaken for an `a` modifier. Double Cross also
/// accepts a negative final modifier; keep that opt-in so `.ww` semantics do not
/// change (`+N` there means extra successes, not arithmetic on the final value).
inline ParsedInput parsePoolInput(const std::string& input,
                                  bool allowNegativeModifier = false) {
    ParsedInput result;
    size_t pos = 0;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
    const size_t poolBegin = pos;
    while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) ++pos;
    if (pos == poolBegin) {
        result.reason = trimAsciiWhitespace(input);
        return result;
    }
    result.expression = input.substr(poolBegin, pos - poolBegin);
    bool hasNamedPart = false;
    bool hasLegacyLine = false;
    while (pos < input.size()) {
        const size_t checkpoint = pos;
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
        if (pos >= input.size()) break;

        const unsigned char current = static_cast<unsigned char>(input[pos]);
        const char part = static_cast<char>(std::tolower(current));
        if (part == 'a' || part == 'c' || part == '+' ||
            (allowNegativeModifier && part == '-')) {
            size_t number = pos + 1;
            while (number < input.size() && std::isspace(static_cast<unsigned char>(input[number]))) ++number;
            const size_t digits = number;
            while (number < input.size() && std::isdigit(static_cast<unsigned char>(input[number]))) ++number;
            if (number == digits) { pos = checkpoint; break; }
            result.expression.push_back(part);
            result.expression.append(input, digits, number - digits);
            hasNamedPart = true;
            pos = number;
            continue;
        }

        // Legacy `.ww N successLine` / `.dx N criticalLine` requires whitespace
        // before the second number and cannot be combined with named parts.
        if (!hasNamedPart && !hasLegacyLine && pos > checkpoint && std::isdigit(current)) {
            const size_t digits = pos;
            while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) ++pos;
            result.expression.push_back(' ');
            result.expression.append(input, digits, pos - digits);
            hasLegacyLine = true;
            continue;
        }
        pos = checkpoint;
        break;
    }
    result.reason = trimAsciiWhitespace(input.substr(pos));
    return result;
}

struct DoubleCrossSpec {
    bool valid = false;
    int pool = 0;
    int critical = 10;
    int modifier = 0;
    bool hasModifier = false;
};

inline bool parseUnsignedInt(std::string_view token, int& value) {
    if (token.empty()) return false;
    int parsed = 0;
    for (const char ch : token) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        const int digit = ch - '0';
        if (parsed > ((std::numeric_limits<int>::max)() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

/// Parse the canonical expression returned by parsePoolInput for `.dx/.rdx`.
/// The optional trailing +/-N is a fixed adjustment to the final Double Cross
/// value, not part of the critical line. Reject malformed or repeated modifiers
/// instead of silently accepting their numeric prefix through std::stoi.
inline DoubleCrossSpec parseDoubleCrossSpec(const std::string& expression) {
    DoubleCrossSpec result;
    std::string base = trimAsciiWhitespace(expression);
    if (base.empty()) return result;

    const size_t modifierPos = base.find_first_of("+-", 1);
    if (modifierPos != std::string::npos) {
        const std::string modifierToken = base.substr(modifierPos);
        int magnitude = 0;
        if (modifierToken.size() < 2 ||
            !parseUnsignedInt(std::string_view(modifierToken).substr(1), magnitude))
            return result;
        result.hasModifier = true;
        result.modifier = modifierToken.front() == '-' ? -magnitude : magnitude;
        base = trimAsciiWhitespace(base.substr(0, modifierPos));
    }

    const size_t criticalPos = base.find_first_of("cC");
    if (criticalPos != std::string::npos) {
        if (base.find_first_of("cC", criticalPos + 1) != std::string::npos) return result;
        const std::string poolToken = trimAsciiWhitespace(base.substr(0, criticalPos));
        const std::string criticalToken = trimAsciiWhitespace(base.substr(criticalPos + 1));
        if (!parseUnsignedInt(poolToken, result.pool) ||
            !parseUnsignedInt(criticalToken, result.critical))
            return result;
    } else {
        size_t split = 0;
        while (split < base.size() &&
               !std::isspace(static_cast<unsigned char>(base[split])))
            ++split;
        const std::string poolToken = base.substr(0, split);
        if (!parseUnsignedInt(poolToken, result.pool)) return result;
        if (split < base.size()) {
            const std::string criticalToken = trimAsciiWhitespace(base.substr(split));
            if (!parseUnsignedInt(criticalToken, result.critical)) return result;
        }
    }

    result.valid = result.pool > 0;
    return result;
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
inline ParsedInput parse(const std::string& input, bool compact = true) {
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
        const std::string tailToken = readDiceToken(tail, true);
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

    const std::string diceToken = readDiceToken(input, compact);
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
