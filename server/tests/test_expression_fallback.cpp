#include "test_framework.h"
#include "../src/core/dice/dice_expression.h"
#include "../src/core/dice/dice_result.h"

#include <onedice/onedice.h>
#include <dicescript/dicescript.h>

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
    const auto result = onedice::eval("1d6+2)", 100, [](long long) { return 1LL; });

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

TEST(ExpressionFallback, OneDiceProbeEvaluationCanDependOnRandomOutcome) {
    const auto probe = onedice::eval(
        "4d6kh3/(1d2-1)", 100, [](long long) { return 1LL; });

    ASSERT_FALSE(probe.ok);
    ASSERT_TRUE(probe.errorKind == onedice::ErrorKind::Evaluation);

    const auto actual = onedice::eval(
        "4d6kh3/(1d2-1)", 100, [](long long faces) { return faces; });
    ASSERT_TRUE(actual.ok);
    ASSERT_EQ(actual.value, 18);
}

static uint64_t diceScriptMax(void* userdata, uint64_t upperBound) {
    auto* calls = static_cast<uint32_t*>(userdata);
    if (calls != nullptr) ++*calls;
    return upperBound - 1;
}

TEST(ExpressionFallback, DiceScriptValidationDoesNotConsumeRandomness) {
    dicescript_options options{};
    dicescript_result result{};
    uint32_t calls = 0;

    dicescript_default_options(&options);
    options.random = diceScriptMax;
    options.random_userdata = &calls;

    ASSERT_TRUE(dicescript_validate("10d10d1", &result));
    ASSERT_EQ(calls, 0u);

    ASSERT_TRUE(dicescript_eval("10d10d1", &options, &result));
    ASSERT_TRUE(result.is_integer);
    ASSERT_EQ(result.integer, 100);
    ASSERT_EQ(calls, 110u);
}

TEST(ExpressionFallback, DiceScriptKeepsEvaluationFailuresTerminal) {
    dicescript_options options{};
    dicescript_result result{};
    dicescript_default_options(&options);
    options.random = diceScriptMax;

    ASSERT_FALSE(dicescript_eval("1d6/0", &options, &result));
    ASSERT_TRUE(result.error_kind == DICESCRIPT_ERROR_EVALUATION);
}
