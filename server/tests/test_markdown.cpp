#include "test_framework.h"
#include "src/common/markdown.h"

using dice::markdown::hasFormatting;
using dice::markdown::toPlainText;

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
