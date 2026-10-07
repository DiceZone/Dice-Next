#pragma once

#include "sample_template.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dice::safe_template {

// Authored syntax only. No script execution, waits, mutations, file/network
// access, or second parsing of substituted user data. Programs are cached.
enum class Kind { literal, variable, reference, sample, exact, grade };
struct Node {
    Kind kind = Kind::literal;
    std::string text;
    std::string field;
    std::vector<std::string> keys;
    std::vector<std::vector<Node>> branches;
};
struct Program {
    std::vector<Node> nodes;
    std::vector<std::string> issues;
    std::vector<std::string> references;
    std::vector<std::string> selectors;
    std::vector<std::string> variables;
};
struct Budget {
    size_t nodes = 8192;
    size_t bytes = 131072;
};
using Args = std::map<std::string, std::string>;
using Resolve = std::function<std::optional<std::string>(const std::string&, unsigned, Budget&)>;
using Literal = std::function<std::string(std::string_view)>;

inline std::vector<std::string_view> split(std::string_view value, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0, depth = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (sample_template::escaped(value, i)) continue;
        if (value[i] == '{') ++depth;
        else if (value[i] == '}' && depth) --depth;
        else if (value[i] == separator && !depth) { parts.push_back(value.substr(start, i - start)); start = i + 1; }
    }
    parts.push_back(value.substr(start));
    return parts;
}

inline std::optional<double> number(const std::string& value) {
    if (value.empty()) return std::nullopt;
    char* end = nullptr;
    const double result = std::strtod(value.c_str(), &end);
    if (end != value.c_str() + value.size() || !std::isfinite(result)) return std::nullopt;
    return result;
}

inline std::vector<Node> compileNodes(std::string_view value, Program& program, unsigned depth, size_t& count) {
    std::vector<Node> nodes;
    if (depth >= 32 || count >= 4096) {
        program.issues.push_back("template nesting/node limit");
        nodes.push_back(Node{Kind::literal, std::string(value)}); return nodes;
    }
    size_t copied = 0, search = 0;
    while (search < value.size() && count < 4096) {
        size_t open = value.find('{', search);
        if (open == std::string_view::npos) break;
        if (sample_template::escaped(value, open)) { search = open + 1; continue; }
        size_t level = 1, close = open + 1;
        for (; close < value.size(); ++close) {
            if (sample_template::escaped(value, close)) continue;
            if (value[close] == '{') ++level;
            else if (value[close] == '}' && --level == 0) break;
        }
        if (close == value.size()) { program.issues.push_back("unclosed template brace"); break; }
        if (open > copied) nodes.push_back(Node{Kind::literal, std::string(value.substr(copied, open - copied))});
        Node node; node.kind = Kind::variable; node.text = std::string(value.substr(open, close - open + 1));
        const auto body = value.substr(open + 1, close - open - 1);
        node.field = std::string(body);
        const auto colon = body.find(':');
        if (colon == std::string_view::npos) program.variables.push_back(node.field);
        if (colon != std::string_view::npos) {
            const auto method = body.substr(0, colon), parameter = body.substr(colon + 1);
            if (method == "sample") {
                node.kind = Kind::sample;
                for (const auto branch : split(parameter, '|')) node.branches.push_back(compileNodes(branch, program, depth + 1, count));
            } else if (method == "text" && !parameter.empty() && parameter.find_first_of("{}:?&|") == std::string_view::npos) {
                node.kind = Kind::reference; node.field = std::string(parameter); program.references.push_back(node.field);
            } else if (method == "case" || method == "grade") {
                const auto question = parameter.find('?');
                bool valid = question != std::string_view::npos && question > 0;
                if (valid) {
                    node.field = std::string(parameter.substr(0, question));
                    program.selectors.push_back(node.field);
                    for (const auto pair : split(parameter.substr(question + 1), '&')) {
                        const auto equals = pair.find('=');
                        if (equals == std::string_view::npos || equals == 0) { valid = false; break; }
                        const std::string key(pair.substr(0, equals));
                        if (method == "grade" && key != "else" && !number(key)) { valid = false; break; }
                        node.keys.push_back(key);
                        node.branches.push_back(compileNodes(pair.substr(equals + 1), program, depth + 1, count));
                    }
                }
                if (valid && !node.branches.empty()) node.kind = method == "case" ? Kind::exact : Kind::grade;
                else { node.branches.clear(); program.issues.push_back("invalid " + std::string(method) + " macro"); }
            } else {
                program.issues.push_back("unsupported macro: " + std::string(method));
            }
        }
        nodes.push_back(std::move(node)); ++count;
        copied = search = close + 1;
    }
    if (copied < value.size()) nodes.push_back(Node{Kind::literal, std::string(value.substr(copied))});
    return nodes;
}

inline Program compile(std::string_view value) {
    Program result; size_t count = 0;
    result.nodes = compileNodes(value, result, 0, count);
    std::sort(result.issues.begin(), result.issues.end());
    result.issues.erase(std::unique(result.issues.begin(), result.issues.end()), result.issues.end());
    std::sort(result.references.begin(), result.references.end());
    result.references.erase(std::unique(result.references.begin(), result.references.end()), result.references.end());
    return result;
}

// Expand to authored text BEFORE interpolation. Each referenced template is
// expanded under the same budget and only the selected branch is traversed.
inline std::string expand(const std::vector<Node>& nodes, const Args& args, const Resolve& resolve,
                          unsigned depth, Budget& budget, const Literal& literal = {}) {
    std::string result;
    if (depth >= 32) return result;
    auto append = [&](std::string text) {
        // Never cut a UTF-8 sequence when an expansion hits its safety limit.
        if (text.size() > budget.bytes) { budget.bytes = 0; return; }
        budget.bytes -= text.size(); result += text;
    };
    for (const auto& node : nodes) {
        if (!budget.nodes || !budget.bytes) break;
        --budget.nodes;
        if (node.kind == Kind::literal) { append(literal ? literal(node.text) : node.text); continue; }
        if (node.kind == Kind::variable) { append(node.text); continue; }
        if (node.kind == Kind::reference) {
            auto referenced = resolve ? resolve(node.field, depth + 1, budget) : std::nullopt;
            // The recursive expansion already accounts for bytes.
            if (referenced) result += *referenced; else append(node.text);
            continue;
        }
        size_t index = node.branches.size();
        if (node.kind == Kind::sample) index = sample_template::choose(node.branches.size());
        else {
            const auto value = args.find(node.field);
            const auto numeric = value == args.end() ? std::nullopt : number(value->second);
            double highest = -INFINITY;
            for (size_t i = 0; i < node.keys.size(); ++i) {
                if (node.keys[i] == "else") { if (index == node.branches.size()) index = i; continue; }
                if (node.kind == Kind::exact && value != args.end() && value->second == node.keys[i]) { index = i; break; }
                if (node.kind == Kind::grade && numeric) {
                    const auto threshold = number(node.keys[i]);
                    if (threshold && *numeric >= *threshold && *threshold >= highest) { index = i; highest = *threshold; }
                }
            }
        }
        if (index < node.branches.size()) result += expand(node.branches[index], args, resolve, depth + 1, budget, literal);
    }
    return result;
}

} // namespace dice::safe_template
