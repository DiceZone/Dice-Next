#include "test_framework.h"
#include "legacy_import_test_support.h"
#include "common/message_break.h"
#include "common/template_preview.h"
#include "common/legacy_reply_references.h"
#include "core/command_router.h"
#include "storage/legacy_import_v2.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace dice;
struct MessageBreakDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("dicenext-message-break-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    MessageBreakDirectory() {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("cannot create message-break fixture");
    }
    ~MessageBreakDirectory() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
struct MessageBreakFixture {
    MessageBreakDirectory dir;
    Database db;
    ConfigManager cfg{u8str(dir.root / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck decks;
    AdapterManager adapters{db}; // Never starts an adapter or a listener.
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, decks, adapters};
    Message msg;
    MessageBreakFixture() {
        if (!db.open(u8str(dir.root / "app.db"))) throw std::runtime_error("cannot open message-break database");
        cfg.resetDefault();
        msg.platform = "onebot_v11"; msg.adapterId = "test-account"; msg.selfId = "bot";
        msg.targetId = "group"; msg.senderId = "player"; msg.senderName = "测试玩家";
        msg.type = MessageType::kGroup; msg.extra = {{"role", "admin"}};
    }
};
} // namespace

TEST(MessageBreak, AuthoredConstantIsCaseInsensitiveLazyAndNotAnUnknownVariable) {
    for (const auto* token : {"FormFeed", "formfeed", "FORMFEED"}) {
        const auto program = safe_template::compile("甲{" + std::string(token) + "}乙");
        ASSERT_TRUE(program.variables.empty()); ASSERT_TRUE(program.issues.empty());
        ASSERT_EQ(I18n::previewTemplate("甲{" + std::string(token) + "}乙"), "甲\f乙");
    }
    ASSERT_EQ(I18n::previewTemplate("{sample:甲{FormFeed}乙}"), "甲\f乙");
    ASSERT_EQ(I18n::previewTemplate("{case:kind?1=甲{FormFeed}乙&else=无}", {{"kind", "1"}}), "甲\f乙");
    ASSERT_EQ(I18n::previewTemplate("{case:kind?1=甲{FormFeed}乙&else=无}", {{"kind", "0"}}), "无");
    ASSERT_EQ(I18n::previewTemplate(R"(甲\{FormFeed}乙)"), R"(甲\{FormFeed}乙)");
    ASSERT_EQ(I18n::previewTemplate("{FormFeeds}"), "{FormFeeds}");
    ASSERT_EQ(I18n::previewTemplate("{sample:未闭合{FormFeed}"), "{sample:未闭合{FormFeed}");
}

TEST(MessageBreak, I18nReferencesPersonasAndCachedPlainPreserveExplicitBoundaries) {
    I18n texts("unused-test-directory");
    texts.setOverride(Locale::kZhHans, "test.page", "**甲**{FormFeed}**{nick}**", ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kPlainText);
    const auto plain = texts.tr(Locale::kZhHans, "test.page", {{"nick", "{FormFeed}"}});
    I18n::endOutboundCapture();
    ASSERT_EQ(plain, "甲\f{FormFeed}");
    texts.setOverride(Locale::kZhHans, "test.ref", "前{text:test.page}后");
    ASSERT_EQ(texts.tr(Locale::kZhHans, "test.ref", {{"nick", "乙"}}), "前甲\f乙后");
    texts.setPersonaBundles(5, Locale::kZhHans, {{"test.persona", "人格甲{FORMFEED}人格乙"}});
    const auto persona = texts.scopedPersona(5);
    ASSERT_EQ(texts.tr(Locale::kZhHans, "test.persona"), "人格甲\f人格乙");
}

TEST(MessageBreak, PreviewSerializesEachPageBeforeMarkdownDowngrade) {
    for (const char* platform : {"qq_group", "qq_channel", "discord", "kook", "plain"}) {
        const auto preview = outbound::templatePreview({{"text", "**甲**{FormFeed}**乙**"},
            {"format", "markdown"}, {"platform", platform}}).at("preview");
        ASSERT_EQ(preview.at("messages").size(), size_t(2));
        ASSERT_EQ(preview.at("messages").at(0).at("plain"), "甲");
        ASSERT_EQ(preview.at("messages").at(1).at("plain"), "乙");
        ASSERT_EQ(preview.at("plain"), "甲\f乙");
        for (const auto& page : preview.at("messages")) ASSERT_FALSE(page.dump().find("\\f") != std::string::npos);
    }
    const auto empty = outbound::templatePreview({{"text", "{FormFeed}{FormFeed}"}});
    ASSERT_TRUE(empty.at("preview").at("messages").empty());
}

TEST(MessageBreak, ForcedPagesIgnoreAutomaticSplitSwitchAndPlatformExemptions) {
    MessageBreakFixture f;
    for (const bool enabled : {false, true}) {
        f.cfg.set<bool>("dice/reply_segment_enabled", enabled);
        for (const char* platform : {"onebot_v11", "milky", "qq_official", "discord", "kook"}) {
            const auto pages = f.router.segmentReply("第一条\f第二条", platform);
            ASSERT_EQ(pages.size(), size_t(2)); ASSERT_EQ(pages.at(0), "第一条"); ASSERT_EQ(pages.at(1), "第二条");
        }
    }
    ASSERT_EQ(message_break::split("\f甲\f\f \t\r\n\f乙\f").size(), size_t(2));
    ASSERT_TRUE(message_break::split("\f\f\f").empty());
    ASSERT_EQ(message_break::split("  甲\n\f乙  ").at(0), "  甲\n");
    ASSERT_EQ(message_break::split("  甲\n\f乙  ").at(1), "乙  ");
    f.cfg.set<int>("dice/reply_segment_len", 100);
    ASSERT_EQ(f.router.segmentReply(std::string(250, 'a') + "\fb", "onebot_v11").size(), size_t(4));
    ASSERT_EQ(f.router.segmentReply(std::string(250, 'a') + "\fb", "qq_official").size(), size_t(2));
    f.cfg.set<bool>("dice/reply_segment_enabled", false);
    ASSERT_EQ(f.router.segmentReply(std::string(250, 'a') + "\fb", "onebot_v11").size(), size_t(2));
}

TEST(MessageBreak, CustomRepliesResolveAuthoredControlsButNeverReparseCapturesOrNames) {
    MessageBreakFixture f;
    ASSERT_EQ(f.router.renderReplyWithGroups(f.msg, "甲{FormFeed}乙", {}), "甲\f乙");
    ASSERT_EQ(f.router.renderReplyWithGroups(f.msg, R"(甲\{FormFeed}乙)", {}), R"(甲\{FormFeed}乙)");
    ASSERT_EQ(f.router.renderReplyWithGroups(f.msg, "{$1}", {"whole", "甲{FormFeed}乙"}), "甲{FormFeed}乙");
    f.msg.senderName = "甲{FormFeed}乙";
    ASSERT_EQ(f.router.renderReplyWithGroups(f.msg, "{qqnick}", {}), "甲{FormFeed}乙");
}

TEST(MessageBreak, DeckEntriesAndNestedDecksHonorOnlyUnescapedControlTokens) {
    MessageBreakDirectory dir;
    { std::ofstream out(dir.root / "deck.json"); out << R"({"page":["甲{FormFeed}乙"],"nested":["{page}{formfeed}丙"],"escaped":["甲\\{FormFeed}乙"]})"; }
    CardDeck decks; ASSERT_TRUE(decks.loadDir(u8str(dir.root)) >= 3);
    ASSERT_EQ(decks.drawFromDeck("page").value_or("missing"), "甲\f乙");
    ASSERT_EQ(decks.drawFromDeck("nested").value_or("missing"), "甲\f乙\f丙");
    ASSERT_EQ(decks.drawFromDeck("escaped").value_or("missing"), "甲{FormFeed}乙");
    { std::ofstream out(dir.root / "named.json"); out << R"({"FormFeed":["同名牌堆"]})"; }
    ASSERT_TRUE(decks.loadDir(u8str(dir.root)) >= 4);
    CardDeck::ReferenceContext references;
    ASSERT_EQ(decks.expandReference("FormFeed", references), "同名牌堆");
}

TEST(MessageBreak, BotMsgCanBreakAfterTheUneditableBannerWithoutReparsingUserData) {
    MessageBreakFixture f; f.msg.content = ".bot";
    f.cfg.set<std::string>("dice/bot_msg", "{FormFeed}附加说明");
    const auto reply = f.router.handleMessage(f.msg, Locale::kZhHans);
    const auto pages = f.router.segmentReply(reply, f.msg.platform);
    ASSERT_EQ(pages.size(), size_t(2));
    ASSERT_EQ(pages.at(0), botBanner(f.msg.platform) + "\n"); ASSERT_EQ(pages.at(1), "附加说明");
}

TEST(MessageBreak, MigrationTreatsFormFeedAsABuiltinRatherThanAMissingDeckOrVariable) {
    const auto detail = legacyv2::checkTextDetail("strRollRegularSuccess", "dice.compat.check.single.regular", "甲{FormFeed}乙");
    ASSERT_EQ(detail.at("status"), "active"); ASSERT_TRUE(detail.at("issues").empty());
    legacy_reply_references::Catalog catalog; legacy_reply_references::Report report; bool compatibility = false;
    ASSERT_EQ(legacy_reply_references::migrate("甲{FormFeed}乙{FORMFEED}丙", catalog, report, "rule", "reply", compatibility),
        "甲{FormFeed}乙{FORMFEED}丙");
    ASSERT_EQ(report.unresolved, 0); ASSERT_EQ(report.converted, 0); ASSERT_FALSE(compatibility);
}
