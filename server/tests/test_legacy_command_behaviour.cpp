#include "test_framework.h"
#include "core/command_router.h"
#include "core/plugin_command_priority.h"
#include "storage/group_account_settings.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct LegacyTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-legacy-command-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LegacyTestDir() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create test directory");
    }
    ~LegacyTestDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct LegacyFixture {
    LegacyTestDir dir; // Destroyed after the database, including all SQLite sidecars.
    Database db;
    ConfigManager cfg{u8str(dir.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db}; // No live adapters or network connections.
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;

    LegacyFixture() {
        if (!db.open(u8str(dir.path / "test.db"))) throw std::runtime_error("cannot open test database");
        cfg.resetDefault();
        msg.platform = "onebot_v11";
        msg.adapterId = "log-test";
        msg.selfId = "9000";
        msg.senderId = "1000";
        msg.senderName = "玩家";
        msg.targetId = "2000";
        msg.type = MessageType::kGroup;
        msg.extra = {{"role", "admin"}};
        setting("logTimerOff", "1");
    }
    void setting(const std::string& key, const std::string& value) {
        setAccountGroupSetting(*db.getStorage(), msg.adapterId, msg.platform,
            msg.targetId, msg.targetId, key, value);
    }
    std::string run(const std::string& content) {
        msg.content = content;
        return router.handleMessage(msg, Locale::kZhHans);
    }
    auto messages() {
        return db.getLogStorage()->get_all<GameLogMessageRow>(sqlite_orm::order_by(&GameLogMessageRow::id));
    }
    auto logs() { return db.getLogStorage()->get_all<GameLogRow>(); }
    void master() {
        cfg.set<json>("dice/masters", json::array({{
            {"platform", msg.platform}, {"adapter_id", msg.adapterId}, {"id", msg.senderId}
        }}));
    }
};

} // namespace

TEST(SampleTemplate, RollReplyUsesChosenTextAndActualResult) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    f.i18n.setOverride(Locale::kZhHans, "dice.roll.result", "{res} {sample:效果拔群|干得漂亮}");
    const auto reply = f.run(".r 1d1");
    ASSERT_TRUE(reply.find("1") != std::string::npos);
    ASSERT_TRUE(reply.find("效果拔群") != std::string::npos || reply.find("干得漂亮") != std::string::npos);
    ASSERT_TRUE(reply.find("sample:") == std::string::npos);
}

TEST(SampleTemplate, CustomReplyRetainsShorthandAndSelectedVariables) {
    LegacyFixture f;
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{user}|{user}}", "", MatchType::kKeyword), "1000");
    ASSERT_EQ(f.router.renderReply(f.msg, "{甲|甲}", "", MatchType::kKeyword), "甲");
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{roll:1d1}|{roll:1d1}}", "", MatchType::kKeyword), "1");
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{$1}}", "^(.+)$", MatchType::kRegex), "");
    f.msg.content = "{sample:不执行|不执行}";
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{$1}}", "^(.+)$", MatchType::kRegex), f.msg.content);
}

TEST(SampleTemplate, TextCommandSupportsNestedChoicesAndDice) {
    LegacyFixture f;
    ASSERT_EQ(f.run(".text {sample:{sample:{user}}}"), "1000");
    ASSERT_EQ(f.run(".text {sample:{1d1}|{1d1}}"), "1");
}

TEST(BotText, PersistsPerAccountAndConversationAndShowsUsageWithoutChangingState) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    ASSERT_TRUE(f.run(".bot text").find(".bot text plain") != std::string::npos);
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.run("。bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text invalid");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text rich 9999"); // Another bot's setting must stay untouched.
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    auto other = f.msg;
    other.adapterId = "other-bot";
    ASSERT_FALSE(f.router.conversationPlainText(other));
    other = f.msg; other.targetId = "other-group";
    ASSERT_FALSE(f.router.conversationPlainText(other));
    other = f.msg; other.type = MessageType::kPrivate;
    ASSERT_FALSE(f.router.conversationPlainText(other));
    f.msg.type = MessageType::kPrivate;
    f.run(".bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text rich");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.type = MessageType::kGroup;
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    CommandRouter reloaded(f.db, f.cfg, f.engine, f.i18n, f.resolver, f.cards, f.deck, f.adapters);
    ASSERT_TRUE(reloaded.conversationPlainText(f.msg));
    f.run(".bot off");
    f.run(".bot text rich");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
}

TEST(BotText, PermissionChecksIncludeOfficialGroupsWithoutTrustedRoles) {
    LegacyFixture f;
    f.msg.extra = {{"role", "member"}};
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.platform = "qq_official";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.master();
    f.msg.extra["__identity_native_sender"] = f.msg.senderId;
    f.run(".bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
}

TEST(BotText, CustomMarkdownTemplateDowngradesBeforeDiceValuesAreInserted) {
    LegacyFixture f;
    f.i18n.load();
    f.i18n.setOverride(Locale::kZhHans, "dice.roll.result", "**{nick}**：`{res}`", ContentFormat::kMarkdown);
    f.run(".bot text plain");
    I18n::beginOutboundCapture(f.router.conversationPlainText(f.msg)
        ? ContentFormat::kPlainText : ContentFormat::kMarkdown);
    const auto reply = f.run(".r 1d1");
    const auto format = I18n::endOutboundCapture();
    ASSERT_TRUE(reply.find("**") == std::string::npos);
    ASSERT_TRUE(reply.find('`') == std::string::npos);
    ASSERT_EQ(static_cast<int>(format), static_cast<int>(ContentFormat::kPlainText));
    f.run(".bot text rich");
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    ASSERT_TRUE(f.run(".r 1d1").find("**") != std::string::npos);
    ASSERT_EQ(static_cast<int>(I18n::endOutboundCapture()), static_cast<int>(ContentFormat::kMarkdown));
}

TEST(BotText, OfficialInviterUsesNativeIdentityAndOnlyControlsTheInvitedBotAndGroup) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.msg.extra = {{"__identity_native_sender", "inviter-openid"}};
    f.setting("inviter", "inviter-openid");
    f.run(".bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text rich");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));

    f.msg.extra["__identity_native_sender"] = "another-openid";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.senderId = "inviter-openid"; // A public ID must not substitute for the native identity.
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.extra.erase("__identity_native_sender");
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));

    f.msg.extra["__identity_native_sender"] = "inviter-openid";
    f.msg.adapterId = "another-bot";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.adapterId = "log-test";
    f.msg.targetId = "another-group";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
}


TEST(LegacyCommands, GrowthParsesCompactValuesAndWritesTheCorrectSign) {
    LegacyFixture f;
    f.cards.setAttr(f.msg.senderId, f.msg.targetId, "侦查", 60);
    ASSERT_EQ(f.run(".en侦查0-1/1"), "card.en.success");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-999), 1);
    ASSERT_EQ(f.run(".en 侦查 0 +1/-1"), "card.en.success");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-999), -1);
    ASSERT_EQ(f.run(".en侦查0+2"), "card.en.success");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-999), 2);
    ASSERT_EQ(f.run(".en侦查0+2d1k1"), "card.en.success");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-999), 1);
}

TEST(QQCharacterStatus, UsesStoredValuesAndActualAttributeChanges) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.cfg.set<bool>("dice/auto_card", false);
    f.run(".pc new 调查员");
    f.run(".st hp14/14 san65/99 mp12/12");
    auto reply = f.run(".st hp-2");
    auto card = f.router.lastOfficialCard();
    ASSERT_TRUE(card != nullptr);
    ASSERT_EQ(card->original, reply);
    ASSERT_EQ(card->vitals[0].current, 12);
    ASSERT_EQ(card->vitals[0].maximum.value_or(-1), 14);
    ASSERT_EQ(card->changes[0].before.value_or(-1), 14);
    ASSERT_EQ(card->changes[0].after, 12);
    const auto before = f.cards.getAttrs(f.msg.senderId, f.msg.targetId);
    reply = f.run(".pc status");
    ASSERT_TRUE(reply.find("12/14") != std::string::npos);
    ASSERT_TRUE(f.cards.getAttrs(f.msg.senderId, f.msg.targetId) == before);
    ASSERT_TRUE(f.router.lastOfficialCard()->statusOnly);
    f.run(".pc nonsense");
    ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
    f.msg.platform = "onebot_v11";
    ASSERT_TRUE(f.run(".pc status").find("12/14") != std::string::npos);
    ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
}

TEST(QQCharacterStatus, ShortcutsFollowConfiguredPrefixAndNeverIncludeUserText) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.cfg.set<bool>("dice/auto_card", false);
    f.run(".st hp10/10");
    f.cfg.set<json>("dice/command_prefixes", json::array({"!"}));
    f.run("!pc status");
    auto card = f.router.lastOfficialCard();
    ASSERT_TRUE(card != nullptr);
    ASSERT_EQ(card->commands[0].text, "!pc status");
    ASSERT_EQ(card->commands[2].text, "!st ");
    f.msg.atList = {"other-user"};
    ASSERT_EQ(f.run("!pc status"), "qq_rich.status_usage");
    ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
}

TEST(QQCharacterStatus, CompactionPreservesCustomRepliesAndOtherPlatforms) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    f.cfg.set<bool>("dice/auto_card", false);
    for (const auto* platform : {"qq_official", "onebot_v11", "milky", "discord", "kook"}) {
        f.msg.platform = platform;
        f.run(".st hp14/14");
        const auto reply = f.run(".st hp-2");
        ASSERT_EQ(reply, f.i18n.tr(Locale::kZhHans, "card.st.done", {{"nick", "<玩家>"}, {"detail", "生命值:12"}}));
        auto card = f.router.lastOfficialCard();
        if (f.msg.platform == "qq_official") {
            ASSERT_TRUE(card != nullptr);
            ASSERT_TRUE(card->body.has_value());
            ASSERT_TRUE(card->body->empty());
            ASSERT_EQ(card->original, reply);
        } else ASSERT_TRUE(card == nullptr);
    }
    f.msg.platform = "qq_official";
    f.i18n.setOverride(Locale::kZhHans, "card.st.done", "自定义回执：{detail}；请记入跑团笔记");
    const auto custom = f.run(".st hp+1");
    ASSERT_FALSE(f.router.lastOfficialCard()->body.has_value());
    ASSERT_TRUE(qq_rich::render(*f.router.lastOfficialCard(), true, false).find("请记入跑团笔记") != std::string::npos);
    ASSERT_EQ(f.router.lastOfficialCard()->original, custom);
}

TEST(LegacyCommands, InvalidGrowthNeverWritesTheCard) {
    LegacyFixture f;
    f.cards.setAttr(f.msg.senderId, f.msg.targetId, "侦查", 60);
    ASSERT_EQ(f.run(".en侦查0+invalid"), "dice.error.roll");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-999), 60);
    ASSERT_EQ(f.run(".en侦查1000+1"), "card.en.usage");
    ASSERT_EQ(f.run(".en侦查0+1/"), "card.en.usage");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-999), 60);
}

TEST(LegacyCommands, InitiativeUsesTheExpressionForEveryEntry) {
    LegacyFixture f;
    ASSERT_EQ(f.run(".ri 1d1+4 哥布林"), "init.rolled");
    auto read = [&] {
        return json::parse(f.router.getGroupSettingFor(f.msg.platform, f.msg.targetId, "init", f.msg.adapterId));
    };
    ASSERT_EQ(read()[0]["name"].get<std::string>(), "哥布林");
    ASSERT_EQ(read()[0]["val"].get<int>(), 5);
    ASSERT_EQ(f.run(".ri 2d1 3#敌人"), "init.rolled_multi");
    auto list = read();
    ASSERT_EQ(list.size(), size_t(4));
    for (size_t i = 1; i < list.size(); ++i) ASSERT_EQ(list[i]["val"].get<int>(), 2);
    ASSERT_EQ(f.run(".ri +1d1 援军"), "init.rolled");
    const int value = read().back()["val"].get<int>();
    ASSERT_TRUE(value >= 2 && value <= 21);
    const auto before = read().dump();
    ASSERT_EQ(f.run(".ri bad#错误"), "dice.error.roll");
    ASSERT_EQ(read().dump(), before);
    ASSERT_EQ(f.run(".ri 0#错误"), "init.count_err");
    ASSERT_EQ(read().dump(), before);
    ASSERT_EQ(f.run(".ri 11#错误"), "init.count_exceeded");
    ASSERT_EQ(read().dump(), before);
    ASSERT_EQ(f.run(".ri 1d0 错误"), "dice.error.roll");
    ASSERT_EQ(read().dump(), before);
    ASSERT_EQ(f.run(".ri 2d1k1 2d1k1#增援"), "init.rolled_multi");
    ASSERT_EQ(read().back()["name"].get<std::string>(), "增援1");
    ASSERT_EQ(read().back()["val"].get<int>(), 1);
}

TEST(LegacyCommands, ObserverHelpAndTyposNeverJoin) {
    LegacyFixture f;
    f.msg.extra["role"] = "member";
    ASSERT_EQ(f.run(".ob"), "help.topic.ob");
    ASSERT_EQ(f.run(".ob typo"), "help.topic.ob");
    ASSERT_EQ(f.run(".ob list"), "ob.empty");
    ASSERT_EQ(f.run(".ob join"), "ob.joined");
    ASSERT_EQ(f.run(".ob clr"), "gate.no_perm");
    ASSERT_EQ(f.run(".ob exit"), "ob.exit");
}

TEST(LegacyCommands, InitiativeTranslationDoesNotRepeatTheTotal) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    const auto output = f.run(".ri 1d1+4 哥布林");
    ASSERT_TRUE(output.find("哥布林") != std::string::npos);
    ASSERT_TRUE(output.find("=5") != std::string::npos);
    ASSERT_TRUE(output.find("=5=5") == std::string::npos);
    ASSERT_TRUE(output.find("{roll}") == std::string::npos);
}

TEST(LegacyCommands, ObserverGmCanClearOnlyTheirOwnSession) {
    LegacyFixture f;
    f.msg.extra["role"] = "member";
    f.router.setGameConf({
        [](const std::string& scope, const std::string& key) {
            return scope == "game:2000" && key == "__gms" ? std::string("[\"1000\"]") : std::string();
        }, {}, {}
    });
    ASSERT_EQ(f.run(".ob join"), "ob.joined");
    ASSERT_EQ(f.run(".ob clr"), "ob.cleared");
    ASSERT_EQ(f.run(".ob list"), "ob.empty");
    f.msg.targetId = "3000";
    ASSERT_EQ(f.run(".ob clr"), "gate.no_perm");
    ASSERT_EQ(f.run(".ob off"), "gate.no_perm");
}

TEST(LegacyCommands, GroupTermsValidateAllArgumentsBeforeWriting) {
    LegacyFixture f;
    ASSERT_EQ(f.run(".group +禁用回复 -禁用help"), "group.term_set\ngroup.term_cleared");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, f.msg.targetId, f.msg.adapterId));
    ASSERT_EQ(f.run(".group -禁用回复+未知词"), "group.term_unknown");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, f.msg.targetId, f.msg.adapterId));
    ASSERT_EQ(f.run(".group -禁用回复+禁用help"), "group.term_cleared\ngroup.term_set");
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, f.msg.targetId, f.msg.adapterId));
    ASSERT_EQ(f.run(".group nonsense"), "group.usage");
}

TEST(LegacyCommands, LocalGroupQueryDoesNotGrantWritePermission) {
    LegacyFixture f;
    f.msg.extra["role"] = "member";
    f.setting("replyDisabled", "1");
    ASSERT_EQ(f.run(".group"), "group.info");
    ASSERT_EQ(f.run(".group state"), "group.info");
    ASSERT_EQ(f.run(".group clr"), "gate.no_perm");
    ASSERT_EQ(f.run(".group -禁用回复"), "gate.no_perm");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, f.msg.targetId, f.msg.adapterId));
}

TEST(LegacyCommands, TargetedGroupNeverBorrowsTheSendingGroupRole) {
    LegacyFixture f;
    f.i18n.setOverride(Locale::kZhHans, "group.target_result", "[{group}] {result}");
    f.router.setGroupSettingFor(f.msg.platform, "3000", "name", "另一群", f.msg.adapterId);
    // Admin in 2000 is not proof of admin in 3000.
    ASSERT_EQ(f.run(".group 3000 +禁用回复"), "[3000] gate.no_perm");
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, "3000", f.msg.adapterId));
    f.master();
    ASSERT_EQ(f.run(".group 3000 +禁用回复"), "[3000] group.term_set");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, "3000", f.msg.adapterId));
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, "2000", f.msg.adapterId));
    f.router.setGroupSettingFor(f.msg.platform, "3000", "locked", "1", f.msg.adapterId);
    ASSERT_EQ(f.run(".group 3000 clr"), "[3000] gate.no_perm");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, "3000", f.msg.adapterId));
    ASSERT_EQ(f.run(".group 4000 +禁用回复"), "group.not_found");
}

TEST(LegacyCommands, GroupAllAndPrivateTargetAreAccountScoped) {
    LegacyFixture f;
    f.router.setGroupSettingFor(f.msg.platform, "3000", "name", "另一群", f.msg.adapterId);
    f.router.setGroupSettingFor(f.msg.platform, "4000", "name", "别的账号", "other-account");
    GroupSettingRow shared;
    shared.platform = f.msg.platform; shared.groupId = "4000";
    shared.key = "name"; shared.value = "别的账号的共享元数据";
    f.db.getStorage()->insert(shared);
    ASSERT_EQ(f.run(".group all +禁用回复"), "gate.no_perm");
    f.master();
    f.run(".group all +禁用回复");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, "2000", f.msg.adapterId));
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, "3000", f.msg.adapterId));
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, "4000", "other-account"));
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, "4000", f.msg.adapterId));
    ASSERT_EQ(f.run(".group 4000 +禁用回复"), "group.not_found");
    f.msg.type = MessageType::kPrivate;
    f.msg.targetId = f.msg.senderId;
    f.run(".group 3000 -禁用回复");
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, "3000", f.msg.adapterId));
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, "2000", f.msg.adapterId));
    ASSERT_EQ(f.run(".group all clr"), "group.usage");
}

TEST(LegacyCommands, OfficialLocalRoleFallbackDoesNotGrantRemoteAccess) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.msg.extra["role"] = "member";
    f.router.setGroupSettingFor(f.msg.platform, "3000", "name", "另一群", f.msg.adapterId);
    f.run(".group 3000 +禁用回复");
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, "3000", f.msg.adapterId));
}

TEST(LegacyCommands, GroupClearMasksLegacyWithoutTouchingOtherAccountsOrLogs) {
    LegacyFixture f;
    f.master();
    GroupSettingRow legacy;
    legacy.platform = f.msg.platform; legacy.groupId = f.msg.targetId;
    legacy.key = "replyDisabled"; legacy.value = "1";
    f.db.getStorage()->insert(legacy);
    legacy.key = "rollEnabled"; legacy.value = "0";
    f.db.getStorage()->insert(legacy);
    ASSERT_EQ(f.run(".log new 保留日志"), "log.new");
    f.setting("replyDisabled", "1");
    f.setting("name", "保留群名");
    f.setting("pluginEnabled", "0");
    f.setting("welcome", "旧欢迎");
    ASSERT_EQ(f.run(".group clr"), "group.cleared");
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "reply"));
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "roll"));
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "plugin"));
    ASSERT_TRUE(f.router.isLogRecording(f.msg));
    ASSERT_EQ(f.router.getGroupSettingFor(f.msg.platform, f.msg.targetId, "name", f.msg.adapterId), "保留群名");
    ASSERT_EQ(f.router.getGroupSettingFor(f.msg.platform, f.msg.targetId, "welcome", f.msg.adapterId), "");
    ASSERT_TRUE(f.router.isReplyDisabledFor(f.msg.platform, f.msg.targetId, "other-account"));
    f.db.close();
    ASSERT_TRUE(f.db.open(u8str(f.dir.path / "test.db")));
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "reply"));
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "roll"));
    ASSERT_TRUE(f.router.isLogRecording(f.msg));
}
