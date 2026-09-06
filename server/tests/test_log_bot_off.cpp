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

TEST(LogBotOff, ActiveTranscriptKeepsIncomingAndFinalReplies) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.msg.content = "关闭前";
    f.router.recordIncoming(f.msg);
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
    f.msg.content = "关闭后";
    f.router.recordIncoming(f.msg);
    f.router.recordBotReply(f.msg, "最终回复");
    auto rows = f.messages();
    ASSERT_EQ(rows.size(), size_t(3));
    ASSERT_EQ(rows[1].content, "关闭后");
    ASSERT_EQ(rows[2].content, "最终回复");
    ASSERT_EQ(rows[0].logId, rows[2].logId);
    ASSERT_EQ(rows[2].userId, "9000");
    ASSERT_EQ(f.logs().front().status, 0);
}

TEST(LogBotOff, ActiveLogManagementDoesNotWakeDiceCommands) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".log"), "log.status");
    ASSERT_EQ(f.run(".LOG on"), "log.already_on");
    ASSERT_EQ(f.run(".log new 另一个"), "log.exists");
    ASSERT_EQ(f.logs().size(), size_t(1));
    ASSERT_EQ(f.run(".r 1d1"), "");
    ASSERT_EQ(f.run(".logplugin"), "");
    ASSERT_EQ(f.run(".help"), "");
}

TEST(LogBotOff, PauseAndResumeAreIndependentOfBotOn) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".log off"), "log.off");
    ASSERT_EQ(f.logs().front().status, 1);
    f.msg.content = "暂停后不应记入";
    f.router.recordMessage(f.msg, "不应记入");
    ASSERT_TRUE(f.messages().empty());
    ASSERT_EQ(f.run(".log on"), "log.on");
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
    f.msg.content = "恢复记录";
    f.router.recordMessage(f.msg, "恢复回复");
    ASSERT_EQ(f.messages().size(), size_t(2));
}

TEST(LogBotOff, EndStopsRecordingUntilExplicitNewLog) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    // No recorded rows: shareLog returns before writing files or contacting a log site.
    ASSERT_EQ(f.run(".log end"), "log.ended");
    ASSERT_EQ(f.logs().front().status, 2);
    f.msg.content = "结束后";
    f.router.recordMessage(f.msg, "不应记录");
    ASSERT_TRUE(f.messages().empty());
    ASSERT_EQ(f.run(".log new 新记录"), "log.new");
    ASSERT_EQ(f.run(".log on"), "log.already_on");
    ASSERT_EQ(f.logs().size(), size_t(2));
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
}

TEST(LogBotOff, BotOffDoesNotStartRecordingUntilExplicitLogCommand) {
    LogFixture f;
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".log"), "log.idle");
    f.msg.content = "没有开启过记录";
    f.router.recordMessage(f.msg, "回复");
    ASSERT_TRUE(f.logs().empty());
    ASSERT_TRUE(f.messages().empty());
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.router.recordMessage(f.msg, "回复");
    ASSERT_EQ(f.messages().size(), size_t(2));
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
}

TEST(LogBotOff, HardLockStillBlocksCommandsAndDelayedReplies) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    f.setting("locked", "1");
    ASSERT_EQ(f.run(".log"), "");
    ASSERT_EQ(f.run(".log end"), "");
    f.msg.atList = {f.msg.selfId};
    ASSERT_EQ(f.run(".log off"), "");
    f.msg.content = "彻底禁用后的消息";
    f.router.recordMessage(f.msg, "禁用前已开始生成、禁用后才定稿的回复");
    ASSERT_TRUE(f.messages().empty());
    ASSERT_EQ(f.logs().front().status, 0);
}

TEST(LogBotOff, GroupsAndAdapterAccountsKeepSeparateActiveLogs) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    f.msg.adapterId = "other-account";
    f.setting("enabled", "0");
    ASSERT_EQ(f.run(".log"), "log.idle");
    f.router.recordMessage(f.msg, "不应串入日志");
    ASSERT_TRUE(f.messages().empty());
    f.msg.adapterId = "log-test";
    f.msg.targetId = "other-group";
    f.setting("enabled", "0");
    ASSERT_EQ(f.run(".log"), "log.idle");
    f.router.recordMessage(f.msg, "不应串群");
    ASSERT_TRUE(f.messages().empty());
    f.msg.targetId = "2000";
    ASSERT_EQ(f.run(".log"), "log.status");
    f.router.recordMessage(f.msg, "本群回复");
    ASSERT_EQ(f.messages().size(), size_t(2));
}

TEST(LogBotOff, ActiveLogAndBotOffSurviveDatabaseReopen) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    f.db.close();
    ASSERT_TRUE(f.db.open(u8str(f.dir.path / "test.db")));
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
    ASSERT_EQ(f.run(".log"), "log.status");
    f.msg.content = "重启后继续记录";
    f.router.recordMessage(f.msg, "回复");
    ASSERT_EQ(f.messages().size(), size_t(2));
}

TEST(LogBotOff, LogExceptionDoesNotBypassOtherSilenceModes) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    f.cfg.set<bool>("dice/silent_global", true);
    ASSERT_EQ(f.run(".log"), "");
    f.cfg.set<bool>("dice/silent_global", false);
    f.setting("externalMode", "1");
    ASSERT_EQ(f.run(".log"), "");
    f.setting("externalMode", "");
    ASSERT_EQ(f.run(".log"), "log.status");
}

TEST(LogBotOff, PrivateMessagesNeverEnterGroupTranscript) {
    LogFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    f.msg.type = MessageType::kPrivate;
    f.msg.content = "私聊内容";
    f.router.recordMessage(f.msg, "私聊回复");
    ASSERT_TRUE(f.messages().empty());
}

TEST(LogBotOff, LogOnCanStartFirstTranscriptWhileOff) {
    LogFixture f;
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".log on"), "log.new");
    ASSERT_EQ(f.logs().size(), size_t(1));
    ASSERT_EQ(f.run(".r 1d1"), "");
}

TEST(BotOffManagement, ReplySwitchKeepsItsOwnPermission) {
    LogFixture f;
    ASSERT_EQ(f.run(".bot off"), "bot.off");
    ASSERT_EQ(f.run(".reply"), "reply.usage");
    ASSERT_EQ(f.run(".reply off"), "reply.off");
    ASSERT_EQ(f.run(".REPLY ON"), "reply.on");
    ASSERT_EQ(f.run(".reply custom"), "");
    ASSERT_EQ(f.run(".replyplugin"), "");
    f.msg.extra["role"] = "member";
    ASSERT_EQ(f.run(".reply off"), "gate.no_perm");
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
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
