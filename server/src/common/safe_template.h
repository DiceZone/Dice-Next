#pragma once

#include "sample_template.h"
#include "message_break.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <charconv>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace dice::safe_template {

// Authored syntax only. No script execution, blocking waits, persistent mutations, file/network
// access, or second parsing of substituted user data. Programs are cached.
enum class Kind { literal, variable, reference, sample, exact, grade, formFeed,
                  random, length, expired, assign, mention, identity, wait };
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
    std::vector<std::string> locals;
};
using Args = std::map<std::string, std::string>;
using Read = std::function<std::optional<std::string>(const std::string&)>;
// Typed collection/scalar length, without serializing objects or invoking their
// methods. String-only callers may omit it and retain UTF-8 byte length.
using Length = std::function<std::optional<size_t>(const std::string&)>;
struct Delay {
    unsigned milliseconds = 0;
    void add(int64_t value) {
        // Preserve the original single-wait range; cap a whole reply at ten
        // minutes even when authored branches contain many waits.
        if (value > 0 && value < 600000)
            milliseconds = static_cast<unsigned>((std::min)(int64_t(600000), milliseconds + value));
    }
};
inline thread_local Delay* contextDelay = nullptr;
class DelayScope {
public:
    explicit DelayScope(Delay* delay) : previous_(std::exchange(contextDelay,delay)) {}
    ~DelayScope() { contextDelay = previous_; }
    DelayScope(const DelayScope&) = delete;
    DelayScope& operator=(const DelayScope&) = delete;
private:
    Delay* previous_;
};
// Context callbacks are borrowed only for the current synchronous render; a
// deferred send stores its rendered text, never these stack-bound readers.
inline thread_local Read contextRead;
inline thread_local Length contextLength;
class ContextScope {
public:
    explicit ContextScope(Read read, Length length = {})
        : previous_(std::move(contextRead)), previousLength_(std::move(contextLength)) {
        contextRead = std::move(read); contextLength = std::move(length);
    }
    ~ContextScope() { contextRead = std::move(previous_); contextLength = std::move(previousLength_); }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
private:
    Read previous_;
    Length previousLength_;
};
struct Budget {
    size_t nodes = 8192;
    size_t bytes = 131072;
    Args locals, values;
    Read read;
    Length length;
    bool contextLengthsOverrideArgs = false; // Lua args are copies of this typed message.
    Delay* delay = nullptr;
    std::time_t now = std::time(nullptr);
};
using Resolve = std::function<std::optional<std::string>(const std::string&, unsigned, Budget&)>;
using Literal = std::function<std::string(std::string_view)>;

inline std::vector<std::string_view> split(std::string_view value, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0, depth = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (sample_template::escaped(value, i)) continue;
        if (value[i] == '{') ++depth;
        else if (value[i] == '}' && depth) --depth;
        else if (value[i] == separator && !depth) {
            parts.push_back(value.substr(start, i - start)); start = i + 1;
            if (parts.size() == 4096) { parts.push_back(value.substr(start)); return parts; }
        }
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

inline std::optional<int64_t> integer(std::string_view value) {
    if (value.empty()) return std::nullopt;
    if (value.front() == '+') {
        value.remove_prefix(1);
        if (value.empty() || value.front() == '-' || value.front() == '+') return std::nullopt;
    }
    int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size()
        && !value.empty() ? std::optional<int64_t>(result) : std::nullopt;
}

inline bool localName(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    for (unsigned char c : name)
        if (c <= 32 || c == 127 || std::string_view(".{}:?&|=/\\").find(c) != std::string_view::npos) return false;
    return true;
}

inline bool identityId(std::string_view id) {
    return !id.empty() && id != "0" && id != "all" && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || c == '_' || c == '-';
        });
}

inline std::optional<std::string> read(const std::string& name, const Args& args, const Budget& budget) {
    if (auto it = budget.locals.find(name); it != budget.locals.end()) return it->second;
    if (auto it = args.find(name); it != args.end()) return it->second;
    return budget.read ? budget.read(name) : std::nullopt;
}

// Computed values remain opaque until the caller's final interpolation. This
// prevents locals, nicknames and captures from becoming authored template code,
// while preserving Markdown/code-span escaping in the native renderer.
inline std::string hold(std::string value, Budget& budget) {
    if (value.size() > budget.bytes || budget.values.size() >= 8192) { budget.bytes = 0; return {}; }
    budget.bytes -= value.size();
    const auto key = std::string("\x1f") + "dnx-value-" + std::to_string(budget.values.size());
    budget.values[key] = std::move(value);
    return "{" + key + "}";
}

inline std::string materialize(std::string_view text, const Args& args, const Budget& budget) {
    (void)args; // Only opaque generated slots may be substituted at this stage.
    std::string out;
    for (size_t pos = 0; pos < text.size();) {
        if (text[pos] == '{') {
            const auto end = text.find('}', pos + 1);
            if (end != std::string_view::npos) {
                const std::string key(text.substr(pos + 1, end - pos - 1));
                const auto value = budget.values.find(key);
                if (value != budget.values.end()) out += value->second;
                else out += text.substr(pos, end - pos + 1);
                if (out.size() > 4 * 1024 * 1024) return {};
                pos = end + 1; continue;
            }
        }
        out += text[pos++];
        if (out.size() > 4 * 1024 * 1024) return {};
    }
    return out;
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
        if (colon == std::string_view::npos) {
            if (message_break::isFormFeed(body)) node.kind = Kind::formFeed;
            else program.variables.push_back(node.field);
        }
        if (colon != std::string_view::npos) {
            const auto method = body.substr(0, colon), parameter = body.substr(colon + 1);
            if (method == "sample") {
                const auto branches = split(parameter, '|');
                if (branches.size() > 4096) program.issues.push_back("template branch limit");
                else {
                    node.kind = Kind::sample;
                    for (const auto branch : branches) node.branches.push_back(compileNodes(branch, program, depth + 1, count));
                }
            } else if (method == "text" && !parameter.empty() && parameter.find_first_of("{}:?&|") == std::string_view::npos) {
                node.kind = Kind::reference; node.field = std::string(parameter); program.references.push_back(node.field);
            } else if (method == "ran") {
                const auto bounds = split(parameter, '~');
                if (bounds.size() == 2) {
                    node.kind = Kind::random;
                    for (auto bound : bounds) node.branches.push_back(compileNodes(bound, program, depth + 1, count));
                } else program.issues.push_back("invalid ran macro");
            } else if (method == "len" || method == "expired") {
                node.kind = method == "len" ? Kind::length : Kind::expired;
                node.field = std::string(parameter); program.selectors.push_back(node.field);
            } else if (method == "at") {
                node.kind = Kind::mention;
                node.branches.push_back(compileNodes(parameter, program, depth + 1, count));
            } else if (method == "wait") {
                node.kind = Kind::wait;
                node.branches.push_back(compileNodes(parameter, program, depth + 1, count));
            } else if (method == "print") {
                // The old formatter accepts '&' pairs, with uid/gid/master/self
                // priority independent of the order in the authored text.
                const auto pairs = split(parameter, '&');
                for (const auto field : {"uid", "gid", "master", "self"}) {
                    for (const auto pair : pairs) {
                        const auto equals = pair.find('=');
                        if (pair.substr(0, equals) != field) continue;
                        node.kind = Kind::identity;
                        node.field = std::string(pair);
                        break;
                    }
                    if (node.kind == Kind::identity) break;
                }
            } else if (method == "var") {
                const auto question = parameter.find('?'), equals = parameter.find('=');
                bool valid = true;
                node.field = question < equals ? "print" : "silent";
                const auto assignments = question < equals ? std::vector<std::string_view>{parameter} : split(parameter, '&');
                if (assignments.size() > 4096) valid = false;
                for (auto assignment : assignments) {
                    if (!valid) break;
                    const auto delimiter = assignment.find(question < equals ? '?' : '=');
                    if (delimiter == std::string_view::npos || !localName(assignment.substr(0, delimiter))) { valid = false; break; }
                    node.keys.emplace_back(assignment.substr(0, delimiter));
                    node.branches.push_back(compileNodes(assignment.substr(delimiter + 1), program, depth + 1, count));
                }
                if (valid && !node.keys.empty()) {
                    node.kind = Kind::assign;
                    program.locals.insert(program.locals.end(), node.keys.begin(), node.keys.end());
                } else { node.field = std::string(body); node.branches.clear(); program.issues.push_back("invalid var macro"); }
            } else if (method == "case" || method == "grade") {
                const auto question = parameter.find('?');
                bool valid = question != std::string_view::npos && question > 0;
                if (valid) {
                    node.field = std::string(parameter.substr(0, question));
                    program.selectors.push_back(node.field);
                    const auto pairs = split(parameter.substr(question + 1), '&');
                    if (pairs.size() > 4096) valid = false;
                    for (const auto pair : pairs) {
                        if (!valid) break;
                        const auto equals = pair.find('=');
                        if (equals == std::string_view::npos || equals == 0) { valid = false; break; }
                        const std::string key(pair.substr(0, equals));
                        if (method == "grade" && key != "else" && !number(key)) { valid = false; break; }
                        node.keys.push_back(key);
                        node.branches.push_back(compileNodes(pair.substr(equals + 1), program, depth + 1, count));
                    }
                }
                if (valid && !node.branches.empty()) node.kind = method == "case" ? Kind::exact : Kind::grade;
                else { node.field = std::string(body); node.branches.clear(); program.issues.push_back("invalid " + std::string(method) + " macro"); }
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
                          unsigned depth, Budget& budget, const Literal& literal = {}, const Resolve& named = {}) {
    std::string result;
    if (depth >= 32) return result;
    auto append = [&](std::string text) {
        // Never cut a UTF-8 sequence when an expansion hits its safety limit.
        if (text.size() > budget.bytes) { budget.bytes = 0; return; }
        budget.bytes -= text.size(); result += text;
    };
    const auto fieldValue = [&](const std::string& field) -> std::optional<std::string> {
        if (auto value = read(field, args, budget)) return value;
        if (named) if (auto value = named(field, depth + 1, budget)) return materialize(*value, {}, budget);
        return std::nullopt;
    };
    for (const auto& node : nodes) {
        if (!budget.nodes || !budget.bytes) break;
        --budget.nodes;
        if (node.kind == Kind::literal) { append(literal ? literal(node.text) : node.text); continue; }
        if (node.kind == Kind::formFeed) { append("\f"); continue; }
        if (node.kind == Kind::variable) {
            if (auto value = read(node.field, args, budget)) result += hold(*value, budget);
            else if (named) {
                if (auto referenced = named(node.field, depth + 1, budget)) result += *referenced;
                else append(node.text);
            }
            else append(node.text);
            continue;
        }
        if (node.kind == Kind::reference) {
            auto referenced = resolve ? resolve(node.field, depth + 1, budget) : std::nullopt;
            // The recursive expansion already accounts for bytes.
            if (referenced) result += *referenced; else append(node.text);
            continue;
        }
        if (node.kind == Kind::random || node.kind == Kind::mention || node.kind == Kind::assign || node.kind == Kind::wait) {
            // Evaluate only selected/required arguments. Their data is never
            // fed back to compile(), including when a local is reused.
            auto evaluate = [&](size_t index) {
                return materialize(expand(node.branches[index], args, resolve, depth + 1, budget, {}, named), args, budget);
            };
            if (node.kind == Kind::wait) {
                const auto milliseconds = integer(evaluate(0));
                if (budget.delay && milliseconds) budget.delay->add(*milliseconds);
            } else if (node.kind == Kind::random) {
                const auto low = integer(evaluate(0)), high = integer(evaluate(1));
                if (low && high && *low <= *high) {
                    thread_local std::mt19937_64 random(std::random_device{}());
                    result += hold(std::to_string(std::uniform_int_distribution<int64_t>(*low, *high)(random)), budget);
                }
            } else if (node.kind == Kind::mention) {
                auto target = evaluate(0);
                if (target.empty()) target = read("uid", args, budget).value_or("");
                if (target == "self") target = read("selfId", args, budget).value_or("");
                if (identityId(target)) result += hold("[CQ:at,qq=" + target + "]", budget);
            } else for (size_t i = 0; i < node.keys.size(); ++i) {
                const auto value = evaluate(i);
                budget.locals[node.keys[i]] = value;
                if (node.field == "print") result += hold(value, budget);
            }
            continue;
        }
        if (node.kind == Kind::length || node.kind == Kind::expired) {
            if (node.kind == Kind::length && budget.length && !budget.locals.contains(node.field)
                && (!args.contains(node.field) || budget.contextLengthsOverrideArgs)) {
                if (const auto length = budget.length(node.field)) {
                    result += hold(std::to_string(*length), budget); continue;
                }
            }
            const auto value = fieldValue(node.field);
            if (node.kind == Kind::length) result += hold(std::to_string(value ? value->size() : 0), budget);
            else {
                const auto stamp = value ? integer(*value) : std::nullopt;
                result += hold(!stamp || *stamp < budget.now ? "true" : "false", budget);
            }
            continue;
        }
        if (node.kind == Kind::identity) {
            const auto equals = node.field.find('=');
            const auto field = node.field.substr(0, equals);
            if (field == "uid" || field == "gid" || field == "master" || field == "self") {
                const auto explicitId = equals == std::string::npos ? "" : node.field.substr(equals + 1);
                if (!explicitId.empty() && !identityId(explicitId) && !(field == "gid" && explicitId == "0")) continue;
                const auto display = read("print." + field + (explicitId.empty() ? "" : "=" + explicitId), args, budget);
                result += hold(display.value_or(explicitId.empty()
                    ? read(field == "self" ? "selfId" : field, args, budget).value_or("") : explicitId), budget);
            }
            continue;
        }
        size_t index = node.branches.size();
        if (node.kind == Kind::sample) index = sample_template::choose(node.branches.size());
        else {
            const auto value = fieldValue(node.field);
            const auto numeric = value ? number(*value) : std::nullopt;
            double highest = -INFINITY;
            for (size_t i = 0; i < node.keys.size(); ++i) {
                if (node.keys[i] == "else") { if (index == node.branches.size()) index = i; continue; }
                if (node.kind == Kind::exact && value && *value == node.keys[i]) { index = i; break; }
                if (node.kind == Kind::grade && numeric) {
                    const auto threshold = number(node.keys[i]);
                    if (threshold && *numeric >= *threshold && *threshold >= highest) { index = i; highest = *threshold; }
                }
            }
        }
        if (index < node.branches.size()) result += expand(node.branches[index], args, resolve, depth + 1, budget, literal, named);
    }
    return result;
}

} // namespace dice::safe_template
