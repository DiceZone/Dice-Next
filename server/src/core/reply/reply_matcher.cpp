#include "reply_matcher.h"
#include "../../common/logger.h"

#include <algorithm>
#include <cctype>

namespace dice {

// ═══════════════════════════════════════════════════════════════
// Case-insensitive helpers
// ═══════════════════════════════════════════════════════════════

bool ReplyMatcher::icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }
    );
    return it != haystack.end();
}

// ═══════════════════════════════════════════════════════════════
// Unified entry
// ═══════════════════════════════════════════════════════════════

bool ReplyMatcher::match(const std::string& input,
                          const std::string& matchContent,
                          MatchType matchType) const {

    DICE_LOG_TRACE("ReplyMatcher: matching input (len={}) with type {} pattern='{}'",
        input.size(), matchTypeToString(matchType),
        matchContent.size() > 50 ? matchContent.substr(0, 50) + "..." : matchContent);

    switch (matchType) {
        case MatchType::kKeyword:
            return matchKeyword(input, matchContent);
        case MatchType::kPrefix:
            return matchPrefix(input, matchContent);
        case MatchType::kRegex:
            return matchRegex(input, matchContent);
        case MatchType::kSearch:
            return matchSearch(input, matchContent);
    }

    DICE_LOG_WARN("ReplyMatcher: unknown match type, defaulting to keyword");
    return matchKeyword(input, matchContent);
}

// ═══════════════════════════════════════════════════════════════
// Strategy implementations
// ═══════════════════════════════════════════════════════════════

bool ReplyMatcher::matchKeyword(const std::string& input,
                                 const std::string& keyword) const {
    // Exact match (case-insensitive)
    if (input.size() != keyword.size()) return false;
    return std::equal(
        input.begin(), input.end(),
        keyword.begin(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }
    );
}

bool ReplyMatcher::matchPrefix(const std::string& input,
                                const std::string& prefix) const {
    // Input starts with prefix (case-insensitive)
    if (prefix.size() > input.size()) return false;
    return std::equal(
        prefix.begin(), prefix.end(),
        input.begin(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }
    );
}

bool ReplyMatcher::validateRegex(const std::string& pattern, std::string* err) {
    if (pattern.empty()) { if (err) *err = "empty pattern"; return false; }
    if (pattern.size() > kMaxRegexLen) {
        if (err) *err = "pattern too long (max " + std::to_string(kMaxRegexLen) + " bytes)";
        return false;
    }
    try {
        std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
        (void)re;
        return true;
    } catch (const std::regex_error& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool ReplyMatcher::matchRegex(const std::string& input,
                               const std::string& pattern) const {
    if (pattern.size() > kMaxRegexLen) return false;   // 对齐原版 400 字上限（防 ReDoS）
    std::shared_ptr<const std::regex> re;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = regexCache_.find(pattern);
        if (it != regexCache_.end()) {
            re = it->second;                            // 命中缓存（nullptr=已知坏正则）
        } else {
            try {
                re = std::make_shared<const std::regex>(
                    pattern, std::regex::ECMAScript | std::regex::icase);
            } catch (const std::regex_error& e) {
                DICE_LOG_WARN("ReplyMatcher: invalid regex pattern '{}': {}", pattern, e.what());
            }
            if (regexCache_.size() >= 256) regexCache_.clear();   // 规则量级远小于此，防御性上限
            regexCache_.emplace(pattern, re);
        }
    }
    if (!re) return false;
    try {
        // Anchor at the START of the message: "测试(.*)" should match "测试一些功能"
        // but NOT "在测试一些功能". match_continuous requires the match to begin at
        // the first character (it still allows a partial-length match, so capture
        // groups work). Use the kSearch type for "appears anywhere" behavior.
        return std::regex_search(input, *re, std::regex_constants::match_continuous);
    } catch (...) { return false; }
}

bool ReplyMatcher::matchSearch(const std::string& input,
                                const std::string& term) const {
    // Fuzzy containment: the term appears anywhere in the input
    return icontains(input, term);
}

}  // namespace dice
