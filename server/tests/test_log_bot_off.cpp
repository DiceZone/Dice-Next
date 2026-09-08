#include "test_framework.h"
#include "core/command_router.h"
#include "core/plugin_command_priority.h"
#include "storage/group_account_settings.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct LogTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-log-off-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LogTestDir() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create test directory");
    }
    ~LogTestDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct LogFixture {
    LogTestDir dir; // Destroyed after the database, including all SQLite sidecars.
    Database db;
    ConfigManager cfg{u8str(dir.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{"unused-log-test-i18n"};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db}; // No live adapters or network connections.
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;

    LogFixture() {
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

TEST(LogBotOff, OverallOffPausesWithoutEndingAndWarnsOnce) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.msg.content = "关闭前";
    f.router.recordIncoming(f.msg);
    ASSERT_EQ(f.run(".bot off"), "bot.off\nbot.log_paused_notice");
    ASSERT_EQ(f.run(".bot off"), "bot.already_off");
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
    f.msg.content = "关闭后";
    f.router.recordMessage(f.msg, "延迟回复");
    ASSERT_EQ(f.messages().size(), size_t(1));
    ASSERT_EQ(f.logs().front().status, 0);
    ASSERT_EQ(f.run(".log off"), "");
    ASSERT_EQ(f.run(".reply on"), "");
    ASSERT_EQ(f.run(".bot on"), "bot.on");
    f.router.recordMessage(f.msg, "恢复回复");
    ASSERT_EQ(f.messages().size(), size_t(3));
    ASSERT_EQ(f.messages()[0].logId, f.messages()[2].logId);
}

TEST(LogBotOff, FeaturePauseRequiresBothSwitchesToResume) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot log off"), "bot.feature_off\nbot.log_paused_notice");
    ASSERT_EQ(f.run(".bot log off"), "bot.feature_already_off");
    ASSERT_EQ(f.run(".log end"), "");
    ASSERT_FALSE(f.router.isGroupDisabled(f.msg));
    ASSERT_FALSE(f.router.isLogRecording(f.msg));
    ASSERT_FALSE(f.run(".r 1d1").empty());
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".bot log on"), "bot.feature_on");
    ASSERT_FALSE(f.router.isLogRecording(f.msg));
    ASSERT_EQ(f.run(".bot on"), "bot.on");
    ASSERT_TRUE(f.router.isLogRecording(f.msg));
    ASSERT_EQ(f.run(".bot log off"), "bot.feature_off\nbot.log_paused_notice");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".bot on"), "bot.on");
    ASSERT_FALSE(f.router.isLogRecording(f.msg));
    ASSERT_EQ(f.run(".bot log on"), "bot.feature_on");
    ASSERT_TRUE(f.router.isLogRecording(f.msg));
    ASSERT_EQ(f.logs().size(), size_t(1));
}

TEST(LogBotOff, ExplicitSessionPauseIsNeverUndoneByFeatureSwitches) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".log off"), "log.off");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".bot log off"), "bot.feature_off");
    ASSERT_EQ(f.run(".bot on"), "bot.on");
    ASSERT_EQ(f.run(".bot log on"), "bot.feature_on");
    ASSERT_FALSE(f.router.isLogRecording(f.msg));
    ASSERT_EQ(f.logs().front().status, 1);
    ASSERT_EQ(f.run(".log on"), "log.on");
    ASSERT_TRUE(f.router.isLogRecording(f.msg));
}

TEST(LogBotOff, NoSessionMeansNoWarningOrImplicitCreation) {
    LogFixture f;
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".log on"), "");
    ASSERT_EQ(f.run(".bot log off"), "bot.feature_off");
    ASSERT_EQ(f.run(".bot log on"), "bot.feature_on");
    ASSERT_EQ(f.run(".bot on"), "bot.on");
    ASSERT_TRUE(f.logs().empty());
    ASSERT_FALSE(f.router.isLogRecording(f.msg));
}

TEST(GroupFeatures, DefaultsAndAccountIsolationPersistAcrossReopen) {
    LogFixture f;
    for (const auto* feature : {"log", "reply", "roll", "plugin"}) {
        ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, feature));
        ASSERT_EQ(f.run(std::string(".bot ") + feature + " off"), "bot.feature_off");
    }
    f.msg.adapterId = "other-account";
    for (const auto* feature : {"log", "reply", "roll", "plugin"})
        ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, feature));
    f.msg.adapterId = "log-test";
    f.msg.targetId = "other-group";
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "roll"));
    f.msg.targetId = "2000";
    f.db.close();
    ASSERT_TRUE(f.db.open(u8str(f.dir.path / "test.db")));
    for (const auto* feature : {"log", "reply", "roll", "plugin"})
        ASSERT_FALSE(f.router.groupFeatureEnabled(f.msg, feature));
}

TEST(GroupFeatures, PermissionsTargetingAndPrivateScope) {
    LogFixture f;
    f.msg.extra["role"] = "member";
    ASSERT_EQ(f.run(".bot log off"), "gate.no_perm");
    ASSERT_EQ(f.run(".bot log"), "bot.feature_state_on");
    f.msg.extra["role"] = "admin";
    ASSERT_EQ(f.run(".bot log off 1234"), "");
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "log"));
    ASSERT_EQ(f.run(".BOT LOG OFF 9000"), "bot.feature_off");
    ASSERT_FALSE(f.router.groupFeatureEnabled(f.msg, "log"));
    f.setting("locked", "1");
    ASSERT_EQ(f.run(".bot log on"), "");
    ASSERT_FALSE(f.router.groupFeatureEnabled(f.msg, "log"));
    f.msg.type = MessageType::kPrivate;
    ASSERT_EQ(f.run(".bot log"), "bot.feature_group_only");
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "log"));
}

TEST(GroupFeatures, ReplyUsesExistingPreferenceWithoutDisablingPlugins) {
    LogFixture f;
    ASSERT_EQ(f.run(".reply off"), "reply.off");
    ASSERT_FALSE(f.router.groupFeatureEnabled(f.msg, "reply"));
    ASSERT_TRUE(f.router.isPluginEnabledInGroup(f.msg.platform, f.msg.targetId, "js:test.js", f.msg.adapterId));
    ASSERT_EQ(f.run(".bot reply on"), "bot.feature_on");
    ASSERT_FALSE(f.router.isReplyDisabledFor(f.msg.platform, f.msg.targetId, f.msg.adapterId));
    ASSERT_EQ(f.run(".bot reply off"), "bot.feature_off");
    ASSERT_EQ(f.run(".reply on"), "reply.on");
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "reply"));
}

TEST(GroupFeatures, PluginMasterPreservesIndividualSelections) {
    LogFixture f;
    f.setting("pluginsOff", "js:disabled.js");
    ASSERT_EQ(f.run(".bot plugin off"), "bot.feature_off");
    ASSERT_FALSE(f.router.isPluginEnabledInGroup(f.msg.platform, f.msg.targetId, "js:enabled.js", f.msg.adapterId));
    ASSERT_TRUE(f.router.isPluginSelectedInGroup(f.msg.platform, f.msg.targetId, "js:enabled.js", f.msg.adapterId));
    ASSERT_TRUE(f.router.isPluginEnabledInGroup(f.msg.platform, f.msg.targetId, "js:enabled.js", "other-account"));
    ASSERT_EQ(f.run(".bot plugin on"), "bot.feature_on");
    ASSERT_TRUE(f.router.isPluginEnabledInGroup(f.msg.platform, f.msg.targetId, "js:enabled.js", f.msg.adapterId));
    ASSERT_FALSE(f.router.isPluginEnabledInGroup(f.msg.platform, f.msg.targetId, "js:disabled.js", f.msg.adapterId));
}

TEST(GroupFeatures, RollSwitchBlocksBuiltinsButNotCardManagement) {
    LogFixture f;
    ASSERT_EQ(f.run(".bot roll off"), "bot.feature_off");
    for (const auto* command : {".r 1d1", ".rh 1d1", ".ra 50", ".rc 50", ".rav 50 50",
            ".rb", ".rp", ".rx", ".ba 50", ".bav 50 50", ".sc 0/1", ".ww 3", ".dx 3",
            ".rdx 3", ".rdc 10", ".en 侦查", ".ri", ".coc", ".dnd"}) {
        ASSERT_EQ(f.run(command), "");
    }
    ASSERT_FALSE(f.run(".st hp:0 san:60").empty());
    ASSERT_EQ(f.run(".ds"), "");
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "reply"));
    ASSERT_TRUE(f.router.groupFeatureEnabled(f.msg, "plugin"));
    ASSERT_EQ(f.run(".bot roll on"), "bot.feature_on");
    ASSERT_FALSE(f.run(".r 1d1").empty());
}

TEST(LogBotOff, HardLockAndPrivateMessagesNeverRecord) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.setting("locked", "1");
    f.msg.atList = {f.msg.selfId};
    ASSERT_EQ(f.run(".bot log off"), "");
    f.router.recordMessage(f.msg, "不应记录");
    f.setting("locked", "0");
    f.msg.type = MessageType::kPrivate;
    f.router.recordMessage(f.msg, "私聊回复");
    ASSERT_TRUE(f.messages().empty());
}

TEST(LogBotOff, SharedWebControlStopsTimerAndReportsActualTransition) {
    LogFixture f;
    f.setting("logTimerOff", "");
    f.run(".log new 测试记录");
    const auto timer = "logTimerStart:" + std::to_string(f.logs().front().id);
    ASSERT_FALSE(f.router.getGroupSettingFor(f.msg.platform, f.msg.targetId, timer, f.msg.adapterId).empty());
    ASSERT_EQ(f.router.updateGroupControl(Locale::kZhHans, f.msg, "log", false), "bot.log_paused_notice");
    ASSERT_TRUE(f.router.getGroupSettingFor(f.msg.platform, f.msg.targetId, timer, f.msg.adapterId).empty());
    ASSERT_EQ(f.router.updateGroupControl(Locale::kZhHans, f.msg, "log", false), "");
    f.router.updateGroupControl(Locale::kZhHans, f.msg, "log", true);
    ASSERT_FALSE(f.router.getGroupSettingFor(f.msg.platform, f.msg.targetId, timer, f.msg.adapterId).empty());
}

TEST(BotOffManagement, MasterReusesAdminAndBlacklistHandlers) {
    LogFixture f;
    f.master();
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".master"), "master.usage");
    ASSERT_EQ(f.run(".master admin +1234"), "admin.added");
    ASSERT_EQ(f.run(".master admin"), "admin.list");
    ASSERT_EQ(f.run(".master admin -1234"), "admin.removed");
    ASSERT_EQ(f.run(".master admin -1234"), "admin.not_admin");
    ASSERT_EQ(f.run(".master clock"), "admin.clock_list");
    ASSERT_EQ(f.run(".master blackqq 1234"), "master.black_added_n");
    f.msg.senderId = "1234";
    ASSERT_TRUE(f.router.isBlocked(f.msg));
    f.msg.senderId = "1000";
    ASSERT_EQ(f.run(".master blackqq -1234"), "master.removed_n");
    f.msg.senderId = "1234";
    ASSERT_FALSE(f.router.isBlocked(f.msg));
}

TEST(BotOffManagement, RemoteSwitchTargetsOnlyTheSpecifiedGroupAndAccount) {
    LogFixture f;
    f.master();
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".master boton"), "master.need_group");
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
    ASSERT_EQ(f.run(".master botoff 3000"), "master.botoff");
    f.msg.targetId = "3000";
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
    f.msg.adapterId = "other-account";
    ASSERT_FALSE(f.router.isGroupDisabled(f.msg));
    f.msg.adapterId = "log-test";
    ASSERT_EQ(f.run(".master boton 3000"), "master.boton");
    ASSERT_FALSE(f.router.isGroupDisabled(f.msg));
    f.msg.targetId = "2000";
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
}

TEST(BotOffManagement, MasterIsNotAGenericCommandRunner) {
    LogFixture f;
    f.master();
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".master r 1d1"), "master.unsupported");
    ASSERT_EQ(f.run(".master delete"), "master.unsupported");
    ASSERT_EQ(f.run(".master reset 1234"), "master.unsupported");
    ASSERT_EQ(f.run(".master admin"), "admin.list");
    ASSERT_EQ(f.run(".masterplugin"), "");
    ASSERT_EQ(f.run(".admin list"), "");
    ASSERT_EQ(f.run(".help"), "");
    ASSERT_TRUE(plugin_command_priority::isReservedCoreCommand("MASTER"));
    ASSERT_FALSE(plugin_command_priority::isReservedCoreCommand("masterplugin"));
}

TEST(BotOffManagement, MasterRequiresMatchingOwnerAccount) {
    LogFixture f;
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".master admin"), "gate.not_master");
    f.master();
    ASSERT_EQ(f.run(".master admin"), "admin.list");
    f.msg.adapterId = "other-account";
    f.setting("enabled", "0");
    ASSERT_EQ(f.run(".master admin"), "gate.not_master");
}

TEST(BotOffManagement, OfficialMasterRequiresNativeIdentity) {
    LogFixture f;
    f.msg.platform = "qq_official";
    f.msg.extra["__identity_native_sender"] = "native-openid";
    f.setting("enabled", "0");
    f.master(); // Public sender ID is not sufficient in official group messages.
    ASSERT_EQ(f.run(".master admin"), "gate.not_master");
    f.cfg.set<json>("dice/masters", json::array({{
        {"platform", "qq_official"}, {"adapter_id", f.msg.adapterId}, {"id", "native-openid"}
    }}));
    ASSERT_EQ(f.run(".master admin"), "admin.list");
    f.msg.adapterId = "another-official-account";
    ASSERT_EQ(f.run(".master admin"), "gate.not_master");
}

TEST(BotOffManagement, HardLockRemainsAbsoluteForNewExceptions) {
    LogFixture f;
    f.master();
    f.setting("locked", "1");
    ASSERT_EQ(f.run(".master admin +1234"), "");
    ASSERT_EQ(f.run(".reply on"), "");
    ASSERT_EQ(f.run(".log new 测试"), "");
    ASSERT_EQ(f.run(".log on"), "");
    ASSERT_TRUE(f.logs().empty());
}

TEST(LogBotOff, BlacklistStillBlocksTheInboundPipeline) {
    LogFixture f;
    f.master();
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".master blackqq 1234"), "master.black_added_n");
    f.msg.senderId = "1234";
    f.msg.content = ".log new 禁止记录";
    ASSERT_TRUE(f.router.isBlocked(f.msg));
    // Same early gate as main.cpp: a blocked event never reaches commands/recording.
    if (!f.router.isBlocked(f.msg)) {
        f.router.handleMessage(f.msg);
        f.router.recordIncoming(f.msg);
    }
    ASSERT_TRUE(f.logs().empty());
    ASSERT_TRUE(f.messages().empty());
}
