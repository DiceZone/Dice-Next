#include "test_framework.h"
#include "src/common/reply_preview.h"
using namespace dice;

TEST(outbound, qq_group_and_private_markdown_channel_plain_and_rejection_fallback) {
    for (const auto* platform : {"qq_group", "qq_private"}) {
        const auto preview = outbound::replyPreview("**成功** `1d6=3`", ContentFormat::kMarkdown, platform, PresentationStyle::kStandard);
        EXPECT_EQ(preview["payload"]["msg_type"].get<int>(), 2);
        EXPECT_EQ(preview["payload"]["markdown"]["content"].get<std::string>(), "**成功** `1d6=3`");
    }
    const auto channel = outbound::replyPreview("**成功** `1d6=3`", ContentFormat::kMarkdown, "qq_channel", PresentationStyle::kVisual);
    EXPECT_FALSE(channel["payload"].contains("msg_type"));
    EXPECT_FALSE(channel["payload"].contains("markdown"));
    EXPECT_EQ(channel["payload"]["content"].get<std::string>(), "成功 1d6=3");
    const auto fallback = outbound::qqText("**成功**", ContentFormat::kMarkdown, false, false);
    EXPECT_EQ(fallback["msg_type"].get<int>(), 0);
    EXPECT_EQ(fallback["content"].get<std::string>(), "成功");
}

TEST(outbound, qq_v2_native_markdown_media_and_everyone_tags_remain_intact) {
    const std::string markdown = "<qqbot-at-everyone />\n![封面 #200px #100px](qqbot-media://image/v1/token)";
    const auto body = outbound::qqText(markdown, ContentFormat::kMarkdown, true, false);
    EXPECT_FALSE(body.contains("content")); // v2 forbids content alongside Markdown.
    EXPECT_EQ(body["markdown"]["content"].get<std::string>(), markdown);
    const auto plain = outbound::qqText("<qqbot-at-everyone />通知", ContentFormat::kPlainText, false, false);
    EXPECT_EQ(plain["content"].get<std::string>(), "<qqbot-at-everyone />通知");
}

TEST(outbound, official_qq_name_card_prefixes_markdown_plain_and_rich_cards) {
    const auto markdown = outbound::qqText("D100=50", ContentFormat::kPlainText, true, false,
        nullptr, {}, "off", "off", "", false, 0, "茶叶 HP10/10");
    EXPECT_EQ(markdown["markdown"]["content"].get<std::string>(),
        "$\\scriptsize\\textcolor{#E5484D}{\\text{茶叶 HP10/10}}$\n\nD100=50");
    const auto plain = outbound::qqText("D100=50", ContentFormat::kPlainText, false, false,
        nullptr, {}, "off", "off", "", false, 0, "茶叶 HP10/10");
    EXPECT_EQ(plain["content"].get<std::string>(), "茶叶 HP10/10\nD100=50");
    const auto escaped = outbound::qqText("x", ContentFormat::kPlainText, true, false,
        nullptr, {}, "off", "off", "", false, 0, "A<B>&C");
    EXPECT_TRUE(escaped["markdown"]["content"].get<std::string>().find("A\\textless{}B\\textgreater{}\\&C") != std::string::npos);
    qq_rich::Card card;
    card.original = "hello";
    card.title = "状态";
    card.statusOnly = true;
    const auto rich = outbound::qqText("hello", ContentFormat::kPlainText, true, false,
        &card, {}, "markdown", "off", "", false, 0, "茶叶");
    EXPECT_EQ(rich["markdown"]["content"].get<std::string>().rfind(
        "$\\scriptsize\\textcolor{#E5484D}{\\text{茶叶}}$\n\n", 0), size_t(0));
}

TEST(outbound, official_sn_presets_custom_templates_and_literal_fallback) {
    const std::string card = "希亚 SAN50 HP0/0 DEX0";
    EXPECT_EQ(qq_sn::prefix(card, true, "quote"), "> " + card + "\n\n");
    EXPECT_EQ(qq_sn::prefix(card, true, "plain"), card + "\n\n");
    EXPECT_EQ(qq_sn::prefix(card, true, "custom", 0, "**{sn}**"), "**" + card + "**\n\n");
    EXPECT_EQ(qq_sn::prefix(card, true, "custom", 0, "$\\text{{sn_tex}}$"), "$\\text{" + card + "}$\n\n");
    EXPECT_EQ(qq_sn::renderTemplate("{sn}", "{sn_tex}"), "{sn\\_tex}");
    for (const auto* mode : {"small_red", "quote", "plain", "custom"}) {
        EXPECT_EQ(qq_sn::prefix(card, false, mode), card + "\n");
        EXPECT_EQ(qq_sn::prefix(card, true, mode, 1, "**{sn}**"), card + "\n\n");
    }
    EXPECT_TRUE(qq_sn::prefix(card, true, "off").empty());
    EXPECT_TRUE(qq_sn::prefix(card, false, "off").empty());
    EXPECT_EQ(qq_sn::prefix(card, true, "small_red", 0, "> {sn}", "#3b82f6"),
        "$\\scriptsize\\textcolor{#3B82F6}{\\text{" + qq_sn::texText(card) + "}}$\n\n");
    for (const auto* bad : {"red", "#12345", "#1234567", "#GGGGGG", "#12}\\x", ""})
        EXPECT_EQ(qq_sn::prefix(card, true, "small_red", 0, "> {sn}", bad).rfind("$\\scriptsize\\textcolor{#E5484D}", 0), size_t(0));
    EXPECT_TRUE(qq_sn::validColor("#00ff7F"));
    EXPECT_FALSE(qq_sn::validColor("00ff7f"));
    EXPECT_FALSE(qq_sn::validStyle("<font>"));
    EXPECT_FALSE(qq_sn::validTemplate("missing placeholder"));
    EXPECT_FALSE(qq_sn::validTemplate(std::string(2045, 'x') + "{sn}"));
    EXPECT_TRUE(qq_sn::validTemplate(std::string(2044, 'x') + "{sn}"));
    EXPECT_EQ(qq_sn::renderTemplate("bad persisted config", card), card);
    std::string repeated;
    for (int i = 0; i < 100; ++i) repeated += "{sn}";
    const std::string longName(120, 'a');
    EXPECT_EQ(qq_sn::renderTemplate(repeated, longName), longName);
    const auto richFallback = outbound::qqText("**成功**", ContentFormat::kMarkdown, true, false,
        nullptr, {}, "off", "off", "", false, 1, card, "custom", "$\\text{{sn_tex}}$");
    EXPECT_EQ(richFallback["markdown"]["content"].get<std::string>(), card + "\n\n**成功**");
}

TEST(outbound, official_sn_tex_keeps_names_literal_and_cannot_inject_markup) {
    const std::string name = "希亚😀 }$\\href{evil}{x}%#&_~^<qqbot-cmd>[CQ:image,file=x]`\n\t";
    const auto encoded = qq_sn::texText(name);
    EXPECT_TRUE(encoded.find("希亚😀") != std::string::npos);
    EXPECT_TRUE(encoded.find("$\\href") == std::string::npos);
    EXPECT_TRUE(encoded.find("\\href") == std::string::npos);
    EXPECT_TRUE(encoded.find("<qqbot") == std::string::npos);
    EXPECT_TRUE(encoded.find("[CQ:") == std::string::npos);
    EXPECT_TRUE(encoded.find("\n") == std::string::npos);
    EXPECT_TRUE(encoded.find("\\textdollar{}\\textbackslash{}href\\textbraceleft{}") != std::string::npos);
    EXPECT_TRUE(encoded.find("\\%\\#\\&\\_\\textasciitilde{}\\textasciicircum{}") != std::string::npos);
    EXPECT_EQ(qq_sn::prefix(name, false), name + "\n");
    EXPECT_EQ(qq_sn::prefix("**名字**", true, "plain"), "\\*\\*名字\\*\\*\n\n");
}

TEST(outbound, kook_card_and_downgrade_use_production_serializer) {
    auto body = outbound::kookText("target", "**成功**", ContentFormat::kMarkdown, true);
    EXPECT_EQ(body["type"].get<int>(), 10);
    const auto card = outbound::Json::parse(body["content"].get<std::string>());
    EXPECT_EQ(card[0]["modules"][0]["text"]["type"].get<std::string>(), "kmarkdown");
    EXPECT_EQ(card[0]["modules"][0]["text"]["content"].get<std::string>(), "**成功**");
    body = outbound::kookText("target", "**成功**", ContentFormat::kMarkdown, false);
    EXPECT_EQ(body["type"].get<int>(), 1);
    EXPECT_EQ(body["content"].get<std::string>(), "成功");
    EXPECT_EQ(outbound::kookText("target", std::string(5001, 'a'), ContentFormat::kMarkdown, true)["type"].get<int>(), 1);
}

TEST(outbound, discord_plain_is_literal_even_when_discord_parses_markdown) {
    const auto rich = outbound::discordText("**成功**", ContentFormat::kMarkdown, true);
    EXPECT_EQ(outbound::discordPayload(rich, true)["embeds"][0]["description"].get<std::string>(), "**成功**");
    const auto plain = outbound::discordText("**结果** `2*3=6 _名字_`", ContentFormat::kMarkdown, false);
    EXPECT_EQ(markdown::toPlainText(plain), "结果 2*3=6 _名字_");
    EXPECT_EQ(outbound::discordText("**原文**", ContentFormat::kPlainText, true), "\\*\\*原文\\*\\*");
    EXPECT_EQ(outbound::discordPayload(std::string(4097, 'a'), true)["content"].get<std::string>().size(), size_t(4097));
}

TEST(outbound, preview_profiles_and_conversation_plain_do_not_change_user_data) {
    const std::string text = "**HP** [[bar:HP|6|10]]\n[[action:掷骰|.r]]";
    const auto visual = outbound::replyPreview(text, ContentFormat::kMarkdown, "qq_group", PresentationStyle::kVisual);
    EXPECT_TRUE(visual["payload"].contains("keyboard"));
    EXPECT_FALSE(visual["actions"].empty());
    const auto forced = outbound::replyPreview(text, ContentFormat::kMarkdown, "qq_group", PresentationStyle::kVisual, true);
    EXPECT_EQ(forced["payload"]["msg_type"].get<int>(), 0);
    EXPECT_FALSE(forced["payload"].contains("keyboard"));
    EXPECT_TRUE(forced["text"].get<std::string>().find("6/10") != std::string::npos);
    EXPECT_TRUE(outbound::replyPreview("**原文**", ContentFormat::kPlainText, "plain", PresentationStyle::kVisual)["text"] == "**原文**");
}
