#pragma once
// A bounded, read-only numeric language. This is NOT a JavaScript interpreter.
// No statements, assignments, objects, indexing, dynamic calls, I/O or eval.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dice::model_formula {
using Value = std::optional<double>;
using Lookup = std::function<std::optional<int>(const std::string&)>;
struct Node { std::string op, name; double number = 0; std::vector<size_t> args; };
struct Program { std::vector<Node> nodes; size_t root = 0; bool valid = false; };

class Parser {
    std::string_view text_; size_t at_ = 0, depth_ = 0; bool valid_ = true;
    Program program_;
    void spaces() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_; }
    bool take(std::string_view token) {
        spaces(); if (text_.substr(at_, token.size()) != token) return false;
        at_ += token.size(); return true;
    }
    size_t add(Node node) {
        if (program_.nodes.size() >= 1024) { valid_ = false; return 0; }
        program_.nodes.push_back(std::move(node)); return program_.nodes.size() - 1;
    }
    static bool identifier(unsigned char c) { return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; }
    size_t primary() {
        spaces();
        if (++depth_ > 48 || at_ >= text_.size()) { valid_ = false; --depth_; return 0; }
        size_t result = 0;
        if (take("(")) { result = conditional(); if (!take(")")) valid_ = false; }
        else if (take("+")) result = add({"positive", "", 0, {primary()}});
        else if (take("-")) result = add({"negative", "", 0, {primary()}});
        else if (take("!")) result = add({"not", "", 0, {primary()}});
        else if (text_[at_] >= '0' && text_[at_] <= '9') {
            const std::string remainder(text_.substr(at_)); char* end = nullptr;
            const double number = std::strtod(remainder.c_str(), &end);
            if (!std::isfinite(number) || end == remainder.c_str()) valid_ = false;
            at_ += static_cast<size_t>(end - remainder.c_str()); result = add({"number", "", number, {}});
        } else {
            const auto start = at_;
            if (at_ < text_.size() && text_[at_] == '&' && text_.substr(at_, 2) != "&&") ++at_;
            const auto nameStart = at_;
            while (at_ < text_.size() && identifier(static_cast<unsigned char>(text_[at_]))) ++at_;
            auto name = std::string(text_.substr(nameStart, at_ - nameStart));
            if (name.empty()) { valid_ = false; if (at_ == start && at_ < text_.size()) ++at_; }
            if (take("(")) {
                if (name.rfind("Math.", 0) == 0) name.erase(0, 5);
                static const std::vector<std::string> functions = {"floor", "ceil", "round", "trunc", "abs", "min", "max", "pow", "sqrt", "sign"};
                if (std::find(functions.begin(), functions.end(), name) == functions.end()) valid_ = false;
                std::vector<size_t> args;
                if (!take(")")) {
                    do { args.push_back(conditional()); if (args.size() > 32) { valid_ = false; break; } } while (take(","));
                    if (!take(")")) valid_ = false;
                }
                if ((name == "min" || name == "max") ? args.empty() : args.size() != (name == "pow" ? 2 : 1)) valid_ = false;
                result = add({"call", name, 0, std::move(args)});
            } else {
                if (name.rfind("this.", 0) == 0) name.erase(0, 5);
                else if (name.rfind("pc.", 0) == 0) name.erase(0, 3);
                if (name.empty() || name.find('.') != std::string::npos) valid_ = false;
                result = add({"attribute", name, 0, {}});
                // Existing Next DSL: attr?N means a missing-attribute default.
                // A real ternary has a colon and is parsed by conditional().
                spaces();
                if (at_ < text_.size() && text_[at_] == '?' && text_.find(':', at_) == std::string_view::npos) {
                    ++at_; const auto fallback = primary(); result = add({"fallback", "", 0, {result, fallback}});
                }
            }
        }
        --depth_; return result;
    }
    size_t product() {
        auto left = primary();
        while (valid_) {
            std::string op;
            if (take("*")) op = "*"; else if (take("/")) op = "/"; else if (take("%")) op = "%"; else break;
            left = add({op, "", 0, {left, primary()}});
        }
        return left;
    }
    size_t sum() {
        auto left = product();
        while (valid_) {
            std::string op;
            if (take("+")) op = "+"; else if (take("-")) op = "-"; else break;
            left = add({op, "", 0, {left, product()}});
        }
        return left;
    }
    size_t relational() {
        auto left = sum();
        while (valid_) {
            std::string op;
            for (const auto* candidate : {"<=", ">=", "<", ">"}) if (take(candidate)) { op = candidate; break; }
            if (op.empty()) break;
            left = add({op, "", 0, {left, sum()}});
        }
        return left;
    }
    size_t comparison() {
        auto left = relational();
        while (valid_) {
            std::string op;
            if (take("==")) op = "=="; else if (take("!=")) op = "!="; else break;
            left = add({op, "", 0, {left, relational()}});
        }
        return left;
    }
    size_t andExpression() { auto left = comparison(); while (valid_ && take("&&")) left = add({"&&", "", 0, {left, comparison()}}); return left; }
    size_t orExpression() { auto left = andExpression(); while (valid_ && take("||")) left = add({"||", "", 0, {left, andExpression()}}); return left; }
    size_t conditional() {
        if (++depth_ > 48) { valid_ = false; --depth_; return 0; }
        auto result = orExpression();
        if (take("?")) {
            const auto yes = conditional(); if (!take(":")) valid_ = false;
            const auto no = valid_ ? conditional() : 0;
            result = add({"?:", "", 0, {result, yes, no}});
        }
        --depth_; return result;
    }
public:
    explicit Parser(std::string_view text) : text_(text) {}
    Program parse() {
        if (text_.empty() || text_.size() > 8192) return {};
        program_.root = conditional(); spaces(); program_.valid = valid_ && at_ == text_.size(); return std::move(program_);
    }
};

inline Value finite(double number) { return std::isfinite(number) && std::abs(number) <= 9007199254740991.0 ? Value(number) : std::nullopt; }
inline Value evaluateNode(const Program& program, size_t index, const Lookup& lookup, bool floating, unsigned depth = 0) {
    if (depth > 64 || index >= program.nodes.size()) return std::nullopt;
    const auto& node = program.nodes[index];
    auto arg = [&](size_t n, bool real) { return n < node.args.size() ? evaluateNode(program, node.args[n], lookup, real, depth + 1) : std::nullopt; };
    if (node.op == "number") return finite(node.number);
    if (node.op == "attribute") { const auto value = lookup ? lookup(node.name) : std::nullopt; return value ? Value(*value) : std::nullopt; }
    const auto a = arg(0, node.op == "call" || floating);
    if (node.op == "fallback") return a ? a : arg(1, floating);
    if (node.op == "||") return a && *a != 0 ? a : arg(1, floating);
    if (node.op == "&&") return a && *a != 0 ? arg(1, floating) : a;
    if (node.op == "?:") return arg(a && *a != 0 ? 1 : 2, floating);
    if (!a) return std::nullopt;
    if (node.op == "positive") return a;
    if (node.op == "negative") return finite(-*a);
    if (node.op == "not") return *a == 0 ? 1 : 0;
    if (node.op == "call") {
        std::vector<double> values;
        for (size_t n = 0; n < node.args.size(); ++n) { const auto value = n == 0 ? a : arg(n, true); if (!value) return std::nullopt; values.push_back(*value); }
        if (node.name == "floor") return finite(std::floor(values[0]));
        if (node.name == "ceil") return finite(std::ceil(values[0]));
        if (node.name == "round") return finite(std::floor(values[0] + 0.5));
        if (node.name == "trunc") return finite(std::trunc(values[0]));
        if (node.name == "abs") return finite(std::abs(values[0]));
        if (node.name == "sign") return values[0] == 0 ? 0 : values[0] > 0 ? 1 : -1;
        if (node.name == "sqrt") return values[0] < 0 ? std::nullopt : finite(std::sqrt(values[0]));
        if (node.name == "pow") return finite(std::pow(values[0], values[1]));
        if (node.name == "min") return *std::min_element(values.begin(), values.end());
        if (node.name == "max") return *std::max_element(values.begin(), values.end());
        return std::nullopt;
    }
    const auto b = arg(1, floating); if (!b) return std::nullopt;
    if (node.op == "+") return finite(*a + *b);
    if (node.op == "-") return finite(*a - *b);
    if (node.op == "*") return finite(*a * *b);
    if (node.op == "/") return *b == 0 ? std::nullopt : finite(floating ? *a / *b : std::trunc(*a / *b));
    if (node.op == "%") return *b == 0 ? std::nullopt : finite(std::fmod(*a, *b));
    if (node.op == "<") return *a < *b ? 1 : 0;
    if (node.op == ">") return *a > *b ? 1 : 0;
    if (node.op == "<=") return *a <= *b ? 1 : 0;
    if (node.op == ">=") return *a >= *b ? 1 : 0;
    if (node.op == "==") return *a == *b ? 1 : 0;
    if (node.op == "!=") return *a != *b ? 1 : 0;
    return std::nullopt;
}
inline bool supported(const std::string& text) { return Parser(text).parse().valid; }
inline std::optional<int> evaluate(const std::string& text, const Lookup& lookup) {
    const auto program = Parser(text).parse(); if (!program.valid) return std::nullopt;
    const auto value = evaluateNode(program, program.root, lookup, false);
    if (!value || *value < (std::numeric_limits<int>::min)() || *value > (std::numeric_limits<int>::max)()) return std::nullopt;
    return static_cast<int>(*value);
}
} // namespace dice::model_formula
