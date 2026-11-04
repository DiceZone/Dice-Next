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

namespace {
std::string rendered(const std::string& input, int face = 4) {
    DiceExpression expression([face](int) { return face; });
    const DiceResult result = expression.evaluate(input);
    return result.ok() ? result.formattedOutput : "error: " + result.error;
}
}  // namespace

TEST(DiceExpression, NegativeOperandsKeepTheirBrackets) {
    // .r 5-(-1) once showed "5-(-1)=5--1=6".
    ASSERT_EQ(rendered("5-(-1)"), std::string("5-(-1)=6"));
    ASSERT_EQ(rendered("5+(-1)"), std::string("5+(-1)=4"));
    ASSERT_EQ(rendered("-(-1)"), std::string("-(-1)=1"));
    ASSERT_EQ(rendered("3*-1"), std::string("3*-1=3×(-1)=-3"));
    ASSERT_EQ(rendered("10/-2"), std::string("10/-2=10/(-2)=-5"));
    ASSERT_EQ(rendered("1d6-(-1d4)"), std::string("1D6-(-1D4)=4-(-4)=8"));
    ASSERT_EQ(rendered("5-(-1)*2"), std::string("5-(-1)*2=5-(-1×2)=5-(-2)=7"));
}

TEST(DiceExpression, ExpandedFormulaKeepsGroupingOfTheInput) {
    // Each expanded stage must read as the value it leads to.
    ASSERT_EQ(rendered("(1+2)*3"), std::string("(1+2)*3=(1+2)×3=9"));
    ASSERT_EQ(rendered("(1-5)*2"), std::string("(1-5)*2=(1-5)×2=-8"));
    ASSERT_EQ(rendered("8/(2*2)"), std::string("8/(2*2)=8/(2×2)=2"));
    // Integer division: 3×(5/2) = 6 but 3×5/2 = 7.
    ASSERT_EQ(rendered("3*(5/2)"), std::string("3*(5/2)=3×(5/2)=6"));
    ASSERT_EQ(rendered("-(1+2)"), std::string("-(1+2)=-3"));
    ASSERT_EQ(rendered("-(2*3)"), std::string("-(2*3)=-(2×3)=-6"));
    ASSERT_EQ(rendered("-3d6"), std::string("-3D6=-(4+4+4)=-12"));
    ASSERT_EQ(rendered("2*(1d6+1)"), std::string("2*(1D6+1)=2×(4+1)=10"));
    ASSERT_EQ(rendered("(2d10+5)*2"), std::string("(2D10+5)*2=((4+4)+5)×2=26"));
    ASSERT_EQ(rendered("-(1d6+1)*2"), std::string("-(1D6+1)*2=-(4+1)×2=-10"));
}

TEST(DiceExpression, ExactRegroupingStaysFlat) {
    ASSERT_EQ(rendered("3d6+2"), std::string("3D6+2=(4+4+4)+2=12+2=14"));
    ASSERT_EQ(rendered("10-(2+3)"), std::string("10-(2+3)=10-2-3=5"));
    ASSERT_EQ(rendered("10-(2-3)"), std::string("10-(2-3)=10-2+3=11"));
    ASSERT_EQ(rendered("1-(2*3)"), std::string("1-(2*3)=1-2×3=1-6=-5"));
    ASSERT_EQ(rendered("2*(3*4)"), std::string("2*(3*4)=2×3×4=24"));
    ASSERT_EQ(rendered("8/2*2"), std::string("8/2*2=8/2×2=8"));
    ASSERT_EQ(rendered("3d6"), std::string("3D6=4+4+4=12"));
}
