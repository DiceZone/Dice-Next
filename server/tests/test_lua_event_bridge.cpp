#include "test_framework.h"
#include "service/lua_event_bridge.h"
#include "adapter/onebot_account_identity.h"
#include "storage/group_account_settings.h"
#include <filesystem>

using namespace dice;
namespace {
class LuaEventAdapter final : public IAdapter {
public:
    std::string account = "lua-event-a", self = "900001", role = "admin";
    bool online = true, protectedUser = false, supportsBan = true;
    int roleRefreshes = 0;
    std::vector<Message> messages;
    struct Ban { std::string group, user; int seconds; };
    std::vector<Ban> bans;
    std::string id() const override { return account; }
    std::string name() const override { return account; }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "offline-test"; }
    bool configure(const json&) override { return true; }
    bool start() override { throw std::runtime_error("tests must never start an adapter"); }
    void stop() override {}
    bool isConnected() const override { return online; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message& message) override { messages.push_back(message); }
    void sendReply(const Message& source, const std::string& text) override { auto copy = source; copy.content = text; messages.push_back(copy); }
    void sendPrivateMessage(const std::string& target, const std::string& text) override {
        Message message; message.type = MessageType::kPrivate; message.targetId = target; message.content = text; messages.push_back(message);
    }
    void sendGroupMessage(const std::string& target, const std::string& text) override {
        Message message; message.type = MessageType::kGroup; message.targetId = target; message.content = text; messages.push_back(message);
    }
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return self; }
    std::string getLoginName() const override { return "离线骰娘"; }
    std::string getGroupName(const std::string&) const override { return "离线群"; }
    std::vector<std::pair<std::string, std::string>> getGroupList() const override { return {{"1108650419", "离线群"}}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string& user) const override { return user != self && protectedUser; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override { throw std::runtime_error("unexpected kick"); }
    void setGroupBan(const std::string& group, const std::string& user, int seconds) override { bans.push_back({group, user, seconds}); }
    json capabilities() const override { return {{"ban", supportsBan}}; }
    std::string getSelfRole(const std::string&) const override { return role; }
    void refreshSelfRole(const std::string&) override { ++roleRefreshes; }
};
struct LuaEventFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("dicenext-lua-event-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Database db;
    ConfigManager cfg{u8str(root / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg}; CharacterCardStore cards{db}; CardDeck decks; AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, decks, adapters};
    std::shared_ptr<LuaEventAdapter> a = std::make_shared<LuaEventAdapter>();
    LuaEventOrigin origin{"onebot_v11", "lua-event-a", "1108650419", "10001", "lua:roulette", false};
    int fallbacks = 0;
    LuaEventFixture() {
        if (!std::filesystem::create_directory(root) || !db.open(u8str(root / "app.db"))) throw std::runtime_error("cannot create Lua event fixture");
        cfg.resetDefault(); if (!i18n.load()) throw std::runtime_error("cannot load translations");
        adapters.registerAdapter(a); setting("__presence", "1");
    }
    ~LuaEventFixture() { db.close(); }
    void setting(const std::string& key, const std::string& value) {
        setAccountGroupSetting(*db.getStorage(), a->id(), a->platform(), origin.groupId, origin.groupId, key, value);
    }
    void run(const std::string& text = ".group 1108650419 ban 10001 1", const std::string& group = "", const std::string& user = "900001") {
        auto fallback = [&](const Message& message, std::string&, std::vector<ProcessingSource>*) { ++fallbacks; return "fallback:" + message.content; };
        auto platform = [](const auto&, const auto&) { return std::string("onebot_v11"); };
        dispatchLegacyLuaEvent(adapters, router, origin, text, group, user, fallback, platform);
    }
};
}

TEST(LuaEventBridge, LegacyBanPreservesMinutesAndDoesNotSendPrivateSelfReceipts) {
    LuaEventFixture f; f.run();
    ASSERT_EQ(f.a->bans.size(), size_t(1)); ASSERT_EQ(f.a->bans[0].group, "1108650419");
    ASSERT_EQ(f.a->bans[0].user, "10001"); ASSERT_EQ(f.a->bans[0].seconds, 60);
    f.run(".group 1108650419 ban 10001 60");
    ASSERT_EQ(f.a->bans.back().seconds, 3600);
    f.run(".group 1108650419 ban 10001 0"); ASSERT_EQ(f.a->bans.back().seconds, 0);
    ASSERT_TRUE(f.a->messages.empty()); ASSERT_EQ(f.fallbacks, 0);
}
TEST(LuaEventBridge, InvalidIdentitiesCannotBecomePrivateZeroOrAcquirePrivilege) {
    LuaEventFixture f;
    for (const auto& target : {"", "0", "-1", "not-a-qq", "9223372036854775808"}) f.run(".r d1", "", target);
    ASSERT_TRUE(f.a->messages.empty()); ASSERT_TRUE(f.a->bans.empty());
    f.a->self.clear(); f.run(".group 1108650419 ban 10001 1", "", "");
    ASSERT_TRUE(f.a->messages.empty()); ASSERT_TRUE(f.a->bans.empty());
}
TEST(LuaEventBridge, SelfCallsStaySilentButOrdinaryGroupAndPrivateEventsStillReply) {
    LuaEventFixture f; f.run(".r d1"); ASSERT_TRUE(f.a->messages.empty());
    f.run("hello", "1108650419", "10001");
    ASSERT_EQ(f.a->messages.size(), size_t(1)); ASSERT_EQ(f.a->messages[0].targetId, "1108650419");
    ASSERT_EQ(f.a->messages[0].content, "fallback:hello");
    f.run("hello", "", "10001"); ASSERT_EQ(f.a->messages.size(), size_t(2));
    ASSERT_EQ(f.a->messages[1].targetId, "10001"); ASSERT_TRUE(f.a->messages[1].type == MessageType::kPrivate);
}
TEST(LuaEventBridge, CannotChangeGroupUserAccountOrEscalateFromVirtualOrigins) {
    LuaEventFixture f;
    f.run(".group 123456 ban 10001 1"); f.run(".group 1108650419 ban 10002 1");
    f.origin.virtualEvent = true; f.run(); f.origin.virtualEvent = false;
    f.origin.pluginId.clear(); f.run(); f.origin.pluginId = "lua:roulette";
    auto b = std::make_shared<LuaEventAdapter>(); b->account = "lua-event-b"; b->self = "900002"; f.adapters.registerAdapter(b);
    f.a->online = false; f.run();
    ASSERT_TRUE(f.a->bans.empty()); ASSERT_TRUE(b->bans.empty());
    ASSERT_TRUE(f.a->messages.empty()); ASSERT_TRUE(b->messages.empty());
}
TEST(LuaEventBridge, RespectsGroupPluginBlacklistAndBotModerationPermissions) {
    LuaEventFixture f;
    for (const auto& key : {"enabled", "locked", "pluginsOff", "__removed", "left"}) {
        f.setting(key, std::string(key) == "enabled" ? "0" : std::string(key) == "pluginsOff" ? "lua:roulette" : "1"); f.run();
        ASSERT_TRUE(f.a->bans.empty()); f.setting(key, "");
    }
    BanlistRow blocked; blocked.targetType = 0; blocked.listType = 0; blocked.targetId = "10001";
    const auto rowId = f.db.getStorage()->insert(blocked); f.run(); ASSERT_TRUE(f.a->bans.empty());
    f.db.getStorage()->remove<BanlistRow>(rowId);
    f.cfg.set<bool>("dice/silent_global", true); f.run(); ASSERT_TRUE(f.a->bans.empty());
    f.cfg.set<bool>("dice/silent_global", false);
    f.a->role = "member"; f.run(); ASSERT_TRUE(f.a->bans.empty());
    f.a->role.clear(); f.run(); ASSERT_TRUE(f.a->bans.empty()); ASSERT_EQ(f.a->roleRefreshes, 1);
    f.a->role = "admin"; f.a->protectedUser = true; f.run(); ASSERT_TRUE(f.a->bans.empty());
    f.a->protectedUser = false; f.a->supportsBan = false; f.run(); ASSERT_TRUE(f.a->bans.empty());
    ASSERT_TRUE(f.a->messages.empty());
}
TEST(LuaEventBridge, MalformedBanAndUnrelatedAdministrativeCallsDoNotGainPermissions) {
    LuaEventFixture f;
    for (const auto& text : {".group 1108650419 ban 10001 -1", ".group 1108650419 ban 10001 43201",
            ".group 1108650419 ban 10001 1d60", ".group 1108650419 ban 10001 1 extra",
            ".group 0 ban 10001 1", ".group 1108650419 ban 0 1", ".group 1108650419 clr"}) f.run(text);
    ASSERT_TRUE(f.a->bans.empty()); ASSERT_TRUE(f.a->messages.empty());
    ASSERT_TRUE(parseLegacyLuaGroupBan(".group 1108650419 ban 10001 43200").has_value());
    ASSERT_FALSE(parseLegacyLuaGroupBan(".group 1108650419 ban 10001 999999999999999999999").has_value());
}
TEST(OneBotIdentity, InboundSelfIdEstablishesAccountBeforeLoginInfoAndZeroDoesNotEraseIt) {
    OneBotAccountIdentity identity;
    identity.observeEvent({{"post_type", "message"}, {"self_id", "900001"}, {"user_id", 10001}});
    ASSERT_EQ(identity.id(), "900001");
    for (const auto& invalid : {json(0), json(-1), json(""), json("bad"), json(true), json(9.5), json("9223372036854775808")}) {
        identity.observeEvent({{"post_type", "meta_event"}, {"self_id", invalid}}); ASSERT_EQ(identity.id(), "900001");
    }
    identity.observeEvent({{"meta_event_type", "lifecycle"}}); ASSERT_EQ(identity.id(), "900001");
    identity.observeEvent({{"user_id", 10001}, {"self_id", 10001}, {"echo", "stranger"}}); ASSERT_EQ(identity.id(), "900001");
    identity.observeLoginInfo({{"user_id", "900001"}, {"nickname", "骰娘"}});
    ASSERT_EQ(identity.id(), "900001"); ASSERT_EQ(identity.name(), "骰娘");
    identity.observeLoginInfo({{"user_id", 0}, {"nickname", "invalid"}}); ASSERT_EQ(identity.name(), "骰娘");
    identity.observeEvent({{"post_type", "notice"}, {"self_id", 900002}});
    ASSERT_EQ(identity.id(), "900002"); ASSERT_EQ(identity.name(), "");
}
