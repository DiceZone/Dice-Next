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
