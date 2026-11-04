#pragma once
// 指令共用的运算模块：检定成功率、属性修改、对抗、D20 加值等「数值 + 运算」场景统一调用。
//   + - * / %、^ 或 ** 乘方、括号、NdM / dM 掷骰、属性名（经 lookup 取卡值）、常量 pi / e，
//   min max floor ceil round trunc abs sign sqrt cbrt pow exp ln log log2 log10
//   sin cos tan asin acos atan atan2 hypot（三角函数用弧度）。中间按实数计算，结果向下取整。
// 掷骰先按出现顺序替换成点数，再交给 model_formula 的科学模式求值：只读、无语句、无副作用。
// 防滥用：表达式长度、节点数、嵌套层数、掷骰总数都有上限；溢出、除零、非法定义域一律报错。
#include "model_formula.h"
#include <cctype>
#include <functional>
#include <optional>
#include <string>

namespace dice::arith {

using Lookup = model_formula::Lookup;
/// Returns the total of `count` dice with `faces` sides, or nullopt on failure.
using Roller = std::function<std::optional<long long>(int count, int faces)>;

struct Result {
    std::optional<int> value;      // nullopt: invalid expression or missing attribute
    std::string resolved;          // expression after dice were replaced, e.g. "60+7"
    std::string diceTrace;         // "1d10=7" (comma separated), empty without dice
    std::string missing;           // first attribute the lookup could not resolve
};

inline bool isOperator(char c) { return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^'; }

inline bool identifierByte(unsigned char c) {
    return c >= 128 || std::isalpha(c) || c == '_';
}

/// True when @p text contains arithmetic: an operator after the first character,
/// or a parenthesis. "力量" / "60" / "力量60" are plain; "力量+20-30" / "(a+b)/2" are not.
inline bool hasArithmetic(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' || c == ')') return true;
        if (i > 0 && isOperator(c)) return true;
    }
    return false;
}

/// A free-standing modifier such as "+20", "-30", "*2", "+1d10" or "-(2+3)":
/// starts with an operator and contains only numbers, dice, operators and parentheses.
inline bool isModifierTail(const std::string& text) {
    if (text.size() < 2 || !isOperator(text[0])) return false;
    bool digit = false;
    for (size_t i = 1; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isdigit(c)) { digit = true; continue; }
        if (isOperator(static_cast<char>(c)) || c == '(' || c == ')' || c == 'd' || c == 'D' || c == '.') continue;
        return false;
    }
    return digit;
}

inline constexpr size_t kMaxExpressionBytes = 512;
inline constexpr int kMaxDiceTotal = 1000;     // dice rolled across one expression

/// Replaces NdM / dM tokens (not part of an identifier such as "dex") with rolled totals.
inline std::optional<std::string> rollDice(const std::string& expr, const Roller& roll, std::string* trace) {
    std::string out;
    size_t i = 0;
    int rolls = 0, diceTotal = 0;
    while (i < expr.size()) {
        const unsigned char c = static_cast<unsigned char>(expr[i]);
        if (c != 'd' && c != 'D') { out += expr[i++]; continue; }
        // Digits immediately before 'd' are the dice count (unless they belong to an identifier).
        size_t countStart = out.size();
        while (countStart > 0 && std::isdigit(static_cast<unsigned char>(out[countStart - 1]))) --countStart;
        const bool afterIdentifier = countStart > 0 && identifierByte(static_cast<unsigned char>(out[countStart - 1]));
        size_t facesEnd = i + 1;
        while (facesEnd < expr.size() && std::isdigit(static_cast<unsigned char>(expr[facesEnd]))) ++facesEnd;
        const bool followedByIdentifier = facesEnd < expr.size()
            && identifierByte(static_cast<unsigned char>(expr[facesEnd]));
        if (afterIdentifier || facesEnd == i + 1 || followedByIdentifier) { out += expr[i++]; continue; }
        const std::string countText = out.substr(countStart);
        const std::string facesText = expr.substr(i + 1, facesEnd - i - 1);
        if (countText.size() > 4 || facesText.size() > 7) return std::nullopt;
        const int count = countText.empty() ? 1 : std::stoi(countText);
        const int faces = std::stoi(facesText);
        diceTotal += count;
        if (count < 1 || faces < 1 || diceTotal > kMaxDiceTotal || ++rolls > 32 || !roll) return std::nullopt;
        const auto total = roll(count, faces);
        if (!total) return std::nullopt;
        out.resize(countStart);
        // Parentheses keep a rolled value atomic, e.g. "-2d6" → "-(7)".
        out += "(" + std::to_string(*total) + ")";
        if (trace) {
            if (!trace->empty()) *trace += ", ";
            *trace += std::to_string(count) + "d" + facesText + "=" + std::to_string(*total);
        }
        i = facesEnd;
    }
    return out;
}

/// Evaluates @p expr to an integer. Attribute names go through @p lookup.
inline Result evaluate(const std::string& expr, const Lookup& lookup, const Roller& roll = {}) {
    Result result;
    if (expr.empty() || expr.size() > kMaxExpressionBytes) return result;
    std::string trace;
    const auto rolled = rollDice(expr, roll, &trace);
    if (!rolled) return result;
    result.resolved = *rolled;
    result.diceTrace = trace;
    result.value = model_formula::evaluateScientific(*rolled, [&](const std::string& name) -> std::optional<int> {
        auto value = lookup ? lookup(name) : std::nullopt;
        if (!value && result.missing.empty()) result.missing = name;
        return value;
    });
    if (result.value) result.missing.clear();
    return result;
}

} // namespace dice::arith
