#pragma once

#include "sample_template.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dice::legacy_reply_references {

inline std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}
inline std::string trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
inline bool diceExpression(std::string text) {
    while (!text.empty() && (text.front() == '%' || text.front() == '_')) text.erase(text.begin());
    bool digit = false;
    for (unsigned char c : text) {
        if (std::isdigit(c)) digit = true;
        else if (std::string("dD+-*/() kKhHlLxX").find(c) == std::string::npos) return false;
    }
    return digit;
}
inline bool variable(const std::string& token) {
    static const std::set<std::string> names = {
        "nick", "name", "user", "qqnick", "card", "pcname", "qqnickw", "cardw", "pcnamew",
        "self", "strSelfName", "strSelfCall", "selfId", "group", "date", "time", "at",
        "pc", "char", "attr", "dice_exp", "reason", "turn"
    };
    return names.count(token) || (!token.empty() && token.front() == '$');
}
inline bool explicitReference(const std::string& token) {
    for (const auto* prefix : {"roll:", "draw:", "deck:", "help:", "text:", "api:", "counter:", "sample:"})
        if (token.rfind(prefix, 0) == 0) return true;
    return false;
}

struct Catalog {
    std::set<std::string> decks; // lowercase, matching the deck engine
    std::set<std::string> help;  // lowercase, trimmed
    std::set<std::string> texts; // exact legacy.* override names
    std::map<std::string, std::string> helpTargets; // normalized name -> actual i18n key spelling
};
struct Report {
    int converted = 0, ambiguous = 0, unresolved = 0;
    nlohmann::json details = nlohmann::json::array();
    nlohmann::json toJSON() const {
        return {{"converted", converted}, {"ambiguous", ambiguous}, {"unresolved", unresolved}, {"details", details}};
    }
};

// Walk authored braces only. No evaluation, random selection, or user-data interpolation.
// Nested sample/random branches are all migrated; escaped constructs stay literal.
inline std::string migrate(const std::string& text, const Catalog& catalog, Report& report,
                           const std::string& rule, const std::string& field, bool& compatibility,
                           unsigned depth = 0) {
    if (depth >= 32) { compatibility = true; return text; }
    std::string out;
    for (size_t pos = 0; pos < text.size();) {
        if (text[pos] != '{') { out += text[pos++]; continue; }
        size_t end = pos + 1, level = 1;
        for (; end < text.size(); ++end) {
            if (sample_template::escaped(text, end)) continue;
            if (text[end] == '{') ++level;
            else if (text[end] == '}' && --level == 0) break;
        }
        if (end == text.size()) { out += text.substr(pos); break; }
        const auto original = text.substr(pos, end - pos + 1);
        auto token = text.substr(pos + 1, end - pos - 1);
        if (sample_template::escaped(text, pos)) { out += original; pos = end + 1; continue; }
        if (token.find('{') != std::string::npos) {
            if (token.rfind("sample:", 0) == 0 || token.find('|') != std::string::npos)
                out += "{" + migrate(token, catalog, report, rule, field, compatibility, depth + 1) + "}";
            else out += original;
            pos = end + 1; continue;
        }
        if (explicitReference(token) || token.find('|') != std::string::npos) {
            out += original; pos = end + 1; continue;
        }
        const auto name = !token.empty() && token.front() == '%' ? token.substr(1) : token;
        const bool deck = catalog.decks.count(lower(name));
        const bool help = catalog.help.count(lower(trim(token)));
        const bool global = catalog.texts.count(token);
        const bool expression = diceExpression(token);
        const int candidates = static_cast<int>(deck) + help + global + expression;
        auto record = [&](const char* status, const char* reason, const std::string& target = "") {
            report.details.push_back({{"rule", rule}, {"field", field}, {"reference", original},
                                      {"status", status}, {"reason", reason}, {"target", target}});
        };
        if (variable(token)) {
            if (deck || help || global) { ++report.ambiguous; compatibility = true; record("ambiguous", "variable_name_collision"); }
            out += original;
        } else if (candidates > 1) {
            ++report.ambiguous; compatibility = true; record("ambiguous", "multiple_reference_types"); out += original;
        } else if (expression && candidates == 1) {
            // A character whitelist cannot prove expression validity, configured
            // limits, or runtime success. Bare failures stay literal whereas
            // roll: failures become '?'; do not silently change that behavior.
            ++report.ambiguous; compatibility = true; record("ambiguous", "dice_expression_requires_review"); out += original;
        } else if (candidates == 1) {
            std::string target;
            if (deck) target = "{deck:" + token + "}";
            else if (help) {
                const auto actual = catalog.helpTargets.find(lower(trim(token)));
                target = "{help:" + (actual == catalog.helpTargets.end() ? trim(token) : actual->second) + "}";
            }
            else if (global) target = "{text:" + token + "}";
            ++report.converted; record("converted", "unique_reference_type", target); out += target;
        } else {
            ++report.unresolved; compatibility = true; record("unresolved", "reference_not_found"); out += original;
        }
        pos = end + 1;
    }
    return out;
}

} // namespace dice::legacy_reply_references
