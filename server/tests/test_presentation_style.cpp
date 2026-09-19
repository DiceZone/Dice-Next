#include "test_framework.h"
#include "common/presentation_style.h"
#include "i18n/i18n.h"
#include "service/ai_gateway.h"
#include <filesystem>

using namespace dice;

TEST(PresentationStyle, ParsesProfilesAndLegacyCardAlias) {
    ASSERT_TRUE(presentationStyleFromString("traditional") == PresentationStyle::kTraditional);
    ASSERT_TRUE(presentationStyleFromString("standard") == PresentationStyle::kStandard);
    ASSERT_TRUE(presentationStyleFromString("card") == PresentationStyle::kStandard);
    ASSERT_TRUE(presentationStyleFromString("visual") == PresentationStyle::kVisual);
}

TEST(PresentationStyle, RendersSemanticComponentsForEveryProfile) {
    const std::string input = "[[bar:HP|6|10]]\n[[action:掷骰指令|.r]]";
    const auto traditional = presentation::expandComponents(
        input, PresentationStyle::kTraditional, ContentFormat::kPlainText);
    ASSERT_EQ(traditional.text, std::string("HP 6/10\n掷骰指令：.r"));
    ASSERT_FALSE(traditional.usedMarkdown);
    ASSERT_EQ(traditional.actions.size(), static_cast<size_t>(1));
    ASSERT_EQ(traditional.actions[0].text, std::string(".r"));

    const auto standard = presentation::expandComponents(
        input, PresentationStyle::kStandard, ContentFormat::kMarkdown);
    ASSERT_EQ(standard.text, std::string("**HP** 6/10\n**掷骰指令**：`.r`"));
    ASSERT_TRUE(standard.usedMarkdown);

    const auto visual = presentation::expandComponents(
        input, PresentationStyle::kVisual, ContentFormat::kPlainText);
    ASSERT_EQ(visual.text, std::string("HP [██████░░░░] 6/10\n.r  // 掷骰指令"));
    ASSERT_FALSE(visual.usedMarkdown);
    ASSERT_EQ(visual.actions.size(), static_cast<size_t>(1));
}

TEST(PresentationStyle, TurnsVisualUsageLinesIntoPrefillActions) {
    const std::string input =
        "多角色卡 .pc\n"
        ".pc new <名字> // 新建人物卡\n"
        ".bot on/off [骰子QQ] // 本群总开关\n"
        ".log type txt/html // 设定群文件格式";
    const auto visual = presentation::expandComponents(
        input, PresentationStyle::kVisual, ContentFormat::kMarkdown);
    ASSERT_EQ(visual.text, input);
    ASSERT_EQ(visual.actions.size(), static_cast<size_t>(5));
    ASSERT_EQ(visual.actions[0].text, std::string(".pc new "));
    ASSERT_EQ(visual.actions[1].text, std::string(".bot on "));
    ASSERT_EQ(visual.actions[2].text, std::string(".bot off "));
    ASSERT_EQ(visual.actions[3].text, std::string(".log type txt"));
    ASSERT_EQ(visual.actions[4].text, std::string(".log type html"));
    ASSERT_TRUE(visual.actions[1].label.find("on") != std::string::npos);

    const auto standard = presentation::expandComponents(
        input, PresentationStyle::kStandard, ContentFormat::kMarkdown);
    ASSERT_TRUE(standard.actions.empty());
}

TEST(PresentationStyle, LeavesMalformedOrUnknownComponentsIntact) {
    const std::string input = "[[bar:HP|oops|10]] [[future:value]] [[unterminated";
    ASSERT_EQ(presentation::expandComponents(input, PresentationStyle::kVisual,
        ContentFormat::kPlainText).text, input);
}

TEST(PresentationStyle, AiMayTranslateLabelsButMustKeepActionCommands) {
    const std::string original = ".ra 侦查 60  // 再次检定";
    ASSERT_TRUE(ai::preservesActionCommands(original, ".ra 侦查 60  // Check again"));
    ASSERT_FALSE(ai::preservesActionCommands(original, ".ra Spot 60  // Check again"));
}

TEST(PresentationStyle, I18nSelectsPartialProfileOverlays) {
    const auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n";
    I18n i18n(dir.string(), Locale::kZhHans);
    ASSERT_TRUE(i18n.load());

    I18n::beginOutboundCapture(ContentFormat::kMarkdown, PresentationStyle::kTraditional);
    const auto traditional = i18n.tr(Locale::kZhHans, "dice.roll.result",
        {{"nick", "希亚"}, {"res", "D100=42"}});
    I18n::endOutboundCapture();
    ASSERT_EQ(traditional, std::string("希亚掷骰: D100=42"));

    I18n::beginOutboundCapture(ContentFormat::kMarkdown, PresentationStyle::kStandard);
    const auto standard = i18n.tr(Locale::kZhHans, "dice.roll.result",
        {{"nick", "希亚"}, {"res", "D100=42"}});
    I18n::endOutboundCapture();
    ASSERT_EQ(standard, std::string("**希亚** 掷骰：`D100=42`"));

    I18n::beginOutboundCapture(ContentFormat::kMarkdown, PresentationStyle::kVisual);
    const auto visualTemplate = i18n.tr(Locale::kZhHans, "dice.roll.result",
        {{"nick", "希亚"}, {"res", "D100=42"}});
    I18n::endOutboundCapture();
    const auto visual = presentation::expandComponents(
        visualTemplate, PresentationStyle::kVisual, ContentFormat::kMarkdown).text;
    ASSERT_TRUE(visual.find("🎲 **希亚**") != std::string::npos);
    ASSERT_TRUE(visual.find(".r  // 再次掷骰") != std::string::npos);
}
