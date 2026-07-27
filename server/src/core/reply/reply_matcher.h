#pragma once

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include "../../common/types.h"

namespace dice {

/**
 * @brief Reply rule matching engine.
 *
 * Implements four matching strategies:
 *   1. keyword — FULL-TEXT exact match, case-insensitive
 *                （整条消息与关键词完全相等，对齐原版 Match 模式；
 *                「出现在消息里即可」请用 search）
 *   2. prefix  — input starts with pattern (case-insensitive)
 *   3. regex   — std::regex, anchored at message start
 *   4. search  — case-insensitive containment (anywhere)
 *
 * All matching functions return true if the input matches
 * the given pattern according to the respective strategy.
 */
class ReplyMatcher {
public:
    ReplyMatcher() = default;

    /// 正则长度上限（对齐原版 400 字防 ReDoS/爆栈；超限拒绝编译）。
    static constexpr size_t kMaxRegexLen = 400;

    /**
     * @brief 校验一个正则是否可用（长度 + 可编译）。保存规则前调用，
     *        别让写错的正则静默变成永不命中的死规则。
     * @param err  非空时写入失败原因。
     */
    static bool validateRegex(const std::string& pattern, std::string* err = nullptr);

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
     * @brief Full-text exact match (case-insensitive). 原版 Match 语义。
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
    // ─── Regex cache ─────────────────────────────────────────
    // 每条消息×每条正则规则都现编译太浪费；按 pattern 缓存编译结果
    // （编译失败缓存 nullptr，坏正则只告警一次）。多线程（消息线程+
    // 网页测试）并发访问，用互斥锁保护。
    mutable std::map<std::string, std::shared_ptr<const std::regex>> regexCache_;
    mutable std::mutex cacheMutex_;

    // Simple helper: case-insensitive substring match
    static bool icontains(const std::string& haystack, const std::string& needle);
};

}  // namespace dice
