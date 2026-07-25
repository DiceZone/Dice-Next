#pragma once

#include <string>
#include <vector>
#include <regex>
#include "../../common/types.h"

namespace dice {

/**
 * @brief Reply rule matching engine.
 *
 * Implements four matching strategies:
 *   1. keyword — exact substring match
 *   2. prefix  — input starts with pattern
 *   3. regex   — std::regex match
 *   4. search  — case-insensitive fuzzy containment
 *
 * All matching functions return true if the input matches
 * the given pattern according to the respective strategy.
 */
class ReplyMatcher {
public:
    ReplyMatcher() = default;

    /**
     * @brief Unified entry point: dispatch to the correct
     *        strategy based on the MatchType enum.
     *
     * @param input      The raw user message text.
     * @param rule       The reply rule (contains matchType and matchContent).
     * @param matchType  Optional override for the match strategy.
     * @return true if the input matches.
     */
    bool match(const std::string& input, const std::string& matchContent,
               MatchType matchType) const;

    /**
     * @brief Exact keyword match (case-insensitive).
     */
    bool matchKeyword(const std::string& input, const std::string& keyword) const;

    /**
     * @brief Prefix match — input starts with the given prefix.
     */
    bool matchPrefix(const std::string& input, const std::string& prefix) const;

    /**
     * @brief Regular expression match using std::regex.
     *        Default flavor: ECMAScript.
     */
    bool matchRegex(const std::string& input, const std::string& pattern) const;

    /**
     * @brief Fuzzy search — the term appears anywhere in the
     *        input (case-insensitive).
     */
    bool matchSearch(const std::string& input, const std::string& term) const;

private:
    // ─── Regex cache (reuse compiled regexes) ────────────────
    mutable std::vector<std::pair<std::string, std::unique_ptr<std::regex>>> regexCache_;

    // Simple helper: case-insensitive substring match
    static bool icontains(const std::string& haystack, const std::string& needle);
};

}  // namespace dice
