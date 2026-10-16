#pragma once
#include "legacy_conf_value.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dice::legacy_mod {
using Json = nlohmann::json;
enum class LimitScope { Chat, User, Global };
struct CounterLimit { LimitScope scope = LimitScope::Chat; std::string name; long long amount = 0; };
struct IdLimit { std::set<std::string> ids; bool negative = false; };
struct Limits {
    int probability = 0;
    IdLimit users, groups;
    std::map<std::string, Json> userVariables, groupVariables, selfVariables;
    std::vector<CounterLimit> cooldowns, daily, locks;
    std::string cooldownNotice, dailyNotice;
    std::string dicePolicy; // Ignore / Only / Off, resolved by the host, never inferred from user text.
};
inline std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
inline bool scalarNumber(const Json& value, long long& out) {
    if (!value.is_number()) return false;
    const double n = value.get<double>();
    if (!std::isfinite(n) || n < 0 || n > 2147483647.0) return false;
    out = static_cast<long long>(n);
    return true;
}
inline bool parseIds(const Json& value, IdLimit& out) {
    const Json* ids = &value;
    if (value.is_object()) {
        if (value.size() != 1 || !value.contains("nor")) return false;
        out.negative = true; ids = &value.at("nor");
    }
    auto add = [&](const Json& item) {
        if (item.is_string()) { const auto text = item.get<std::string>(); if (text.empty() || text.size() > 256) return false; out.ids.insert(text); return true; }
        if (item.is_number_integer() || item.is_number_unsigned()) { out.ids.insert(item.dump()); return true; }
        return false;
    };
    if (ids->is_array()) {
        if (ids->size() > 10000) return false;
        for (const auto& item : *ids) if (!add(item)) return false;
        return true;
    }
    return add(*ids);
}
inline bool idMatches(const IdLimit& limit, const std::string& id) {
    if (limit.ids.empty()) return true;
    const bool found = limit.ids.count(id.empty() ? "0" : id) != 0;
    return limit.negative ? !found : found;
}
inline bool comparisonKey(const std::string& op) {
    return op == "equal" || op == "neq" || op == "at_least" || op == "at_most" || op == "more" || op == "less";
}
inline bool parseVariables(const Json& value, std::map<std::string, Json>& out) {
    if (!value.is_object() || value.size() > 1000) return false;
    for (const auto& [key, condition] : value.items()) {
        if (key.empty() || key.size() > 256) return false;
        if (condition.is_object()) {
            if (condition.empty()) return false;
            for (const auto& [op, operand] : condition.items()) if (!comparisonKey(op)) return false;
        }
        out[key] = condition;
    }
    return true;
}
inline bool scopeName(const std::string& name, LimitScope& scope) {
    const auto key = lower(name);
    if (key == "chat" || key == "grp" || key == "group") scope = LimitScope::Chat;
    else if (key == "user") scope = LimitScope::User;
    else if (key == "global") scope = LimitScope::Global;
    else return false;
    return true;
}
inline bool parseCounters(const Json& value, std::vector<CounterLimit>& out, std::string& notice) {
    auto add = [&](LimitScope scope, const std::string& name, const Json& amount) {
        if (name.size() > 256) return false;
        long long number = 0;
        const Json& actual = amount.is_array() && !amount.empty() ? amount.front() : amount;
        if (!scalarNumber(actual, number)) return false;
        out.push_back({scope, name, number}); return true;
    };
    if (value.is_number() || value.is_array()) return add(LimitScope::Chat, "", value);
    if (!value.is_object()) return false;
    for (const auto& [key, item] : value.items()) {
        if (key == "echo") { if (!item.is_string() || item.get_ref<const std::string&>().size() > 32768) return false; notice = item.get<std::string>(); continue; }
        // A Lua table can have both its first list entry and named scopes.
        // luaToJson represents that mixed table as an object with key "1".
        if (key == "1") { if (!add(LimitScope::Chat, "", item)) return false; continue; }
        LimitScope scope;
        if (scopeName(key, scope)) {
            if (item.is_object()) { for (const auto& [name, amount] : item.items()) if (!add(scope, name == "1" ? "" : name, amount)) return false; }
            else if (!add(scope, "", item)) return false;
        } else if (!add(LimitScope::Chat, key, item)) return false;
    }
    return out.size() <= 100;
}
inline bool parseLocks(const Json& value, std::vector<CounterLimit>& out) {
    auto add = [&](LimitScope scope, const Json& names) {
        if (names.is_boolean()) { if (names.get<bool>()) out.push_back({scope, "", 1}); return true; }
        if (names.is_string()) { if (names.get_ref<const std::string&>().size() > 256) return false; out.push_back({scope, names.get<std::string>(), 1}); return true; }
        if (names.is_array()) { for (const auto& name : names) { if (!name.is_string() || name.get_ref<const std::string&>().size() > 256) return false; out.push_back({scope, name.get<std::string>(), 1}); } return true; }
        return false;
    };
    if (!value.is_object()) return add(LimitScope::Global, value) && out.size() <= 100;
    for (const auto& [key, names] : value.items()) {
        if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos) {
            if (!add(LimitScope::Global, names)) return false;
            continue;
        }
        LimitScope scope;
        if (!scopeName(key, scope) || !add(scope, names)) return false;
    }
    return out.size() <= 100;
}
inline std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
inline std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> result;
    size_t begin = 0;
    do {
        const auto end = text.find(delimiter, begin);
        result.push_back(trim(text.substr(begin, end == std::string::npos ? end : end - begin)));
        if (end == std::string::npos) break;
        begin = end + 1;
    } while (begin <= text.size());
    return result;
}
inline bool parseStringLimits(const std::string& text, Limits& out, std::string& error) {
    if (text.size() > 65536) { error = "limit 文本过长"; return false; }
    for (const auto& field : split(text, ';')) {
        if (field.empty()) continue;
        const auto colon = field.find(':');
        const auto key = lower(trim(field.substr(0, colon)));
        const auto value = colon == std::string::npos ? std::string() : trim(field.substr(colon + 1));
        bool ok = true;
        if (key == "prob") {
            long long n = 0;
            const auto parsed = Json::parse(value, nullptr, false);
            ok = scalarNumber(parsed, n) && n <= 100;
            if (ok) out.probability = static_cast<int>(n);
        } else if (key == "user_id" || key == "grp_id") {
            auto& ids = key == "user_id" ? out.users : out.groups;
            auto raw = value;
            if (!raw.empty() && raw.front() == '!') { ids.negative = true; raw.erase(0, 1); }
            for (const auto& id : split(raw, ',')) {
                if (id.empty() || id.size() > 256 || ids.ids.size() >= 10000) { ok = false; break; }
                ids.ids.insert(id);
            }
        } else if (key == "user_var" || key == "grp_var" || key == "self_var") {
            auto& vars = key == "user_var" ? out.userVariables : key == "grp_var" ? out.groupVariables : out.selfVariables;
            for (const auto& item : split(value, '&')) {
                const auto equals = item.find('=');
                if (equals == std::string::npos) { ok = false; break; }
                const auto name = trim(item.substr(0, equals));
                auto operand = trim(item.substr(equals + 1));
                if (name.empty() || name.size() > 256 || vars.size() >= 1000) { ok = false; break; }
                std::string op = "equal";
                if (!operand.empty()) {
                    const char suffix = operand.back();
                    if (suffix == '+' || suffix == '-' || suffix == '!') {
                        operand.pop_back();
                        op = suffix == '+' ? "at_least" : suffix == '-' ? "at_most" : "neq";
                    }
                }
                auto parsed = Json::parse(operand, nullptr, false);
                if (parsed.is_discarded()) parsed = operand;
                vars[name] = Json::object({{op, parsed}});
            }
        } else if (key == "cd" || key == "today" || key == "lock") {
            auto& counters = key == "cd" ? out.cooldowns : key == "today" ? out.daily : out.locks;
            if (key == "lock" && colon == std::string::npos) { counters.push_back({LimitScope::Global, "", 1}); continue; }
            for (const auto& item : split(value, '&')) {
                const auto equals = item.find('=');
                const auto label = key == "lock" ? item : equals == std::string::npos ? std::string() : trim(item.substr(0, equals));
                auto name = label;
                auto scope = LimitScope::Chat;
                if (const auto at = label.find('@'); at != std::string::npos) {
                    name = trim(label.substr(0, at));
                    const auto scopeText = trim(label.substr(at + 1));
                    if (scopeText == "echo" && key != "lock" && equals != std::string::npos) {
                        (key == "cd" ? out.cooldownNotice : out.dailyNotice) = item.substr(equals + 1);
                        continue;
                    }
                    if (!scopeName(scopeText, scope)) { ok = false; break; }
                }
                long long amount = 1;
                if (key != "lock") {
                    const auto raw = equals == std::string::npos ? item : trim(item.substr(equals + 1));
                    if (!scalarNumber(Json::parse(raw, nullptr, false), amount)) { ok = false; break; }
                }
                if (name.size() > 256 || counters.size() >= 100) { ok = false; break; }
                counters.push_back({scope, name, amount});
            }
        } else if (key == "dicemaid") {
            out.dicePolicy = lower(value);
            ok = out.dicePolicy == "ignore" || out.dicePolicy == "only" || out.dicePolicy == "off";
        } else ok = false;
        if (!ok) { error = "不支持或无效的限制条件 limit." + key; return false; }
    }
    return true;
}
inline bool parseLimits(const Json& input, Limits& out, std::string& error) {
    if (input.is_null()) return true;
    if (input.is_string()) return parseStringLimits(input.get<std::string>(), out, error);
    if (!input.is_object()) { error = "limit 必须是对象"; return false; }
    for (const auto& [key, value] : input.items()) {
        bool ok = false;
        if (key == "prob") { long long n = 0; ok = scalarNumber(value, n) && n <= 100; if (ok) out.probability = static_cast<int>(n); }
        else if (key == "user_id") ok = parseIds(value, out.users);
        else if (key == "grp_id") ok = parseIds(value, out.groups);
        else if (key == "user_var") ok = parseVariables(value, out.userVariables);
        else if (key == "grp_var") ok = parseVariables(value, out.groupVariables);
        else if (key == "self_var") ok = parseVariables(value, out.selfVariables);
        else if (key == "cd") ok = parseCounters(value, out.cooldowns, out.cooldownNotice);
        else if (key == "today") ok = parseCounters(value, out.daily, out.dailyNotice);
        else if (key == "lock") ok = parseLocks(value, out.locks);
        else if (key == "dicemaid" && value.is_string()) {
            out.dicePolicy = lower(value.get<std::string>());
            ok = out.dicePolicy == "ignore" || out.dicePolicy == "only" || out.dicePolicy == "off";
        }
        if (!ok) { error = "不支持或无效的限制条件 limit." + key; return false; }
    }
    return true;
}
inline Json storedValue(const std::string& raw, bool exists) {
    if (!exists) return nullptr;
    Json typed;
    if (legacy_conf::decode(raw, typed)) return typed;
    auto parsed = Json::parse(raw, nullptr, false);
    if (!parsed.is_discarded() && (parsed.is_number() || parsed.is_boolean())) return parsed;
    return raw;
}
inline bool truthy(const Json& value) { return !value.is_null() && (!value.is_boolean() || value.get<bool>()); }
inline bool compare(const Json& actual, const std::string& op, const Json& expected) {
    bool equal = false;
    if (expected.is_boolean()) equal = truthy(actual) == expected.get<bool>();
    else if (expected.is_string()) equal = actual.is_string() ? actual == expected
        : !actual.is_null() && actual.is_primitive() && actual.dump() == expected.get<std::string>();
    else equal = actual == expected;
    if (op == "equal") return equal;
    if (op == "neq") return !equal;
    if (actual.is_number() && expected.is_number()) {
        const double a = actual.get<double>(), b = expected.get<double>();
        if (!std::isfinite(a) || !std::isfinite(b)) return false;
        if (op == "at_least") return a >= b;
        if (op == "at_most") return a <= b;
        if (op == "more") return a > b;
        if (op == "less") return a < b;
    } else if (actual.is_string() && expected.is_string()) {
        const auto a = actual.get<std::string>(), b = expected.get<std::string>();
        if (op == "at_least") return a >= b;
        if (op == "at_most") return a <= b;
        if (op == "more") return a > b;
        if (op == "less") return a < b;
    }
    return false;
}
inline bool matches(const Json& actual, const Json& condition) {
    if (!condition.is_object()) return compare(actual, "equal", condition);
    for (const auto& [op, expected] : condition.items()) if (!compare(actual, op, expected)) return false;
    return true;
}
} // namespace dice::legacy_mod
