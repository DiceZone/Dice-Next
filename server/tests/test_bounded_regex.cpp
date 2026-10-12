#include "test_framework.h"
#include "test_watchdog.h"
#include "common/bounded_regex.h"
#include "core/reply/reply_matcher.h"

#include <regex>
#include <nlohmann/json.hpp>

using namespace dice;

TEST(ReplyRegex, LegacyChineseLengthLimitAppliesToTheWholeMessage) {
    ReplyMatcher matcher;
    const std::string pattern = "^白(丽|麗).{0,6}";
    ASSERT_TRUE(ReplyMatcher::validateRegex(pattern));
    for (const auto* name : {"白丽", "白麗"}) {
        for (int count = 0; count <= 7; ++count) {
            const std::string text = std::string(name) + std::string(count, 'a');
            ASSERT_EQ(matcher.matchRegex(text, pattern), count <= 6);
            ASSERT_EQ(matcher.match(text, pattern, MatchType::kRegex), count <= 6);
        }
        ASSERT_TRUE(matcher.matchRegex(std::string(name) + "一二三四五六", pattern));
        ASSERT_FALSE(matcher.matchRegex(std::string(name) + "一二三四五六七", pattern));
        ASSERT_FALSE(matcher.matchRegex("前面" + std::string(name), pattern));
    }
    ASSERT_FALSE(matcher.matchRegex("白丽\n", pattern));
    ASSERT_TRUE(matcher.matchPrefix("白丽1234567", "白丽"));
    ASSERT_TRUE(matcher.matchSearch("前面白丽后面", "白丽"));
    ASSERT_FALSE(matcher.matchKeyword("白丽后面", "白丽"));
}

TEST(BoundedRegex, FullMatchingBacktracksWithoutChangingCaptureNumbers) {
    const auto alternative = boundedRegex("(a)|(ab)", "ab", RegexMode::Full);
    ASSERT_TRUE(alternative.matched);
    ASSERT_EQ(alternative.groups.size(), size_t(3));
    ASSERT_EQ(alternative.groups[0], "ab");
    ASSERT_EQ(alternative.groups[1], "");
    ASSERT_EQ(alternative.groups[2], "ab");
    const auto lazy = boundedRegex("(a+?)(a*)", "aaaa", RegexMode::Full);
    ASSERT_TRUE(lazy.matched);
    ASSERT_EQ(lazy.groups[1], "a");
    ASSERT_EQ(lazy.groups[2], "aaa");
    const auto boundedLazy = boundedRegex("(.{0,6}?)", "abcdef", RegexMode::Full);
    ASSERT_TRUE(boundedLazy.matched);
    ASSERT_EQ(boundedLazy.groups[1], "abcdef");
    ASSERT_FALSE(boundedRegex("(.{0,6}?)", "abcdefg", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("a|ab", "ab", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("a??", "a", RegexMode::Full).matched);
}

TEST(BoundedRegex, CachedModesDoNotLeakFullMatchingIntoSearchOrPrefix) {
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(boundedRegex("a|ab", "", RegexMode::Full, true).error.empty());
        ASSERT_EQ(boundedRegex("a|ab", "ab", RegexMode::Full).groups[0], "ab");
        ASSERT_EQ(boundedRegex("a|ab", "ab", RegexMode::Prefix).groups[0], "a");
        ASSERT_EQ(boundedRegex("a|ab", "xab", RegexMode::Search).groups[0], "a");
        ASSERT_FALSE(boundedRegex("a|ab", "xab", RegexMode::Prefix).matched);
        ASSERT_FALSE(boundedRegex("a|ab", "xab", RegexMode::Full).matched);
    }
}

TEST(BoundedRegex, WholeMatchingPreservesWhitespaceAndHandlesLineTerminators) {
    for (const auto* lineBreak : {"\n", "\r", "\r\n", "\xe2\x80\xa8", "\xe2\x80\xa9"}) {
        const std::string text = std::string("a") + lineBreak;
        ASSERT_FALSE(boundedRegex(".*", text, RegexMode::Full).matched);
        const auto multiline = boundedRegex("([\\s\\S]*)", text, RegexMode::Full);
        ASSERT_TRUE(multiline.matched);
        ASSERT_EQ(multiline.groups[1], text);
    }
    ASSERT_FALSE(boundedRegex("a", " a ", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex(" a ", " a ", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("\\s+", " \t\n", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("", "", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex("", "a", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex("a$", "a\n", RegexMode::Full).matched);
    // Default ECMAScript/i does not enable multiline anchors. Some MSVC
    // std::regex versions treat these as multiline in regex_search(), so use
    // explicit specification expectations rather than that implementation here.
    ASSERT_FALSE(boundedRegex("^a.*$", "a\nb", RegexMode::Search).matched);
    ASSERT_FALSE(boundedRegex("^a.*$", "a\nb", RegexMode::Prefix).matched);
    ASSERT_TRUE(boundedRegex(".*", "a\nb", RegexMode::Search).matched);
}

TEST(BoundedRegex, CapturesPreserveEmbeddedNulAndUtf8) {
    const std::string text("a\0b", 3);
    const auto nul = boundedRegex("(a)([\\s\\S]*)(b)", text, RegexMode::Full);
    ASSERT_TRUE(nul.matched);
    ASSERT_EQ(nul.groups.size(), size_t(4));
    ASSERT_EQ(nul.groups[0], text);
    ASSERT_EQ(nul.groups[2], std::string(1, '\0'));
    const auto chinese = boundedRegex("(白(?:丽|麗))(.{0,6})", "白麗测试文本", RegexMode::Full);
    ASSERT_TRUE(chinese.matched);
    ASSERT_EQ(chinese.groups[1], "白麗");
    ASSERT_EQ(chinese.groups[2], "测试文本");
}

TEST(BoundedRegex, EcmaClassesEscapesLookaroundAndBackreferences) {
    ASSERT_TRUE(boundedRegex(R"((a)(b)\2\1)", "abba", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex(R"((a)\1)", "aA", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex(R"(\d{2})", "12", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex(R"(\d{2})", "１２", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex(R"(\w+)", "中文", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex(R"(\w+)", "Ab_12", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex(R"(\[a\]\.\{2\})", "[a].{2}", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("(?=ab)ab", "ab", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("a(?!b).", "ac", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex("a(?!b).", "ab", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex("ab(?=c)", "abc", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("a(?<=a)(b)", "ab", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex(R"((?<name>ab)\k<name>)", "abab", RegexMode::Full).matched);
    ASSERT_EQ(boundedRegex("(a)?b", "b", RegexMode::Full).groups[1], "");
}

TEST(BoundedRegex, NonUnicodeModeHasExplicitUtf16CharacterAndCaseSemantics) {
    // Keep existing ECMAScript/i syntax; silently adding 'u' would reject
    // legacy identity escapes and alter case folding/quantifier behavior.
    ASSERT_FALSE(boundedRegex(".", "🙂", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("..", "🙂", RegexMode::Full).matched);
    ASSERT_EQ(boundedRegex("(..)", "🙂", RegexMode::Full).groups[1], "🙂");
    ASSERT_TRUE(boundedRegex("(?:🙂){2}", "🙂🙂", RegexMode::Full).matched);
    ASSERT_TRUE(boundedRegex("ä", "Ä", RegexMode::Full).matched);
    ASSERT_FALSE(boundedRegex("k", "K", RegexMode::Full).matched);
}

TEST(BoundedRegex, SplitEmojiCapturesAreWellFormedUtf8ForRepliesAndJson) {
    const auto split = boundedRegex("(.)(.)", "🙂", RegexMode::Full);
    ASSERT_TRUE(split.matched);
    ASSERT_EQ(split.groups.size(), size_t(3));
    bool serializable = true;
    try { (void)nlohmann::json(split.groups).dump(); }
    catch (const nlohmann::json::type_error&) { serializable = false; }
    ASSERT_TRUE(serializable);
    ASSERT_EQ(split.groups[0], "🙂");
    ASSERT_EQ(split.groups[1], "\xef\xbf\xbd");
    ASSERT_EQ(split.groups[2], "\xef\xbf\xbd");
    const auto literal = boundedRegex(R"((\ud83d)\ude42)", "🙂", RegexMode::Full);
    ASSERT_TRUE(literal.matched);
    ASSERT_EQ(literal.groups[1], "\xef\xbf\xbd");
    ASSERT_EQ(boundedRegex("(..)", "🙂", RegexMode::Full).groups[1], "🙂");
}

TEST(BoundedRegex, InvalidOriginalPatternsAndSizeLimitsFailClosed) {
    for (const auto* pattern : {"[", "(", ")", "\\", "a{2,1}", "(?i)a", "a++", "(?P<name>a)"}) {
        const auto result = boundedRegex(pattern, "a", RegexMode::Full);
        ASSERT_FALSE(result.matched);
        ASSERT_TRUE(!result.error.empty());
    }
    ASSERT_TRUE(!boundedRegex(std::string(4097, 'a'), "a").error.empty());
    ASSERT_TRUE(!boundedRegex("a*", std::string(65537, 'a'), RegexMode::Full).error.empty());
    ASSERT_TRUE(boundedRegex("a*", std::string(65536, 'a'), RegexMode::Full).matched);
    ASSERT_TRUE(!ReplyMatcher::validateRegex(std::string(401, 'a')));
    ASSERT_TRUE(ReplyMatcher::regexMatch("a", std::string(401, 'a')).groups.empty());
}

TEST(BoundedRegex, FullBacktrackingStillHasTimeoutAndRecoveryProtection) {
    TestWatchdog watchdog;
    BoundedRegexEngine engine;
    const auto started = std::chrono::steady_clock::now();
    const std::string pattern = "^(a+)+$";
    const auto bad = engine.match(pattern, std::string(100, 'a') + "!", RegexMode::Full, false);
    ASSERT_FALSE(bad.matched);
    ASSERT_TRUE(!bad.error.empty());
    ASSERT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    ASSERT_TRUE(!engine.match(pattern, "a", RegexMode::Full, false).error.empty());
    const auto good = engine.match("(a|ab)", "ab", RegexMode::Full, false);
    ASSERT_TRUE(good.matched);
    ASSERT_EQ(good.groups[1], "ab");
}

TEST(BoundedRegex, CommonEcmaSubsetMatchesCppReferenceInAllThreeModes) {
    TestWatchdog watchdog(std::chrono::seconds(15));
    const std::vector<std::string> patterns{
        "", "a", "ab", "a|ab", "(a)|(ab)", "(a|ab)(b?)", "a*", "a*?", "(a+?)",
        "a?b", "a{0,2}", "a{0,2}?b", "(ab)+", "a(b)?a", "[ab]+", "[^ab]{0,2}",
        R"(\d+)", R"(\w+)", R"(\s+)", R"(\b(a+)\b)", R"(\Ba)", "^a.*$",
        "(?=a)a.*", "a(?!b).*", R"((a)\1)", R"((a|b)\1)", R"(a\.)", R"(\[a\])",
        "a$", "^(a|ab)$"
    };
    std::vector<std::string> inputs{""}, level{""};
    for (int depth = 0; depth < 3; ++depth) {
        std::vector<std::string> next;
        for (const auto& prefix : level) for (const char c : std::string("abA0 ")) {
            next.push_back(prefix + c); inputs.push_back(next.back());
        }
        level = std::move(next);
    }
    for (const auto* text : {"abab", "abb", " a ", "a.b", "[a]", "a\nb"}) inputs.emplace_back(text);
    inputs.emplace_back("a\0b", 3);
    for (const auto& pattern : patterns) {
        // ASCII fixtures deliberately avoid locale/platform-specific wctype
        // extensions; Unicode and new ECMAScript features are tested above.
        const std::wregex reference(std::wstring(pattern.begin(), pattern.end()),
            std::regex::ECMAScript | std::regex::icase);
        for (const auto& input : inputs) for (const auto mode : {RegexMode::Search, RegexMode::Prefix, RegexMode::Full}) {
#ifdef _MSC_VER
            // Full-mode comparisons still include every input. Partial-mode
            // newline anchors have explicit ECMAScript tests above; don't turn
            // an MSVC-specific behavior into a portable engine requirement.
            if (mode != RegexMode::Full && input.find('\n') != std::string::npos &&
                (pattern.find('^') != std::string::npos || pattern.find('$') != std::string::npos)) continue;
#endif
            const std::wstring wide(input.begin(), input.end());
            std::wsmatch expectedGroups;
            const bool expected = mode == RegexMode::Full ? std::regex_match(wide, expectedGroups, reference)
                : std::regex_search(wide, expectedGroups, reference,
                    mode == RegexMode::Prefix ? std::regex_constants::match_continuous : std::regex_constants::match_default);
            const auto actual = boundedRegex(pattern, input, mode);
            if (actual.matched != expected || !actual.error.empty())
                std::cerr << "regex reference mismatch: " << pattern << ", mode=" << static_cast<int>(mode)
                          << ", inputBytes=" << input.size() << "\n";
            ASSERT_TRUE(actual.error.empty());
            ASSERT_EQ(actual.matched, expected);
            if (expected) {
                ASSERT_EQ(actual.groups.size(), expectedGroups.size());
                for (size_t i = 0; i < expectedGroups.size(); ++i) {
                    const auto value = expectedGroups[i].str();
                    std::string ascii;
                    for (const wchar_t c : value) ascii.push_back(static_cast<char>(c));
                    ASSERT_EQ(actual.groups[i], ascii);
                }
            }
        }
    }
}
