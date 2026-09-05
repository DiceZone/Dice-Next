#include "test_framework.h"
#include "core/command_router.h"
#include "storage/group_account_settings.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

// Own all SQLite sidecar databases as well as the main database. Database::open
// with :memory: alone would still open cards.db/chat.db/logs.db in the cwd.
struct MeTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-me-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    MeTestDir() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create test directory");
    }
    ~MeTestDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Exercise the real router and capture remote sends without a network connection.
class MeAdapter final : public IAdapter {
public:
    std::vector<Message> sent;
    json members = json::array();
    std::string id() const override { return "me-test"; }
    std::string name() const override { return id(); }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "test"; }
    bool configure(const json&) override { return true; }
    bool start() override { return true; }
    void stop() override {}
    bool isConnected() const override { return true; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message& msg) override { sent.push_back(msg); }
    void sendReply(const Message&, const std::string&) override {}
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return "9000"; }
    std::string getLoginName() const override { return "骰娘"; }
    std::string getGroupName(const std::string&) const override { return "测试群"; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
    json getMembers(const std::string& group) const override {
        return group == "2000" ? members : json::array();
    }
};

struct MeFixture {
    MeTestDir dir;
    Database db;
    ConfigManager cfg{"unused-me-test-config"};
    DiceEngine engine{cfg};
    I18n i18n{"unused-me-test-i18n"};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    std::shared_ptr<MeAdapter> adapter = std::make_shared<MeAdapter>();
    Message msg;

    MeFixture() {
        if (!db.open(u8str(dir.path / "test.db"))) throw std::runtime_error("cannot open test database");
        cfg.resetDefault();
        adapters.registerAdapter(adapter);
        msg.platform = "onebot_v11";
        msg.adapterId = adapter->id();
        msg.selfId = adapter->getLoginId();
        msg.senderId = "1000";
        msg.senderName = "平台昵称";
        msg.targetId = "2000";
        msg.type = MessageType::kGroup;
        msg.extra = json::object();
    }

    std::string run(const std::string& command) {
        msg.content = command;
        return router.handleMessage(msg, Locale::kZhHans);
    }
    void trust(int level) {
        db.getStorage()->remove_all<PlayerProfileRow>();
        PlayerProfileRow row;
        row.platform = msg.platform;
        row.userId = msg.senderId;
        row.trustLevel = level;
        db.getStorage()->insert(row);
    }
    void nick(const std::string& scope, const std::string& name) {
        UserSettingRow row;
        row.userId = msg.senderId;
        row.groupId = scope;
        row.key = "nick";
        row.value = name;
        db.getStorage()->insert(row);
    }
    void makePrivate() {
        msg.type = MessageType::kPrivate;
        msg.targetId = msg.senderId;
    }
    void master() {
        cfg.set<json>("dice/masters", json::array({{
            {"platform", msg.platform}, {"adapter_id", msg.adapterId}, {"id", msg.senderId}
        }}));
    }
};

} // namespace

// Legacy reference: Dice-dev/Dice/DiceEvent.cpp, pref2 == "me".
// Group uses (trust > 4 ? "" : idx_pc); remote uses (trust > 4 ? getName : "").
TEST(MeCommand, GroupTrustBoundaryAndUnwrappedNames) {
    MeFixture f;
    f.cfg.set<std::string>("dice/nick_prefix", "【");
    f.cfg.set<std::string>("dice/nick_suffix", "】");
    for (int trust : {0, 4, 5}) {
        f.trust(trust);
        ASSERT_EQ(f.run(".me 测试"), trust > 4 ? "测试" : "平台昵称测试");
    }
}

TEST(MeCommand, GroupCharacterCardThenNickname) {
    MeFixture f;
    f.msg.extra["card"] = "群名片";
    ASSERT_EQ(f.run(".me测试"), "群名片测试");
    f.nick("", "全局昵称");
    ASSERT_EQ(f.run(".me 测试"), "全局昵称测试");
    f.nick("2000", "本群昵称");
    ASSERT_EQ(f.run(".me 测试"), "本群昵称测试");
    f.cards.bindCard("1000", "2000", "调查员");
    ASSERT_EQ(f.run(".me 测试"), "调查员测试");
    f.cards.bindCard("1000", "2000", "角色卡");
    ASSERT_EQ(f.run(".me 测试"), "本群昵称测试");
}

TEST(MeCommand, MasterAndSelfGroupOmitName) {
    MeFixture f;
    f.master();
    ASSERT_EQ(f.run(".me 测试"), "测试");
    f.cfg.set<json>("dice/masters", json::array());
    f.msg.senderId = f.msg.selfId;
    f.msg.fromSelf = true;
    ASSERT_EQ(f.run(".me 测试"), "测试");
}

TEST(MeCommand, RemoteTrustBoundaryUsesTargetGroupNicknameNotCharacterCard) {
    MeFixture f;
    f.nick("", "全局昵称");
    f.nick("2000", "目标群昵称");
    f.cards.bindCard("1000", "", "私聊人物卡");
    f.cards.bindCard("1000", "2000", "群人物卡");
    f.makePrivate();
    for (int trust : {0, 4, 5}) {
        f.trust(trust);
        ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
        ASSERT_EQ(f.adapter->sent.back().targetId, "2000");
        ASSERT_EQ(f.adapter->sent.back().content, trust > 4 ? "目标群昵称测试" : "测试");
    }
}

TEST(MeCommand, RemoteMasterUsesTargetGroupCardAndSelfUsesBotName) {
    MeFixture f;
    f.master();
    f.adapter->members = json::array({{{"user_id", 1000}, {"card", "目标群名片"}, {"nickname", "QQ昵称"}}});
    f.makePrivate();
    f.msg.extra["card"] = "不应沿用的名片";
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "目标群名片测试");
    f.msg.senderId = f.msg.selfId;
    f.msg.fromSelf = true;
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "骰娘测试");
}

TEST(MeCommand, RemoteSupportsCompactGroupAndRejectsEmptyInput) {
    MeFixture f;
    f.makePrivate();
    ASSERT_EQ(f.run(".me2000测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "测试");
    ASSERT_EQ(f.run(".me 2000测试"), "me.sent");
    ASSERT_EQ(f.run(".me"), "me.private_usage");
    ASSERT_EQ(f.run(".me invalid 测试"), "me.private_usage");
    ASSERT_EQ(f.run(".me 2000"), "me.empty");
    ASSERT_EQ(f.adapter->sent.size(), size_t(2));
}

TEST(MeCommand, GroupDisableBlocksEvenMasterButCanBeReenabled) {
    MeFixture f;
    f.master();
    ASSERT_EQ(f.run(".me off"), "me.off");
    ASSERT_EQ(f.run(".me 测试"), "me.disabled");
    ASSERT_EQ(f.run(".me on"), "me.on");
    ASSERT_EQ(f.run(".me 测试"), "测试");
}

TEST(MeCommand, RemoteDisableTrustBoundary) {
    MeFixture f;
    f.msg.extra["role"] = "admin";
    ASSERT_EQ(f.run(".me off"), "me.off");
    f.makePrivate();
    for (int trust : {0, 4}) {
        f.trust(trust);
        ASSERT_EQ(f.run(".me 2000 测试"), "me.disabled");
        ASSERT_TRUE(f.adapter->sent.empty());
    }
    f.trust(5);
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
}

TEST(MeCommand, GlobalDisableAppliesBeforeToggleAndTrustFourBypasses) {
    MeFixture f;
    f.cfg.set<bool>("dice/disabled_me", true);
    f.msg.extra["role"] = "admin";
    ASSERT_EQ(f.run(".me on"), "gate.me_global");
    ASSERT_EQ(f.run(".me 测试"), "gate.me_global");
    f.trust(4);
    ASSERT_EQ(f.run(".me 测试"), "平台昵称测试");
    f.makePrivate();
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "测试");
}

TEST(MeCommand, OfficialGroupDoesNotTrustUnverifiedPublicIdentity) {
    MeFixture f;
    f.msg.platform = "qq_official";
    f.trust(5);
    ASSERT_EQ(f.run(".me 测试"), "平台昵称测试");
}

TEST(MeCommand, RemoteExternalModeRespectsTrustAndAdapterScope) {
    MeFixture f;
    setAccountGroupSetting(*f.db.getStorage(), "me-test", "onebot_v11", "2000", "2000", "externalMode", "1");
    f.makePrivate();
    ASSERT_EQ(f.run(".me 2000 测试"), "me.disabled");
    ASSERT_TRUE(f.adapter->sent.empty());
    f.trust(5);
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    f.trust(0);
    f.msg.adapterId = "another-account";
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "测试");
}

TEST(MeCommand, RemoteNicknameFallbackAndNativeMemberId) {
    MeFixture f;
    f.trust(5);
    f.makePrivate();
    f.msg.extra["card"] = "私聊缓存名片";
    f.msg.extra["__identity_native_sender"] = "native-user";
    f.adapter->members = json::array({{{"user_id", "native-user"}, {"card", "目标群名片"}, {"nickname", "QQ昵称"}}});
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "目标群名片测试");
    f.adapter->members[0]["card"] = "";
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "QQ昵称测试");
    f.adapter->members = json::array();
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "平台昵称测试");
    f.nick("", "全局昵称");
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "全局昵称测试");
}

TEST(MeCommand, GroupTogglePermissionEmptyActionAndPluginPrefix) {
    MeFixture f;
    ASSERT_EQ(f.run(".me on"), "gate.no_perm");
    ASSERT_EQ(f.run(".me off"), "gate.no_perm");
    ASSERT_EQ(f.run(".me"), "me.empty");
    ASSERT_EQ(f.run(".menu"), "");
    ASSERT_EQ(f.run(".memory 测试"), "");
    f.trust(4);
    ASSERT_EQ(f.run(".me off"), "me.off");
    ASSERT_EQ(f.run(".me 测试"), "me.disabled");
    f.trust(5);
    ASSERT_EQ(f.run(".me 测试"), "me.disabled");
    ASSERT_EQ(f.run(".me on"), "me.on");
}

TEST(MeCommand, OfficialMasterRequiresNativeIdentityAndMatchingAdapter) {
    MeFixture f;
    f.msg.platform = "qq_official";
    f.msg.extra["__identity_native_sender"] = "native-openid";
    f.cfg.set<json>("dice/masters", json::array({{
        {"platform", "qq_official"}, {"adapter_id", f.msg.adapterId}, {"id", "native-openid"}
    }}));
    ASSERT_EQ(f.run(".me 测试"), "测试");
    f.msg.adapterId = "another-official-account";
    ASSERT_EQ(f.run(".me 测试"), "平台昵称测试");
}

TEST(MeCommand, LegacyStringMasterConfigurationStillIdentifiesOwner) {
    MeFixture f;
    f.cfg.set<json>("dice/masters", json::array({"1000"}));
    ASSERT_EQ(f.run(".me 测试"), "测试");
    f.makePrivate();
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "平台昵称测试");
    f.msg.senderId = "another-user";
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "测试");
}

TEST(MeCommand, AliasInheritsMainAccountTrustWithoutRenamingSpeaker) {
    MeFixture f;
    f.trust(5);
    f.cfg.set<json>("dice/aliases", json::array({{
        {"platform", "onebot_v11"}, {"alias", "1001"}, {"main", "1000"}
    }}));
    f.msg.senderId = "1001";
    f.msg.senderName = "别名账号";
    ASSERT_EQ(f.run(".me 测试"), "测试");
    f.makePrivate();
    ASSERT_EQ(f.run(".me 2000 测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().content, "别名账号测试");
}

TEST(MeCommand, RemoteLeadingZerosCannotBypassTargetGroupDisable) {
    MeFixture f;
    f.msg.extra["role"] = "admin";
    ASSERT_EQ(f.run(".me off"), "me.off");
    f.makePrivate();
    ASSERT_EQ(f.run(".me 0002000测试"), "me.disabled");
    ASSERT_TRUE(f.adapter->sent.empty());
    f.trust(5);
    ASSERT_EQ(f.run(".me 0002000测试"), "me.sent");
    ASSERT_EQ(f.adapter->sent.back().targetId, "2000");
}
