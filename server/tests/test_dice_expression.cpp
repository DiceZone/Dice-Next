#include "test_framework.h"
#include "../src/core/dice/dice_expression.h"
#include "../src/core/dice/dice_result.h"

#include <vector>

using namespace dice;

TEST(DiceExpression, RepeatedDiceOperatorsAssociateLeftToRight) {
    std::vector<int> requestedSides;
    DiceExpression expression([&](int sides) {
        requestedSides.push_back(sides);
        return sides == 1 ? 1 : 5;
    });

    const DiceResult result = expression.evaluate("10d10d1");

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.modifiedTotal, 50);
    ASSERT_TRUE(result.formattedOutput.rfind("50D1={", 0) == 0);
    ASSERT_TRUE(result.formattedOutput.size() >= 8
        && result.formattedOutput.substr(result.formattedOutput.size() - 8) == "}(50)=50");
    // Left association first rolls 10D10 (10 calls), then rolls its result
    // 50D1 (50 calls).  Right association would call D1 first and only make
    // 20 calls in total, despite coincidentally producing the same total.
    ASSERT_EQ((int)requestedSides.size(), 60);
    for (int i = 0; i < 10; ++i) ASSERT_EQ(requestedSides[i], 10);
    for (size_t i = 10; i < requestedSides.size(); ++i) ASSERT_EQ(requestedSides[i], 1);
}

TEST(DiceExpression, ThreeDiceOperatorsFoldInSourceOrder) {
    std::vector<int> requestedSides;
    DiceExpression expression([&](int sides) {
        requestedSides.push_back(sides);
        return 1;
    });

    const DiceResult result = expression.evaluate("2d6d4d1");

    ASSERT_TRUE(result.ok());
    // (2D6)=2, then (2D4)=2, then (2D1)=2.
    ASSERT_EQ(result.modifiedTotal, 2);
    ASSERT_EQ((int)requestedSides.size(), 6);
    ASSERT_EQ(requestedSides[0], 6);
    ASSERT_EQ(requestedSides[1], 6);
    ASSERT_EQ(requestedSides[2], 4);
    ASSERT_EQ(requestedSides[3], 4);
    ASSERT_EQ(requestedSides[4], 1);
    ASSERT_EQ(requestedSides[5], 1);
}

TEST(DiceExpression, ParenthesesCanStillRequestRightAssociationExplicitly) {
    std::vector<int> requestedSides;
    DiceExpression expression([&](int sides) {
        requestedSides.push_back(sides);
        return 1;
    });

    const DiceResult result = expression.evaluate("2d(6d1)");

    ASSERT_TRUE(result.ok());
    // The parenthesized side expression is evaluated first.
    ASSERT_EQ((int)requestedSides.size(), 8);
    for (int i = 0; i < 6; ++i) ASSERT_EQ(requestedSides[i], 1);
    ASSERT_EQ(requestedSides[6], 6);
    ASSERT_EQ(requestedSides[7], 6);
}
