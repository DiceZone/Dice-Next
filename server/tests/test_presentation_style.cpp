#include "test_framework.h"
#include "common/presentation_style.h"
#include "common/qq_rich_reply.h"
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

TEST(PresentationStyle, BarPlaceholdersStayInsideTheComponentUntilLookedUp) {
    const std::string input = "[[bar:HP|{hp}|{hpmax}]] {hp}";
    ASSERT_TRUE(presentation::variableOnlyInBar(input, "hpmax"));
    ASSERT_FALSE(presentation::variableOnlyInBar(input, "hp"));
    const auto filled = presentation::resolveBarPlaceholders(input, [](std::string_view name) -> std::optional<int> {
        if (name == "hp") return 8;
        if (name == "hpmax") return 12;
        return std::nullopt;
    });
    ASSERT_EQ(filled, std::string("[[bar:HP|8|12]] {hp}"));
    ASSERT_EQ(presentation::expandComponents(filled, PresentationStyle::kTraditional,
        ContentFormat::kPlainText).text, std::string("HP 8/12 {hp}"));
    ASSERT_EQ(presentation::resolveBarPlaceholders("[[bar:HP|{missing}|10]]",
        [](std::string_view) { return std::optional<int>{}; }), std::string("[[bar:HP|{missing}|10]]"));
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

TEST(PresentationStyle, ActionArgumentsStayRawWhileMessageTextIsEscaped) {
    I18n i18n((std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n").string());
    ASSERT_TRUE(i18n.load());
    for (const auto format : {ContentFormat::kMarkdown, ContentFormat::kPlainText}) {
        I18n::beginOutboundCapture(format, PresentationStyle::kVisual);
        const auto reply = i18n.tr(Locale::kZhHans, "dnd.rc.result",
            {{"nick", "玩家_1"}, {"attr", "力量-20"}, {"command", ".rc 力量-20"},
             {"roll", "1d20=10"}, {"mod", "+0"}, {"total", "10"}});
        I18n::endOutboundCapture();
        const auto expanded = presentation::expandComponents(reply, PresentationStyle::kVisual, format);
        ASSERT_EQ(expanded.actions.size(), size_t(1));
        ASSERT_EQ(expanded.actions[0].text, ".rc 力量-20");
        const auto keyboard = qq_rich::keyboard(qq_rich::mergeCommands(nullptr, expanded.actions), "user");
        ASSERT_EQ(keyboard["content"]["rows"][0]["buttons"][0]["action"]["data"].get<std::string>(), ".rc 力量-20");
    }
    const std::string hostile = "a_1-5\\x|%20]] [[action:extra|.master exit]]";
    const auto rendered = I18n::previewTemplate("**{name}** [[action:抽取|.draw {name}]]",
        {{"name", hostile}}, ContentFormat::kMarkdown);
    const auto expanded = presentation::expandComponents(rendered, PresentationStyle::kStandard, ContentFormat::kMarkdown);
    ASSERT_EQ(expanded.actions.size(), size_t(1));
    ASSERT_EQ(expanded.actions[0].text, ".draw " + hostile);
    ASSERT_TRUE(rendered.find("a\\_1\\-5") != std::string::npos);
}

TEST(PresentationStyle, FinalReplyPreservesExactCommandsAndOnlyUpdatesCaptions) {
    for (const std::string prefix : {"/", "。", "custom:"}) {
        const std::string command = prefix + "rc 优势力量-5 [推开门]";
        const auto rendered = I18n::previewTemplate("[[action:再次检定|{command}]]",
            {{"command", command}}, ContentFormat::kMarkdown);
        auto expanded = presentation::expandComponents(rendered, PresentationStyle::kVisual, ContentFormat::kMarkdown);
        ASSERT_EQ(expanded.actions.size(), size_t(1));
        presentation::refreshActionLabels(command + " // Recheck\n.master exit // invented", expanded.actions);
        ASSERT_EQ(expanded.actions.size(), size_t(1));
        ASSERT_EQ(expanded.actions[0].text, command);
    }
    std::vector<presentation::Action> actions{{"再次检定", "/rc 力量-5 "}};
    presentation::refreshActionLabels("/rc 力量-5 // Recheck", actions);
    ASSERT_EQ(actions[0].text, "/rc 力量-5 ");
    ASSERT_EQ(actions[0].label, "Recheck");
    ASSERT_TRUE(ai::preservesActionCommands("/rc 力量-5  // 检定", "/rc 力量-5  // Check"));
    ASSERT_FALSE(ai::preservesActionCommands("/rc 力量-5  // 检定", "/rc 力量+5  // Check"));
}

TEST(PresentationStyle, EditedVisualPresetRetainsButtonsForGlobalAndPersonaOverrides) {
    I18n i18n((std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n").string());
    ASSERT_TRUE(i18n.load());
    const auto presets = i18n.defaultTemplates(Locale::kZhHans, "dice.check.result");
    auto edited = presets["visual"]["value"].get<std::string>();
    ASSERT_TRUE(edited.find("[[action:") != std::string::npos);
    edited = "我的检定：" + edited;
    const I18n::Args args{{"nick", "测试"}, {"attr", "侦查"}, {"roll", "42"}, {"rate", "60"}, {"level", "成功"}, {"command", ".ra侦查"}};
    for (const bool persona : {false, true}) {
        if (persona) i18n.setPersonaBundles(9, Locale::kZhHans, {{"dice.check.result", edited}}, {{"dice.check.result", "markdown"}});
        else i18n.setOverride(Locale::kZhHans, "dice.check.result", edited, ContentFormat::kMarkdown);
        const I18n::PersonaScope scope(persona ? 9 : 0);
        I18n::beginOutboundCapture(ContentFormat::kMarkdown, PresentationStyle::kVisual);
        const auto rendered = i18n.tr(Locale::kZhHans, "dice.check.result", args);
        I18n::endOutboundCapture();
        const auto expanded = presentation::expandComponents(rendered, PresentationStyle::kVisual, ContentFormat::kMarkdown);
        ASSERT_TRUE(expanded.text.find("我的检定") != std::string::npos);
        ASSERT_EQ(expanded.actions.size(), size_t(1));
        ASSERT_EQ(expanded.actions[0].text, ".ra侦查");
    }
    i18n.clearOverride(Locale::kZhHans, "dice.check.result");
    ASSERT_EQ(i18n.getDefault(Locale::kZhHans, "dice.check.result", PresentationStyle::kVisual), presets["visual"]["value"].get<std::string>());
}

TEST(PresentationStyle, AuthoredFormulasSupportSafeVariablesAndPlainFallback) {
    ASSERT_EQ(presentation::expandMathVariables(R"(\frac{{{roll}}}{2})", {{"roll", "42"}}), R"(\frac{42}{2})");
    const std::string source = R"([[math:\textcolor{#E5484D}{\frac{ {{roll}} }{ {{rate}} }}|{roll}/{rate}]])";
    for (const auto format : {ContentFormat::kPlainText, ContentFormat::kMarkdown}) {
        const auto rendered = I18n::previewTemplate(source, {{"roll", "42"}, {"rate", "60"}}, format);
        const auto rich = presentation::expandComponents(rendered, PresentationStyle::kVisual, ContentFormat::kMarkdown);
        ASSERT_EQ(rich.text, R"($\textcolor{#E5484D}{\frac{ 42 }{ 60 }}$)");
        ASSERT_EQ(presentation::expandComponents(rendered, PresentationStyle::kVisual, ContentFormat::kPlainText).text, "42/60");
        ASSERT_EQ(presentation::expandComponents(rendered, PresentationStyle::kTraditional, ContentFormat::kMarkdown).text, "42/60");
        ASSERT_EQ(presentation::expandComponents(rendered, PresentationStyle::kVisual, ContentFormat::kMarkdown, false).text, "42/60");
    }
    const auto hostile = I18n::previewTemplate(R"([[math:{{nick}}|{nick}]])", {{"nick", R"(}\href{javascript:bad}{bad}$]])"}}, ContentFormat::kMarkdown);
    const auto result = presentation::expandComponents(hostile, PresentationStyle::kVisual, ContentFormat::kMarkdown);
    ASSERT_TRUE(result.text.find("\\textbackslash{}href") != std::string::npos);
    ASSERT_TRUE(result.text.find("\\$") != std::string::npos);
    ASSERT_TRUE(result.actions.empty());
}
