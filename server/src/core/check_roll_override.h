#pragma once

#include <cctype>
#include <string>

namespace dice {

struct CheckRollOverride {
    bool present = false;
    bool valid = true;
    std::string expression;
    std::string rest;
};

// COC check fixed-outcome preview syntax:
//   .ra(1)50  -> use 1 as the d100 outcome and 50 as the target value.
// Only a leading balanced parenthesized expression is consumed. Nested
// parentheses are preserved so ordinary dice expressions remain available.
inline CheckRollOverride parseCheckRollOverride(const std::string& input) {
    CheckRollOverride out;
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    if (begin >= input.size() || input[begin] != '(') return out;

    out.present = true;
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = begin; i < input.size(); ++i) {
        if (input[i] == '(') ++depth;
        else if (input[i] == ')' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos || depth != 0) {
        out.valid = false;
        return out;
    }

    size_t exprBegin = begin + 1;
    size_t exprEnd = close;
    while (exprBegin < exprEnd && std::isspace(static_cast<unsigned char>(input[exprBegin]))) ++exprBegin;
    while (exprEnd > exprBegin && std::isspace(static_cast<unsigned char>(input[exprEnd - 1]))) --exprEnd;
    if (exprBegin == exprEnd) {
        out.valid = false;
        return out;
    }
    out.expression = input.substr(exprBegin, exprEnd - exprBegin);

    size_t restBegin = close + 1;
    while (restBegin < input.size() && std::isspace(static_cast<unsigned char>(input[restBegin]))) ++restBegin;
    out.rest = input.substr(restBegin);
    return out;
}

} // namespace dice
