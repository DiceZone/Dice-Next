#include "test_framework.h"
#include "../src/adapter/adapter_interface.h"
#include "../src/core/dice/dice_expression.h"
#include "../src/core/dice/dice_result.h"
#include "../src/core/dice/roll_command_parser.h"
#include <onedice/onedice.h>

using namespace dice;

TEST(RollCommandParser, CompactDefaultDieKeepsSimplifiedChineseReason) {
    const auto parsed = roll_command::parse("d测试");
    ASSERT_TRUE(parsed.expression.empty());
    ASSERT_EQ(parsed.reason, std::string("测试"));
}

TEST(RollCommandParser, CompactDefaultDieKeepsTraditionalChineseReason) {
    const auto parsed = roll_command::parse("D測試");
    ASSERT_TRUE(parsed.expression.empty());
    ASSERT_EQ(parsed.reason, std::string("測試"));
}

TEST(RollCommandParser, CompactReasonDefaultWorksWithEveryExpressionEngine) {
    const auto parsed = roll_command::parse("d测试");
    const std::string expressionText =
        parsed.expression.empty() ? std::string("d100") : parsed.expression;

    DiceExpression native([](int) { return 1; });
    ASSERT_TRUE(native.evaluate(expressionText).ok());

    const auto compatible = onedice::eval(
        expressionText, 100, [](long long) { return 1LL; });
    ASSERT_TRUE(compatible.ok);

    dicescript_options options{};
    dicescript_result enhanced{};
    dicescript_default_options(&options);
    ASSERT_TRUE(dicescript_eval(expressionText.c_str(), &options, &enhanced));
}

TEST(RollCommandParser, UnicodeDiceModifiersRemainExpressions) {
    auto parsed = roll_command::parse("d优势测试");
    ASSERT_EQ(parsed.expression, std::string("d优势"));
    ASSERT_EQ(parsed.reason, std::string("测试"));

    parsed = roll_command::parse("d劣勢檢定");
    ASSERT_EQ(parsed.expression, std::string("d劣勢"));
    ASSERT_EQ(parsed.reason, std::string("檢定"));

    parsed = roll_command::parse("d＋2测试");
    ASSERT_EQ(parsed.expression, std::string("d＋2"));
    ASSERT_EQ(parsed.reason, std::string("测试"));
}

TEST(RollCommandParser, EnhancedDiceScriptPrefixesRemainIntact) {
    auto parsed = roll_command::parse("[1, 2, 3].sum() + 2d6 attack");
    ASSERT_EQ(parsed.expression, std::string("[1, 2, 3].sum() + 2d6"));
    ASSERT_EQ(parsed.reason, std::string("attack"));

    parsed = roll_command::parse("力量+1 检定");
    ASSERT_EQ(parsed.expression, std::string("力量+1"));
    ASSERT_EQ(parsed.reason, std::string("检定"));
}

TEST(RollCommandParser, MultiRollPrefixKeepsFollowingDiceExpression) {
    auto parsed = roll_command::parse("2#d100");
    ASSERT_EQ(parsed.expression, std::string("2#d100"));
    ASSERT_TRUE(parsed.reason.empty());

    parsed = roll_command::parse("12#d6 attack");
    ASSERT_EQ(parsed.expression, std::string("12#d6"));
    ASSERT_EQ(parsed.reason, std::string("attack"));

    parsed = roll_command::parse("2# d100测试");
    ASSERT_EQ(parsed.expression, std::string("2#d100"));
    ASSERT_EQ(parsed.reason, std::string("测试"));
}
TEST(RollCommandParser, NativeLexerReturnsValidUtf8ForUnicodeError) {
    DiceExpression expression([](int) { return 1; });
    const DiceResult result = expression.evaluate("d测试");
    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.error.find("测") != std::string::npos);
    ASSERT_TRUE(json(result.error).dump().find("unexpected") != std::string::npos);
}

TEST(RollCommandParser, AdapterJsonBoundaryReplacesMalformedUtf8) {
    std::string malformed = "reply:";
    malformed.push_back(static_cast<char>(0xe6));
    const std::string encoded = dumpJsonUtf8Safe(json{{"content", malformed}});
    const json decoded = json::parse(encoded);
    ASSERT_TRUE(decoded.at("content").get<std::string>().find("reply:") == 0);
    ASSERT_TRUE(encoded.find("\xef\xbf\xbd") != std::string::npos);
}
TEST(MessageFormat, GlobalTraditionalOverridesEveryScopedMode) {
    ASSERT_FALSE(IAdapter::resolveCardMessageMode(false, -1));
    ASSERT_FALSE(IAdapter::resolveCardMessageMode(false, 0));
    ASSERT_FALSE(IAdapter::resolveCardMessageMode(false, 1));
}

TEST(MessageFormat, GlobalRichModeStillAllowsScopedOptOut) {
    ASSERT_TRUE(IAdapter::resolveCardMessageMode(true, -1));
    ASSERT_FALSE(IAdapter::resolveCardMessageMode(true, 0));
    ASSERT_TRUE(IAdapter::resolveCardMessageMode(true, 1));
}
