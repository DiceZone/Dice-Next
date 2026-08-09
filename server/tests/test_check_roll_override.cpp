#include "test_framework.h"
#include "../src/core/check_roll_override.h"

using namespace dice;

TEST(CheckRollOverride, ParsesAttachedOutcomeAndRate) {
    const auto parsed = parseCheckRollOverride("(1)1");
    ASSERT_TRUE(parsed.present);
    ASSERT_TRUE(parsed.valid);
    ASSERT_EQ(parsed.expression, "1");
    ASSERT_EQ(parsed.rest, "1");
}

TEST(CheckRollOverride, PreservesNestedExpressionAndArguments) {
    const auto parsed = parseCheckRollOverride(" ((1 + 2) * 3) 侦查 50 试投");
    ASSERT_TRUE(parsed.present);
    ASSERT_TRUE(parsed.valid);
    ASSERT_EQ(parsed.expression, "(1 + 2) * 3");
    ASSERT_EQ(parsed.rest, "侦查 50 试投");
}

TEST(CheckRollOverride, LeavesOrdinaryCheckUntouched) {
    const auto parsed = parseCheckRollOverride("侦查 50");
    ASSERT_FALSE(parsed.present);
    ASSERT_TRUE(parsed.valid);
}

TEST(CheckRollOverride, RejectsEmptyOrUnclosedExpression) {
    const auto empty = parseCheckRollOverride("()50");
    ASSERT_TRUE(empty.present);
    ASSERT_FALSE(empty.valid);

    const auto unclosed = parseCheckRollOverride("(1d100 50");
    ASSERT_TRUE(unclosed.present);
    ASSERT_FALSE(unclosed.valid);
}
