#include "test_framework.h"
#include "src/common/markdown.h"
#include "src/common/reply_content.h"

using dice::markdown::hasFormatting;
using dice::markdown::toPlainText;
using dice::markdown::escapeLiteral;
using dice::markdown::escapeQQMarkdownLiteral;

TEST(markdown, ai_structure_allows_words_but_not_layout_changes) {
    using dice::markdown::preservesStructure;
    EXPECT_TRUE(preservesStructure("# Result\n**Success**", "# 结果\n**成功**"));
    EXPECT_FALSE(preservesStructure("# Result\n**Success**", "结果\n成功"));
    EXPECT_FALSE(preservesStructure("# Result", "## Result"));
    EXPECT_TRUE(preservesStructure("- **Success**\n- Fail", "- **成功**\n- 失败"));
    EXPECT_FALSE(preservesStructure("- Success\n- Fail", "Success\nFail"));
    EXPECT_TRUE(preservesStructure("**Success**", "描述\n\n**成功**\n\n后记"));
    EXPECT_FALSE(preservesStructure("- [x] done", "- [ ] done"));
}

TEST(markdown, ai_structure_preserves_commands_math_and_media_targets) {
    using dice::markdown::preservesStructure;
    EXPECT_TRUE(preservesStructure("**roll** `1d6=3`", "**掷骰** `1d6=3`"));
    EXPECT_FALSE(preservesStructure("`1d6=3`", "`2d6=3`"));
    EXPECT_FALSE(preservesStructure("```lua\na=1\n```", "```lua\na=2\n```"));
    EXPECT_TRUE(preservesStructure("[Help](https://dice.zone/help)", "[帮助](https://dice.zone/help)"));
    EXPECT_FALSE(preservesStructure("[Help](https://dice.zone/help)", "[帮助](https://evil.example/help)"));
    EXPECT_FALSE(preservesStructure("![图](https://dice.zone/a.png)", "![图](https://dice.zone/b.png)"));
    EXPECT_FALSE(preservesStructure("$\\color{red}{HP}$", "$\\color{green}{HP}$"));
    EXPECT_FALSE(preservesStructure("[CQ:at,qq=123] **Hi**", "[CQ:at,qq=321] **Hi**"));
    EXPECT_TRUE(preservesStructure("2*3 and 3*4", "2*3 与 3*4"));
}

TEST(markdown, mixed_reply_fragments_keep_plugin_text_literal) {
    using dice::ContentFormat;
    const auto combined = dice::joinReplyContent({{"**插件原文** [CQ:at,qq=123]", ContentFormat::kPlainText},
        {"**成功** `1d6=3`", ContentFormat::kMarkdown}, {"2*3=6 _原文_", ContentFormat::kPlainText}});
    EXPECT_TRUE(combined.format == ContentFormat::kMarkdown);
    EXPECT_EQ(toPlainText(combined.text), "**插件原文** [CQ:at,qq=123]\n成功 1d6=3\n2*3=6 _原文_");
    const auto plain = dice::joinReplyContent({{"**原文**", ContentFormat::kPlainText}, {"", ContentFormat::kMarkdown},
        {"2*3", ContentFormat::kPlainText}});
    EXPECT_TRUE(plain.format == ContentFormat::kPlainText);
    EXPECT_EQ(plain.text, "**原文**\n2*3");
}

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
    const std::string literal = "**不是粗体**\n# 1号方案\n> 10 [CQ:image,file=a.png]\n$HP$";
    const std::string escaped = escapeLiteral(literal);
    EXPECT_NE(escaped, literal);
    EXPECT_EQ(toPlainText(escaped), literal);
    EXPECT_TRUE(escaped.find("[CQ:image,file=a.png]") != std::string::npos);
    EXPECT_TRUE(escaped.find("\\$HP\\$") != std::string::npos);
}

TEST(markdown, structured_renderer_handles_lists_links_code_and_entities) {
    const std::string formatted =
        "## Overview\n\n"
        "1. **First**\n"
        "2. [Help](https://dice.zone/help)\n\n"
        "\x60**literal code**\x60 &amp;";
    EXPECT_EQ(toPlainText(formatted),
              "Overview\n"
              "1. First\n"
              "2. Help (https://dice.zone/help)\n"
              "**literal code** &");
}

TEST(markdown, qq_plain_banner_does_not_turn_parentheses_into_math) {
    const std::string banner =
        "Dice!Next By DiceZone/Shia Ver 3.0.0(858)\n"
        "[GNUC 13.3.0 2026-08-22 16:24:54 For Adapter/qq_official]\n"
        "OneDice V1 Compatible";
    const std::string safe = escapeQQMarkdownLiteral(banner);
    EXPECT_EQ(safe, banner);
    EXPECT_TRUE(safe.find("\\(") == std::string::npos);
    EXPECT_TRUE(safe.find("\\[") == std::string::npos);
}

TEST(markdown, qq_plain_markdown_syntax_is_preserved_as_literal_text) {
    const std::string literal =
        "**不是粗体**\n"
        "# 1号方案\n"
        "[文档](https://dice.zone/help)\n"
        "1. 项目\n"
        "qq_official 1d6*2";
    const std::string safe = escapeQQMarkdownLiteral(literal);
    EXPECT_NE(safe, literal);
    EXPECT_EQ(toPlainText(safe), literal);
    EXPECT_TRUE(safe.find("\\(") == std::string::npos);
    EXPECT_TRUE(safe.find("\\[") == std::string::npos);
}
