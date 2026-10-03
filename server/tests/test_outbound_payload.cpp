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
