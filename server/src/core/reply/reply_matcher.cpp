#include "reply_matcher.h"
#include "../../common/logger.h"
#include "../../common/bounded_regex.h"

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
    const auto result = boundedRegex(pattern, "", RegexMode::Full, true);
    if (err) *err = result.error;
    return result.error.empty();
}

bool ReplyMatcher::matchRegex(const std::string& input,
                               const std::string& pattern) const {
    return regexMatch(input, pattern).matched;
}

RegexResult ReplyMatcher::regexMatch(const std::string& input, const std::string& pattern) {
    if (pattern.size() > kMaxRegexLen)
        return {false, {}, "pattern too long (max " + std::to_string(kMaxRegexLen) + " bytes)"};
    return boundedRegex(pattern, input, RegexMode::Full);
}

bool ReplyMatcher::matchSearch(const std::string& input,
                                const std::string& term) const {
    // Fuzzy containment: the term appears anywhere in the input
    return icontains(input, term);
}

}  // namespace dice
