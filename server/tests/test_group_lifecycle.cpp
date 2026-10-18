#include "test_framework.h"
#include "service/group_list.h"
#include <filesystem>

using namespace dice;

namespace {
struct LifecycleDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-group-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LifecycleDirectory() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create lifecycle fixture");
    }
    ~LifecycleDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

class LifecycleAdapter final : public IAdapter {
public:
    std::string account;
    bool online = true;
    explicit LifecycleAdapter(std::string value) : account(std::move(value)) {}
    std::string id() const override { return account; }
    std::string name() const override { return account; }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "test"; }
    bool configure(const json&) override { return true; }
    bool start() override { throw std::runtime_error("test must never start an adapter"); }
    void stop() override {}
    bool isConnected() const override { return online; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message&) override {}
    void sendReply(const Message&, const std::string&) override {}
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return "9000"; }
    std::string getLoginName() const override { return "测试骰"; }
    std::string getGroupName(const std::string&) const override { return "测试群"; }
    std::vector<std::pair<std::string, std::string>> getGroupList() const override { return {{"5001", "测试群"}}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
};

struct LifecycleFixture {
    LifecycleDirectory dir;
    Database db;
    ConfigManager cfg{(dir.path / "config").string()};
    AdapterManager adapters{db};
    std::shared_ptr<LifecycleAdapter> a = std::make_shared<LifecycleAdapter>("a");
    int delivered = 0;
    LifecycleFixture() {
        if (!db.open((dir.path / "app.db").string())) throw std::runtime_error("cannot open lifecycle fixture");
        cfg.resetDefault();
        adapters.registerAdapter(a);
        adapters.onMessage([&](const Message&) { ++delivered; });
        group_management::discoverRecords(db, adapters);
    }
    auto& storage() { return *db.getStorage(); }
    void set(const std::string& key, const std::string& value, const std::string& account = "a") {
        setAccountGroupSetting(storage(), account, "onebot_v11", "5001", "5001", key, value);
    }
    std::string get(const std::string& key, const std::string& account = "a") {
        return accountGroupSetting(storage(), account, "onebot_v11", "5001", key);
    }
    json list() {
        group_management::discoverRecords(db, adapters);
        return group_management::listRecords(db, adapters, cfg);
    }
    void inbound() {
        Message msg;
        msg.platform = "onebot_v11"; msg.adapterId = "a"; msg.selfId = "9000";
        msg.type = MessageType::kGroup; msg.targetId = "5001";
        msg.senderId = "10001"; msg.senderName = "玩家"; msg.content = "模拟在途消息";
        adapters.routeMessage(msg);
    }
    void leave(bool remove) {
        group_management::requestLeave(storage(), "a", "onebot_v11", "5001", "5001", remove,
            [](const std::string&) {});
    }
};

template <typename Fn> bool lifecycleRejected(Fn&& fn) {
    try { fn(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
} // namespace

TEST(GroupLifecycle, LeaveAndClearDoesNotFallBackToSharedRecordOrCachedPresence) {
    for (bool sharedLeft : {false, true}) {
        LifecycleFixture f;
        if (sharedLeft) group_management::setSharedSetting(f.storage(), "onebot_v11", "5001", "left", "1");
        f.leave(true);
        ASSERT_EQ(f.get("__removed"), "1");
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(f.list().empty());
            f.inbound();
            ASSERT_EQ(f.delivered, 0);
        }
        // The old ten-minute grace expiring cannot resurrect an explicitly left group.
        f.set(kLeaveRequestedAt, std::to_string(static_cast<long long>(std::time(nullptr)) - 3600));
        ASSERT_TRUE(f.list().empty());
        f.inbound();
        ASSERT_EQ(f.delivered, 0);
        ASSERT_EQ(f.get("__removed"), "1");
    }
}

TEST(GroupLifecycle, InboundCannotInterleaveWithLeaveAndClear) {
    LifecycleFixture f;
    int requests = 0;
    group_management::requestLeave(f.storage(), "a", "onebot_v11", "5001", "5001", true,
        [&](const std::string& endpoint) { ++requests; f.inbound(); });
    ASSERT_EQ(requests, 1);
    ASSERT_EQ(f.delivered, 0);
    ASSERT_TRUE(f.list().empty());
}

TEST(GroupLifecycle, ClearOneAccountKeepsOtherBotInTheSameGroup) {
    LifecycleFixture f;
    auto b = std::make_shared<LifecycleAdapter>("b");
    f.adapters.registerAdapter(b);
    group_management::discoverRecords(f.db, f.adapters);
    f.leave(true);
    auto rows = f.list();
    ASSERT_EQ(rows.size(), size_t(1));
    ASSERT_FALSE(rows[0]["left"].get<bool>());
    ASSERT_EQ(rows[0]["accounts"].size(), size_t(1));
    ASSERT_EQ(rows[0]["accounts"][0]["adapterId"].get<std::string>(), "b");
    ASSERT_EQ(f.get("__removed", "b"), "0");
}

TEST(GroupLifecycle, DepartedOfflineRecordCanBeClearedWithoutAnotherLeaveRequest) {
    LifecycleFixture f;
    f.set("left", "1"); // Old or kicked record without a request timestamp.
    f.a->online = false;
    group_management::removeRecord(f.storage(), "a", "onebot_v11", "5001", "5001", true);
    ASSERT_TRUE(f.list().empty());
    ASSERT_TRUE(groupLeaveRequested(f.storage(), "a", "onebot_v11", "5001"));
    f.inbound();
    ASSERT_EQ(f.delivered, 0);
}

TEST(GroupLifecycle, GenuineRejoinRestoresOnlyTheJoinedAccount) {
    LifecycleFixture f;
    auto b = std::make_shared<LifecycleAdapter>("b");
    f.adapters.registerAdapter(b);
    group_management::discoverRecords(f.db, f.adapters);
    f.leave(true);
    f.set("left", "1", "b");
    group_management::removeRecord(f.storage(), "b", "onebot_v11", "5001", "5001", true);
    ASSERT_TRUE(f.list().empty());
    group_management::restoreRecord(f.storage(), "a", "onebot_v11", "5001", "5001");
    const auto rows = f.list();
    ASSERT_EQ(rows.size(), size_t(1));
    ASSERT_EQ(rows[0]["accounts"].size(), size_t(1));
    ASSERT_EQ(rows[0]["accounts"][0]["adapterId"].get<std::string>(), "a");
    ASSERT_EQ(f.get("__removed", "b"), "1");
    f.inbound();
    ASSERT_EQ(f.delivered, 1);
}

TEST(GroupLifecycle, OnlyDepartedCleanupRejectsRejoinedOrWrongAccountBeforeDeleting) {
    LifecycleFixture f;
    ASSERT_TRUE(lifecycleRejected([&] { group_management::removeRecord(f.storage(), "a", "onebot_v11", "5001", "5001", true); }));
    ASSERT_TRUE(lifecycleRejected([&] { group_management::removeRecord(f.storage(), "a", "milky", "5001", "5001"); }));
    ASSERT_TRUE(lifecycleRejected([&] { group_management::removeRecord(f.storage(), "a", "onebot_v11", "5001", "other-group"); }));
    ASSERT_TRUE(lifecycleRejected([&] { group_management::removeRecord(f.storage(), "missing", "onebot_v11", "5001"); }));
    ASSERT_TRUE(lifecycleRejected([&] { group_management::removeRecord(f.storage(), "", "onebot_v11", "5001"); }));
    ASSERT_EQ(f.get("__removed"), "0");
    ASSERT_EQ(f.list().size(), size_t(1));
}

TEST(GroupLifecycle, FailedLeaveSubmissionRollsBackAndDoesNotRemoveRecord) {
    LifecycleFixture f;
    bool failed = false;
    try {
        group_management::requestLeave(f.storage(), "a", "onebot_v11", "5001", "5001", true,
            [](const std::string&) { throw std::runtime_error("simulated send failure"); });
    } catch (const std::runtime_error&) { failed = true; }
    ASSERT_TRUE(failed);
    ASSERT_FALSE(groupLeaveRequested(f.storage(), "a", "onebot_v11", "5001"));
    ASSERT_EQ(f.get("__removed"), "0");
    ASSERT_EQ(f.list().size(), size_t(1));
}

TEST(GroupLifecycle, RecordOnlyDeletionOfJoinedGroupCanStillRestoreOnNewMessage) {
    LifecycleFixture f;
    group_management::removeRecord(f.storage(), "a", "onebot_v11", "5001", "5001");
    ASSERT_TRUE(f.list().empty());
    f.inbound();
    ASSERT_EQ(f.delivered, 1);
    ASSERT_EQ(f.list().size(), size_t(1));
}

TEST(GroupLifecycle, DelayedLeaveEventAndRestartDoNotRecreateDeletedRecord) {
    LifecycleFixture f;
    f.leave(true);
    f.set("left", "1"); // Delayed self-decrease notice after DELETE.
    f.db.close();
    ASSERT_TRUE(f.db.open((f.dir.path / "app.db").string()));
    ASSERT_TRUE(f.list().empty());
    f.inbound();
    ASSERT_EQ(f.delivered, 0);
}

TEST(GroupLifecycle, CleanupKeepsCharacterCardsAndGameTranscripts) {
    LifecycleFixture f;
    CharacterCardRow card;
    card.userId = "10001"; card.name = "调查员"; card.attrs = "{}";
    f.db.getCardStorage()->insert(card);
    GameLogRow log; log.name = "保留跑团日志"; log.groupId = "5001";
    f.db.getLogStorage()->insert(log);
    f.leave(true);
    ASSERT_TRUE(f.list().empty());
    ASSERT_EQ(f.db.getCardStorage()->count<CharacterCardRow>(), 1);
    ASSERT_EQ(f.db.getLogStorage()->count<GameLogRow>(), 1);
}
