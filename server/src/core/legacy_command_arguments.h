#pragma once

#include "command_prefix_policy.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dice::legacy_args {

struct Growth {
    std::string attr;
    std::optional<int> value;
    std::string failure;
    std::string success = "+1d10";
    bool valid = true;
};

inline Growth growth(const std::string& input) {
    Growth out;
    const auto text = trimCommandText(input);
    size_t p = 0;
    while (p < text.size() && !std::isdigit(static_cast<unsigned char>(text[p]))
           && std::string("=:+-*/").find(text[p]) == std::string::npos) ++p;
    out.attr = trimCommandText(text.substr(0, p));
    while (p < text.size() && (std::isspace(static_cast<unsigned char>(text[p])) || text[p] == ':' || text[p] == '=')) ++p;
    const size_t start = p;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) ++p;
    if (p > start) {
        if (p - start > 3) { out.valid = false; return out; }
        out.value = std::stoi(text.substr(start, p - start));
    }
    if (out.attr.empty() && !out.value) { out.valid = false; return out; }
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    if (p < text.size() && (text[p] == '+' || text[p] == '-')) {
        const auto end = text.find_first_of(" \t\r\n", p);
        const auto expr = text.substr(p, end == std::string::npos ? end : end - p);
        const auto slash = expr.find('/');
        if (slash == std::string::npos) out.success = expr;
        else {
            out.failure = expr.substr(0, slash);
            out.success = expr.substr(slash + 1);
            if (out.success.empty() || out.failure.size() == 1 || out.success.find('/') != std::string::npos)
                out.valid = false;
        }
    }
    return out;
}

struct Initiative {
    std::string expression = "1d20";
    std::string name;
    std::string count = "1";
    bool multiple = false;
};

inline std::string initiativeDice(std::string expression) {
    // Both initiative and repetition-count dice default to D20 in Dice!.
    for (size_t i = 0; i < expression.size(); ++i)
        if ((expression[i] == 'd' || expression[i] == 'D')
            && (i + 1 == expression.size() || (expression[i + 1] != '('
                && !std::isdigit(static_cast<unsigned char>(expression[i + 1]))))) {
            expression.insert(i + 1, "20"); i += 2;
        }
    return expression;
}

inline Initiative initiative(const std::string& input) {
    Initiative out;
    std::string rest = trimCommandText(input);
    const auto firstEnd = rest.find_first_of(" \t\r\n");
    const auto first = rest.substr(0, firstEnd);
    // A leading N#name is the count, not the initiative expression.
    if (!first.empty() && first.find('#') == std::string::npos) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(first[0])));
        const bool bareD = c == 'd' && (first.size() == 1 || std::isdigit(static_cast<unsigned char>(first[1]))
            || std::string("+-*/()").find(first[1]) != std::string::npos);
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '(' || bareD) {
            size_t p = 0;
            while (p < rest.size()) {
                const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[p])));
                const std::string tokens = (c == '+' || c == '-') ? "dkpbaf+-x*/()%" : "dkpb+-x*/()%";
                if (!std::isdigit(static_cast<unsigned char>(ch)) && tokens.find(ch) == std::string::npos) break;
                ++p;
            }
            const auto candidate = rest.substr(0, p);
            // An attached numeric name ("42号敌人") is not a fixed initiative.
            const bool numericName = candidate.find_first_not_of("0123456789") == std::string::npos
                && p < rest.size() && !std::isspace(static_cast<unsigned char>(rest[p]));
            if (!numericName) {
                out.expression = initiativeDice((c == '+' || c == '-') ? "1d20" + candidate : candidate);
                rest = trimCommandText(rest.substr(p));
            }
        }
    }
    const auto hash = rest.find('#');
    if (hash != std::string::npos) {
        out.multiple = true;
        const auto count = trimCommandText(rest.substr(0, hash));
        if (!count.empty()) out.count = initiativeDice(count);
        out.name = trimCommandText(rest.substr(hash + 1));
    } else out.name = rest;
    return out;
}

struct GroupTerm { std::string name; bool on; };
inline std::optional<std::vector<GroupTerm>> groupTerms(const std::string& input) {
    std::vector<GroupTerm> terms;
    size_t p = 0;
    while (p < input.size()) {
        while (p < input.size() && std::isspace(static_cast<unsigned char>(input[p]))) ++p;
        if (p == input.size()) break;
        if (input[p] != '+' && input[p] != '-') return std::nullopt;
        const bool on = input[p++] == '+';
        const auto start = p;
        while (p < input.size() && input[p] != '+' && input[p] != '-') ++p;
        auto name = trimCommandText(input.substr(start, p - start));
        if (name.empty()) return std::nullopt;
        terms.push_back({std::move(name), on});
    }
    if (terms.empty()) return std::nullopt;
    return terms;
}
} // namespace dice::legacy_args
