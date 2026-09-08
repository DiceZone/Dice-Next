#include "test_framework.h"
#include "../src/core/command_prefix_policy.h"

TEST(CommandPrefixPolicy, ScopedBotControlsRemainEmergencyCommands) {
    for (const auto* feature : {"log", "reply", "roll", "plugin"}) {
        const auto parsed = dice::parseBotControlCommand(std::string("bot ") + feature + " off 9000");
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(parsed->feature, std::string(feature));
        ASSERT_EQ(parsed->action, "off");
        ASSERT_EQ(parsed->target, "9000");
        ASSERT_TRUE(dice::forcedSafetyCommandBody(std::string("。bot ") + feature + " on").has_value());
    }
    ASSERT_FALSE(dice::parseBotControlCommand("botlog off").has_value());
    ASSERT_FALSE(dice::parseBotControlCommand("bot log reply off").has_value());
    ASSERT_FALSE(dice::parseBotControlCommand("bot log on off").has_value());
    ASSERT_FALSE(dice::parseBotControlCommand("bot on log").has_value());
    ASSERT_FALSE(dice::parseBotControlCommand("bot plugin custom").has_value());
}

TEST(CommandPrefixPolicy, KeepsDotForSafetyCommands) {
    ASSERT_TRUE(dice::forcedSafetyCommandBody(".bot").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody(".BOT on").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody(".bot off 5080").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody(".bot5080").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody(".boton").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody(".dismiss").has_value());
}

TEST(CommandPrefixPolicy, KeepsChineseFullStopForSafetyCommands) {
    ASSERT_TRUE(dice::forcedSafetyCommandBody("。bot").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody("。bot off").has_value());
    ASSERT_TRUE(dice::forcedSafetyCommandBody("。dismiss").has_value());
}

TEST(CommandPrefixPolicy, DoesNotRestoreOtherCommandsOrInvalidBotText) {
    ASSERT_FALSE(dice::forcedSafetyCommandBody(".roll 1d100").has_value());
    ASSERT_FALSE(dice::forcedSafetyCommandBody("。help").has_value());
    ASSERT_FALSE(dice::forcedSafetyCommandBody(".bot nonsense").has_value());
    ASSERT_FALSE(dice::forcedSafetyCommandBody(".dismiss now").has_value());
    ASSERT_FALSE(dice::forcedSafetyCommandBody("!bot").has_value());
}
