#include "test_framework.h"
#include "src/common/markdown.h"

using dice::markdown::hasFormatting;
using dice::markdown::toPlainText;
using dice::markdown::escapeLiteral;

TEST(markdown, strips_inline_formatting_and_keeps_destinations) {
    const std::string input =
        "**Alice** rolled `1D100=42` with *luck* and ~~old text~~. "
        "[Help](https://dice.zone/help).";
    EXPECT_EQ(toPlainText(input),
              "Alice rolled 1D100=42 with luck and old text. "
              "Help (https://dice.zone/help).");
}

TEST(markdown, converts_blocks_without_losing_content) {
    const std::string input =
        "# Result\n"
        "> **Success**\n"
        "- first\n"
        "- [x] second\n"
        "```text\n"
        "1d6 * 2 and **literal stars**\n"
        "```";
    EXPECT_EQ(toPlainText(input),
              "Result\n"
              "Success\n"
              "- first\n"
              "[x] second\n"
              "1d6 * 2 and **literal stars**");
}

TEST(markdown, preserves_onebot_codes_and_dice_expressions) {
    const std::string input =
        "**Result** [CQ:at,qq=123456] [CQ:image,file=https://img.test/a_b.png] "
        "1d6*2+1d4*3 foo_bar_baz";
    EXPECT_EQ(toPlainText(input),
              "Result [CQ:at,qq=123456] [CQ:image,file=https://img.test/a_b.png] "
              "1d6*2+1d4*3 foo_bar_baz");
}

TEST(markdown, preserves_escaped_and_inline_code_content) {
    EXPECT_EQ(toPlainText(R"(\*not italic\* and \_literal\_ and `2*3*4_and_more`)"),
              "*not italic* and _literal_ and 2*3*4_and_more");
}

TEST(markdown, mirrors_commonmark_code_span_padding) {
    EXPECT_EQ(toPlainText("`` `edge` ``"), "`edge`");
    EXPECT_EQ(toPlainText("`  kept  `"), " kept ");
}

TEST(markdown, detects_common_formatting) {
    EXPECT_TRUE(hasFormatting("**bold**"));
    EXPECT_TRUE(hasFormatting("# heading"));
    EXPECT_FALSE(hasFormatting("1d6*2+1 snake_case"));
}

TEST(markdown, downgraded_default_roll_has_no_markdown_markers) {
    const std::string formatted =
        "**Alice** makes a **Spot Hidden** check: `42/60` **Success**\n"
        "> Result: `1D100=42`";
    const std::string plain = toPlainText(formatted);
    EXPECT_EQ(plain,
              "Alice makes a Spot Hidden check: 42/60 Success\n"
              "Result: 1D100=42");
    EXPECT_FALSE(hasFormatting(plain));
}

TEST(markdown, removes_empty_and_segmented_strong_markers) {
    EXPECT_EQ(toPlainText("**** 掷骰：`1D100=42`"),
              " 掷骰：1D100=42");
    EXPECT_EQ(toPlainText("**这是被分段截断的粗体内容"),
              "这是被分段截断的粗体内容");
    EXPECT_EQ(toPlainText("这是被分段截断的粗体内容**：`1D100=42`"),
              "这是被分段截断的粗体内容：1D100=42");
}

TEST(markdown, keeps_expression_operators_and_identifiers) {
    EXPECT_EQ(toPlainText("2**3 + 1||0"), "2**3 + 1||0");
    EXPECT_EQ(toPlainText("foo__bar"), "foo__bar");
    EXPECT_EQ(toPlainText("**结果**：`2**3=8`"), "结果：2**3=8");
}

TEST(markdown, orphan_cleanup_is_idempotent) {
    const std::string once = toPlainText("**** **Alice** **unfinished");
    EXPECT_EQ(toPlainText(once), once);
    EXPECT_FALSE(hasFormatting(once));
}

TEST(markdown, explicit_plain_text_can_be_embedded_without_becoming_formatting) {
    const std::string literal = "**不是粗体**\n# 1号方案\n> 10 [CQ:image,file=a.png]";
    const std::string escaped = escapeLiteral(literal);
    EXPECT_NE(escaped, literal);
    EXPECT_EQ(toPlainText(escaped), literal);
    EXPECT_TRUE(escaped.find("[CQ:image,file=a.png]") != std::string::npos);
}
