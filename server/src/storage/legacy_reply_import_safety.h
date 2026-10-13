#pragma once

// Migration-only validation. Unsupported legacy restrictions must never become
// active, unrestricted replies. The source rule is retained in the import report.
#include "../core/reply/reply_definition.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dice::legacyv2::reply_import_safety {
using json = nlohmann::json;

inline std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \r\n\t");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \r\n\t") - begin + 1);
}

inline bool unsignedInteger(const std::string& value, int& number) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
    try {
        const auto parsed = std::stoll(value);
        if (parsed > (std::numeric_limits<int>::max)()) return false;
        number = static_cast<int>(parsed);
        return true;
    } catch (...) { return false; }
}

inline std::string readSource(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("legacy reply/blacklist source is not a regular file");
    // Restrict allocation before parsing; keep original files untouched.
    if (std::filesystem::file_size(path) > 64 * 1024 * 1024) throw std::runtime_error("legacy reply/blacklist source exceeds 64 MiB import limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read legacy reply/blacklist source");
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("failed reading legacy reply/blacklist source");
    return body;
}

inline void diagnostic(json* out, const std::string& source, const std::string& name,
                       const std::string& status, const std::vector<std::string>& reasons,
                       const json& original = nullptr, int ruleId = 0, bool disabled = false) {
    if (!out) return;
    if (!out->is_array()) *out = json::array();
    json detail{{"source", source}, {"name", name}, {"status", status}, {"reasons", reasons}, {"disabled", disabled},
                {"requiresReview", status == "disabled" || status == "failed" || status == "skipped" || status == "warning" || status == "conflict" || status == "preserved"}};
    if (!reasons.empty()) detail["reason"] = reasons.front();
    if (ruleId > 0) detail["ruleId"] = ruleId;
    if (!original.is_null()) {
        const auto raw = original.dump();
        if (raw.size() <= 65536) detail["original"] = original;
        else {
            size_t end = 65536;
            while (end > 0 && (static_cast<unsigned char>(raw[end]) & 0xC0) == 0x80) --end;
            detail["originalExcerpt"] = raw.substr(0, end);
            detail["originalTruncated"] = true;
            detail["originalBytes"] = raw.size();
        }
    }
    out->push_back(std::move(detail));
}

inline void readLimits(const json& source, ReplyRule& rule, std::vector<std::string>& reasons) {
    if (!source.contains("limit")) return;
    if (!source["limit"].is_string()) { reasons.push_back("limit is not a string; legacy restriction retained but not enabled"); return; }
    std::set<std::string> seen;
    std::stringstream limits(source["limit"].get<std::string>());
    std::string segment;
    while (std::getline(limits, segment, ';')) {
        segment = trim(segment);
        if (segment.empty()) continue;
        const auto colon = segment.find(':');
        const auto key = trim(segment.substr(0, colon));
        if (colon == std::string::npos) { reasons.push_back("unsupported or malformed restriction: " + key); continue; }
        const auto value = trim(segment.substr(colon + 1));
        if (!seen.insert(key).second) { reasons.push_back("repeated restriction cannot be represented losslessly: " + key); continue; }
        if (key == "prob") {
            int number = 0;
            if (!unsignedInteger(value, number)) reasons.push_back("invalid probability restriction");
            // Dice! treats 0/out-of-range probability as 'no probability limit'.
            else rule.prob = number > 0 && number < 100 ? number : 100;
        } else if (key == "cd" || key == "today") {
            std::stringstream pieces(value); std::string piece;
            int number = 0, counters = 0;
            bool valid = true;
            std::string notice;
            while (std::getline(pieces, piece, '&')) {
                piece = trim(piece);
                if (piece.rfind("@echo=", 0) == 0) { notice = piece.substr(6); continue; }
                if (piece.empty() || !unsignedInteger(piece, number)) { valid = false; continue; }
                ++counters;
            }
            if (!valid || counters != 1) {
                reasons.push_back("named, shared, scoped or malformed " + key + " counter cannot be represented losslessly");
            } else if (key == "cd") { rule.cooldownSec = number; rule.cooldownNotice = notice; }
            else { rule.dayLimit = number; rule.dayLimitNotice = notice; }
        } else if (key == "grp_id" || key == "user_id") {
            bool deny = !value.empty() && value.front() == '!';
            std::stringstream pieces(deny ? value.substr(1) : value); std::string piece, csv;
            bool valid = true;
            while (std::getline(pieces, piece, '&')) {
                piece = trim(piece);
                if (piece.empty() || !std::all_of(piece.begin(), piece.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) { valid = false; continue; }
                // QQ identifiers may exceed int; keep decimal digits as text.
                const auto nonzero = piece.find_first_not_of('0');
                piece = nonzero == std::string::npos ? "0" : piece.substr(nonzero);
                if (!csv.empty()) csv += ',';
                csv += piece;
            }
            if (!valid || csv.empty()) reasons.push_back("invalid identifier restriction: " + key);
            else if (key == "grp_id") { rule.scopeMode = deny ? "deny" : "allow"; rule.scopeIds = csv; }
            else { rule.scopeUsersMode = deny ? "deny" : "allow"; rule.scopeUsers = csv; }
        } else {
            reasons.push_back("unsupported legacy restriction: " + key);
        }
    }
}

inline std::string lowerAscii(std::string value) {
    for (auto& character : value) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return value;
}

inline ReplyRule convert(const std::string& key, const json& source, std::vector<std::string>& reasons) {
    ReplyRule rule; rule.logic = "or"; rule.priority = 100;
    if (source.contains("type")) {
        if (!source["type"].is_string() || lowerAscii(source["type"].get<std::string>()) != "reply")
            reasons.push_back("legacy type switch is not equivalent to an ordinary reply");
    }
    const bool explicitMatch = source.contains("match") || source.contains("prefix") || source.contains("search") || source.contains("regex") || source.contains("mode");
    auto addWords = [&](const char* field, MatchType type) {
        if (!source.contains(field)) return;
        if (!source[field].is_array()) { reasons.push_back(std::string("invalid match list: ") + field); return; }
        for (const auto& value : source[field]) {
            if (!value.is_string()) { reasons.push_back(std::string("non-text match item: ") + field); continue; }
            const auto word = value.get<std::string>();
            if (!word.empty()) rule.conditions.push_back({type, word});
        }
    };
    // The old reader permits mode plus explicit lists. Preserve all of them.
    if (source.contains("mode")) {
        if (!source["mode"].is_string()) reasons.push_back("invalid legacy match mode");
        else {
            const auto mode = lowerAscii(source["mode"].get<std::string>());
            const auto type = mode == "prefix" ? MatchType::kPrefix : mode == "search" ? MatchType::kSearch : mode == "regex" ? MatchType::kRegex : MatchType::kKeyword;
            if (mode != "match" && mode != "prefix" && mode != "search" && mode != "regex") reasons.push_back("unknown legacy match mode");
            const char* field = type == MatchType::kPrefix ? "prefix" : type == MatchType::kSearch ? "search" : type == MatchType::kRegex ? "regex" : "match";
            if (!source.contains(field)) {
                if (source.contains("keyword") && !source["keyword"].is_string()) reasons.push_back("invalid legacy keyword");
                else {
                    const auto keyword = source.value("keyword", key);
                    if (type == MatchType::kRegex) { if (!keyword.empty()) rule.conditions.push_back({type, keyword}); }
                    else { std::stringstream words(keyword); std::string word; while (std::getline(words, word, '|')) if (!word.empty()) rule.conditions.push_back({type, word}); }
                }
            }
        }
    }
    addWords("match", MatchType::kKeyword); addWords("prefix", MatchType::kPrefix);
    addWords("search", MatchType::kSearch); addWords("regex", MatchType::kRegex);
    if (!explicitMatch) {
        std::stringstream words(key); std::string word;
        while (std::getline(words, word, '|')) if (!word.empty()) rule.conditions.push_back({MatchType::kKeyword, word});
    }
    // Preserve the old precedence (Match -> Prefix -> Search -> Regex), since
    // the first matched branch supplies captures and suffix variables.
    std::stable_sort(rule.conditions.begin(), rule.conditions.end(), [](const auto& a, const auto& b) {
        auto rank = [](MatchType type) { return type == MatchType::kKeyword ? 0 : type == MatchType::kPrefix ? 1 : type == MatchType::kSearch ? 2 : 3; };
        return rank(a.type) < rank(b.type);
    });
    if (rule.conditions.empty()) reasons.push_back("no usable match condition");
    for (const auto& condition : rule.conditions) if (condition.type == MatchType::kRegex) {
        std::string error;
        if (!ReplyMatcher::validateRegex(condition.content, &error)) reasons.push_back("regex rejected by the active engine: " + error);
    }
    if (source.contains("answer")) {
        if (source["answer"].is_array()) {
            for (const auto& answer : source["answer"]) {
                if (!answer.is_string()) { reasons.push_back("non-text legacy answer cannot be represented losslessly"); continue; }
                if (!answer.get<std::string>().empty()) rule.results.push_back(answer.get<std::string>());
            }
        } else if (source["answer"].is_string()) { if (!source["answer"].get<std::string>().empty()) rule.results.push_back(source["answer"].get<std::string>()); }
        else reasons.push_back("invalid legacy answer");
    }
    if (rule.results.empty()) reasons.push_back("no usable text answer");
    auto answers = rule.results;
    reply_definition::decodeResults(json(answers), rule.results, rule.resultWeights);
    if (const auto error = reply_definition::validateWeights(rule); !error.empty()) reasons.push_back(error);
    readLimits(source, rule, reasons);
    rule.enabled = reasons.empty();
    return rule;
}
} // namespace dice::legacyv2::reply_import_safety
