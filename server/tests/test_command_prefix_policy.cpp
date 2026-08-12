#include "test_framework.h"
#include "../src/core/command_prefix_policy.h"

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
