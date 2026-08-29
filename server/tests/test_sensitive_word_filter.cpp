#include "test_framework.h"
#include "../src/service/sensitive_word_filter.h"

using dice::censor::Level;

TEST(SensitiveWordFilter, MatchesLikeOriginalDice) {
    dice::censor::Settings settings;
    settings.enabled = true;
    settings.rules = {
        {"BadWord", Level::Caution},
        {"测试词", Level::Danger},
    };

    auto ascii = dice::censor::search(settings, ".rd b-a d_word");
    ASSERT_TRUE(ascii.found());
    ASSERT_EQ(static_cast<int>(ascii.level), static_cast<int>(Level::Caution));

    auto unicode = dice::censor::search(settings, "。r 测 试 词");
    ASSERT_TRUE(unicode.found());
    ASSERT_EQ(static_cast<int>(unicode.level), static_cast<int>(Level::Danger));
}

TEST(SensitiveWordFilter, ReturnsHighestMatchedLevel) {
    dice::censor::Settings settings{true, {
        {"alpha", Level::Notice},
        {"beta", Level::Warning},
        {"gamma", Level::Danger},
    }};
    auto match = dice::censor::search(settings, ".text alpha beta gamma");
    ASSERT_EQ(static_cast<int>(match.level), static_cast<int>(Level::Danger));
    ASSERT_EQ(match.words.size(), static_cast<size_t>(3));
}

TEST(SensitiveWordFilter, HonorsEnabledAndIgnoreLevels) {
    dice::censor::Settings settings{false, {{"blocked", Level::Danger}}};
    ASSERT_FALSE(dice::censor::search(settings, ".r blocked").found());
    settings.enabled = true;
    settings.rules = {{"blocked", Level::Ignore}};
    ASSERT_FALSE(dice::censor::search(settings, ".r blocked").found());
}

TEST(SensitiveWordFilter, CompiledMatcherUpdatesAtomically) {
    dice::censor::Matcher matcher;
    matcher.replace(dice::censor::Settings{true, {
        {"alpha", Level::Notice},
        {"alphabet", Level::Danger},
        {"测试词", Level::Warning},
    }});
    auto match = matcher.searchText(".r a-l-p-h-a-b-e-t 测 试 词");
    ASSERT_TRUE(match.found());
    ASSERT_EQ(static_cast<int>(match.level), static_cast<int>(Level::Danger));
    ASSERT_EQ(match.words.size(), static_cast<size_t>(3));

    matcher.replace(dice::censor::Settings{false, {{"alpha", Level::Danger}}});
    ASSERT_FALSE(matcher.searchText(".r alpha").found());
}

TEST(SensitiveWordFilter, AppliesLegacyTrustReduction) {
    ASSERT_FALSE(dice::censor::blocks(Level::Notice, 0));
    ASSERT_TRUE(dice::censor::blocks(Level::Caution, 0));
    ASSERT_TRUE(dice::censor::blocks(Level::Warning, 2));
    ASSERT_FALSE(dice::censor::blocks(Level::Caution, 3));
    ASSERT_FALSE(dice::censor::blocks(Level::Danger, 4));
}

TEST(SensitiveWordFilter, LoadsLegacyIntegerLevels) {
    auto settings = dice::censor::settingsFromJson(nlohmann::json{
        {"enabled", true},
        {"words", {{"one", 1}, {"four", 4}, {"named", "Caution"}}}
    });
    ASSERT_TRUE(settings.enabled);
    ASSERT_EQ(settings.rules.size(), static_cast<size_t>(3));
    ASSERT_EQ(static_cast<int>(dice::censor::parseLevel("danger")),
              static_cast<int>(Level::Danger));
}
