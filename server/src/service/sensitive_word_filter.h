#pragma once

// Original Dice! compatible command censoring. The legacy matcher was
// case-insensitive and skipped ASCII punctuation and whitespace. Dice!Next
// keeps that observable behaviour and stores owner-defined rules only.

#include "../config/config_manager.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace dice::censor {

enum class Level : int {
    Ignore = 0, Notice = 1, Caution = 2, Warning = 3, Danger = 4,
    Critical = 5,  // legacy highest severity; old built-in lists did not use it
};

struct Rule { std::string word; Level level = Level::Warning; };
struct Settings { bool enabled = false; std::vector<Rule> rules; };
struct Match {
    Level level = Level::Ignore;
    std::vector<std::string> words;
    bool found() const noexcept { return level != Level::Ignore && !words.empty(); }
};

inline int clampLevel(int value) noexcept {
    return (std::max)(0, (std::min)(5, value));
}
inline const char* levelName(Level level) noexcept {
    switch (level) {
        case Level::Ignore: return "Ignore"; case Level::Notice: return "Notice";
        case Level::Caution: return "Caution"; case Level::Warning: return "Warning";
        case Level::Danger: return "Danger"; case Level::Critical: return "Critical";
    }
    return "Warning";
}
inline Level parseLevel(std::string value, Level fallback = Level::Warning) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [](unsigned char c) { return !std::isspace(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [](unsigned char c) { return !std::isspace(c); }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::map<std::string, Level> names{
        {"ignore", Level::Ignore}, {"notice", Level::Notice},
        {"caution", Level::Caution}, {"warning", Level::Warning},
        {"danger", Level::Danger}, {"critical", Level::Critical},
    };
    if (auto it = names.find(value); it != names.end()) return it->second;
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '5')
        return static_cast<Level>(value[0] - '0');
    return fallback;
}

inline bool ignoredCodePoint(char32_t cp) noexcept {
    constexpr std::string_view punctuation = "~!@#$%^&*()-=`_+[]\\{}|;':\",./<>?";
    if (cp < 0x80) {
        const unsigned char c = static_cast<unsigned char>(cp);
        return std::isspace(c) != 0 ||
               punctuation.find(static_cast<char>(c)) != std::string_view::npos;
    }
    return cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}
inline void appendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
inline std::string normalize(std::string_view text) {
    std::string out; out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        char32_t cp = first; size_t width = 1;
        if ((first & 0xE0) == 0xC0 && i + 1 < text.size()) { cp = first & 0x1F; width = 2; }
        else if ((first & 0xF0) == 0xE0 && i + 2 < text.size()) { cp = first & 0x0F; width = 3; }
        else if ((first & 0xF8) == 0xF0 && i + 3 < text.size()) { cp = first & 0x07; width = 4; }
        if (width > 1) {
            bool valid = true;
            for (size_t n = 1; n < width; ++n) {
                const unsigned char next = static_cast<unsigned char>(text[i + n]);
                if ((next & 0xC0) != 0x80) { valid = false; break; }
                cp = (cp << 6) | (next & 0x3F);
            }
            if (!valid) { cp = first; width = 1; }
        }
        i += width;
        if (ignoredCodePoint(cp)) continue;
        if (cp >= 'A' && cp <= 'Z') cp += ('a' - 'A');
        appendUtf8(out, cp);
    }
    return out;
}
inline bool validRuleWord(const std::string& word) {
    const std::string normalized = normalize(word);
    return !normalized.empty() && normalized.size() <= 256 && word.size() <= 512;
}
inline Settings settingsFromJson(const nlohmann::json& value) {
    Settings settings;
    if (!value.is_object()) return settings;
    settings.enabled = value.value("enabled", false);
    const auto words = value.value("words", nlohmann::json::object());
    if (!words.is_object()) return settings;
    settings.rules.reserve((std::min)(words.size(), static_cast<size_t>(5000)));
    for (const auto& [word, levelValue] : words.items()) {
        if (settings.rules.size() >= 5000 || !validRuleWord(word)) continue;
        int raw = static_cast<int>(Level::Warning);
        if (levelValue.is_number_integer()) raw = levelValue.get<int>();
        else if (levelValue.is_string())
            raw = static_cast<int>(parseLevel(levelValue.get<std::string>()));
        settings.rules.push_back({word, static_cast<Level>(clampLevel(raw))});
    }
    return settings;
}
inline Settings load(const ConfigManager& config) {
    return settingsFromJson(
        config.get<nlohmann::json>("dice/censor", nlohmann::json::object()));
}
inline Match search(const Settings& settings, const std::string& text) {
    Match result;
    if (!settings.enabled || settings.rules.empty()) return result;
    const std::string haystack = normalize(text);
    if (haystack.empty()) return result;
    for (const auto& rule : settings.rules) {
        if (rule.level == Level::Ignore) continue;
        const std::string needle = normalize(rule.word);
        if (needle.empty() || haystack.find(needle) == std::string::npos) continue;
        result.words.push_back(rule.word);
        if (static_cast<int>(rule.level) > static_cast<int>(result.level))
            result.level = rule.level;
    }
    return result;
}
class Matcher {
public:
    void replace(const Settings& settings) {
        std::vector<Rule> rules;
        std::vector<Node> nodes(1);
        if (settings.enabled) {
            for (const auto& rule : settings.rules) {
                if (rule.level == Level::Ignore) continue;
                const std::string word = normalize(rule.word);
                if (word.empty()) continue;
                const size_t ruleIndex = rules.size();
                rules.push_back(rule);
                size_t state = 0;
                for (unsigned char byte : word) {
                    auto [it, inserted] = nodes[state].next.emplace(byte, nodes.size());
                    const size_t nextState = it->second;
                    if (inserted) nodes.emplace_back();
                    state = nextState;
                }
                nodes[state].outputs.push_back(ruleIndex);
            }
            std::queue<size_t> pending;
            for (const auto& [byte, state] : nodes[0].next) {
                (void)byte;
                pending.push(state);
            }
            while (!pending.empty()) {
                const size_t parent = pending.front();
                pending.pop();
                for (const auto& [byte, child] : nodes[parent].next) {
                    size_t fallback = nodes[parent].fail;
                    while (fallback && !nodes[fallback].next.count(byte))
                        fallback = nodes[fallback].fail;
                    if (auto it = nodes[fallback].next.find(byte); it != nodes[fallback].next.end())
                        nodes[child].fail = it->second;
                    const auto& inherited = nodes[nodes[child].fail].outputs;
                    nodes[child].outputs.insert(nodes[child].outputs.end(), inherited.begin(), inherited.end());
                    pending.push(child);
                }
            }
        }
        std::unique_lock lock(mutex_);
        enabled_ = settings.enabled;
        rules_ = std::move(rules);
        nodes_ = std::move(nodes);
    }

    Match searchText(const std::string& text) const {
        const std::string normalized = normalize(text);
        std::shared_lock lock(mutex_);
        Match result;
        if (!enabled_ || rules_.empty() || normalized.empty()) return result;
        std::vector<bool> seen(rules_.size(), false);
        size_t state = 0;
        for (unsigned char byte : normalized) {
            while (state && !nodes_[state].next.count(byte)) state = nodes_[state].fail;
            if (auto it = nodes_[state].next.find(byte); it != nodes_[state].next.end())
                state = it->second;
            for (size_t index : nodes_[state].outputs) {
                if (seen[index]) continue;
                seen[index] = true;
                const auto& rule = rules_[index];
                result.words.push_back(rule.word);
                if (static_cast<int>(rule.level) > static_cast<int>(result.level))
                    result.level = rule.level;
            }
        }
        return result;
    }

private:
    struct Node {
        std::unordered_map<unsigned char, size_t> next;
        size_t fail = 0;
        std::vector<size_t> outputs;
    };
    mutable std::shared_mutex mutex_;
    bool enabled_ = false;
    std::vector<Rule> rules_;
    std::vector<Node> nodes_{1};
};

inline bool blocks(Level level, int trust) noexcept {
    if (trust >= 4) return false;
    const int raw = static_cast<int>(level);
    return raw >= static_cast<int>(Level::Caution) && trust <= raw;
}
inline int noticeMask(Level level) noexcept {
    const int raw = static_cast<int>(level);
    if (raw >= static_cast<int>(Level::Critical)) return 8;
    if (raw >= static_cast<int>(Level::Danger)) return 4;
    if (raw >= static_cast<int>(Level::Caution)) return 2;
    return 1;
}

}  // namespace dice::censor
