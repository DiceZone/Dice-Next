#include "test_framework.h"
#include "../src/core/legacy_command_arguments.h"

TEST(LegacyArguments, GrowthKeepsAttachedValuesAndIndependentSigns) {
    for (const auto* text : {"侦查60 -1/+1", "侦查 60 -1/+1", "侦查60-1/+1"}) {
        const auto result = dice::legacy_args::growth(text);
        ASSERT_TRUE(result.valid);
        ASSERT_EQ(result.attr, "侦查");
        ASSERT_EQ(result.value.value_or(-1), 60);
        ASSERT_EQ(result.failure, "-1");
        ASSERT_EQ(result.success, "+1");
    }
    const auto result = dice::legacy_args::growth("侦查0-1/1");
    ASSERT_EQ(result.failure, "-1");
    ASSERT_EQ(result.success, "1");
    ASSERT_EQ(dice::legacy_args::growth("侦查").success, "+1d10");
    ASSERT_FALSE(dice::legacy_args::growth("侦查1000").valid);
    ASSERT_FALSE(dice::legacy_args::growth("侦查0+1/").valid);
    ASSERT_FALSE(dice::legacy_args::growth("侦查0+/1").valid);
    ASSERT_FALSE(dice::legacy_args::growth("").valid);
}

TEST(LegacyArguments, InitiativeSeparatesExpressionCountAndName) {
    auto result = dice::legacy_args::initiative("+1d4 哥布林");
    ASSERT_EQ(result.expression, "1d20+1d4");
    ASSERT_EQ(result.name, "哥布林");
    result = dice::legacy_args::initiative("1d6哥布林");
    ASSERT_EQ(result.expression, "1d6");
    ASSERT_EQ(result.name, "哥布林");
    result = dice::legacy_args::initiative("1d6 3#哥布林");
    ASSERT_EQ(result.expression, "1d6");
    ASSERT_EQ(result.count, "3");
    ASSERT_TRUE(result.multiple);
    result = dice::legacy_args::initiative("1d4#哥布林");
    ASSERT_EQ(result.expression, "1d20");
    ASSERT_EQ(result.count, "1d4");
    result = dice::legacy_args::initiative("+2 3#");
    ASSERT_EQ(result.expression, "1d20+2");
    ASSERT_EQ(result.count, "3");
    result = dice::legacy_args::initiative("d+2");
    ASSERT_EQ(result.expression, "d20+2");
    result = dice::legacy_args::initiative("Dave");
    ASSERT_EQ(result.expression, "1d20");
    ASSERT_EQ(result.name, "Dave");
    result = dice::legacy_args::initiative("0#哥布林");
    ASSERT_EQ(result.count, "0");
    result = dice::legacy_args::initiative("1d0");
    ASSERT_EQ(result.expression, "1d0");
    result = dice::legacy_args::initiative("1d6foo");
    ASSERT_EQ(result.expression, "1d6");
    ASSERT_EQ(result.name, "foo");
    result = dice::legacy_args::initiative("2d(4+2) 哥布林");
    ASSERT_EQ(result.expression, "2d(4+2)");
    result = dice::legacy_args::initiative("d(4+2) 哥布林");
    ASSERT_EQ(result.expression, "d(4+2)");
    result = dice::legacy_args::initiative("d#哥布林");
    ASSERT_EQ(result.count, "d20");
    result = dice::legacy_args::initiative("42号敌人");
    ASSERT_EQ(result.expression, "1d20");
    ASSERT_EQ(result.name, "42号敌人");
    result = dice::legacy_args::initiative("42 敌人");
    ASSERT_EQ(result.expression, "42");
    ASSERT_EQ(result.name, "敌人");
}

TEST(LegacyArguments, GroupTermsRequireExplicitSignsAndNonEmptyNames) {
    for (const auto* text : {"+禁用回复 -禁用help", "+禁用回复-禁用help", "+ 禁用回复 - 禁用help"}) {
        auto terms = dice::legacy_args::groupTerms(text);
        ASSERT_TRUE(terms.has_value());
        ASSERT_EQ(terms->size(), size_t(2));
        ASSERT_EQ((*terms)[0].name, "禁用回复");
        ASSERT_TRUE((*terms)[0].on);
        ASSERT_EQ((*terms)[1].name, "禁用help");
        ASSERT_FALSE((*terms)[1].on);
    }
    ASSERT_FALSE(dice::legacy_args::groupTerms("+").has_value());
    ASSERT_FALSE(dice::legacy_args::groupTerms("禁用回复").has_value());
    ASSERT_FALSE(dice::legacy_args::groupTerms("+禁用回复 -").has_value());
}
