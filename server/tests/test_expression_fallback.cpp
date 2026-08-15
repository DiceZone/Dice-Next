#include "test_framework.h"
#include "../src/core/dice/dice_expression.h"
#include "../src/core/dice/dice_result.h"

#include <onedice/onedice.h>

using namespace dice;

TEST(ExpressionFallback, NativeUnsupportedSyntaxDoesNotConsumeRandomness) {
    int rolls = 0;
    DiceExpression expression([&](int) { ++rolls; return 3; });

    const DiceResult result = expression.evaluate("1d6kh1");

    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.failureKind == DiceFailureKind::kUnsupportedSyntax);
    ASSERT_EQ(rolls, 0);
}

TEST(ExpressionFallback, NativeEvaluationFailureIsNotFallbackEligible) {
    int rolls = 0;
    DiceExpression expression([&](int) { ++rolls; return 3; });

    const DiceResult result = expression.evaluate("1d6/0");

    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.failureKind == DiceFailureKind::kEvaluation);
    ASSERT_EQ(rolls, 1);
}

TEST(ExpressionFallback, OneDiceClassifiesUnsupportedSyntax) {
    const auto result = onedice::eval("1d6unknown", 100, [](long long) { return 1LL; });

    ASSERT_FALSE(result.ok);
    ASSERT_TRUE(result.errorKind == onedice::ErrorKind::UnsupportedSyntax);
}

TEST(ExpressionFallback, OneDiceClassifiesEvaluationFailure) {
    const auto result = onedice::eval("1d6/0", 100, [](long long) { return 1LL; });

    ASSERT_FALSE(result.ok);
    ASSERT_TRUE(result.errorKind == onedice::ErrorKind::Evaluation);
}

TEST(ExpressionFallback, OneDiceRequiresFullExpressionConsumption) {
    const auto result = onedice::eval("1d6 2", 100, [](long long) { return 1LL; });

    ASSERT_FALSE(result.ok);
    ASSERT_TRUE(result.errorKind == onedice::ErrorKind::UnsupportedSyntax);
}

TEST(ExpressionFallback, OneDiceProbeDoesNotUseExternalRandomState) {
    int rolls = 0;
    const auto probe = onedice::eval("4d6kh3", 100, [](long long) { return 1LL; });
    const auto actual = onedice::eval("4d6kh3", 100, [&](long long) { ++rolls; return 2LL; });

    ASSERT_TRUE(probe.ok);
    ASSERT_TRUE(actual.ok);
    ASSERT_EQ(rolls, 4);
}
