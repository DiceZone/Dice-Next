#include "test_framework.h"
#include "../src/adapter/qq_official_adapter.h"
#include "../src/adapter/adapter_manager.h"
#include <chrono>
#include <filesystem>
#include <memory>

using namespace dice;

namespace {
struct NamesDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dice-official-names-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    NamesDir() { if (!std::filesystem::create_directory(path)) throw std::runtime_error("test directory failed"); }
    ~NamesDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct NameFixture {
    NamesDir dir;
    Database db;
    std::unique_ptr<AdapterManager> manager;
    std::shared_ptr<QQOfficialAdapter> adapter;
    struct Request { std::string path; json body; IAdapter::ActionCallback reply; };
    std::vector<Request> requests;
    std::vector<Message> messages;
    std::vector<BotEvent> events;
    unsigned messageSequence = 0;
    explicit NameFixture(bool persistent = true) {
        if (persistent) {
            if (!db.open((dir.path / "dice.db").string())) throw std::runtime_error("test database failed");
            manager = std::make_unique<AdapterManager>(db);
        }
        adapter = std::make_shared<QQOfficialAdapter>("official-A");
        adapter->configure({{"appId", "app-A"}, {"appSecret", "fixture-only"}});
        adapter->setApiTransportForTest([this](drogon::HttpMethod method, const std::string& path, const json& body, IAdapter::ActionCallback reply) {
            if (method != drogon::Get) throw std::runtime_error("names must not write to the platform");
            requests.push_back({path, body, std::move(reply)});
        });
        if (manager) {
            manager->registerAdapter(adapter);
            manager->onMessage([this](const Message& message) { messages.push_back(message); });
            manager->onEvent([this](const BotEvent& event) { events.push_back(event); });
        } else {
            adapter->onMessage([this](const Message& message) { messages.push_back(message); });
            adapter->onEvent([this](const BotEvent& event) { events.push_back(event); });
        }
    }
    void answer(size_t index, const json& data, int status = 200) {
        auto callback = std::move(requests.at(index).reply);
        callback(QQOfficialAdapter::apiResponseForTest(status, data));
    }
    void group(const std::string& user, const json& username, const std::string& united = {}) {
        adapter->dispatchForTest("GROUP_MESSAGE_CREATE", {{"id", "message-" + std::to_string(++messageSequence)}, {"group_openid", "group-A"}, {"content", ".r"},
            {"author", {{"id", user}, {"member_openid", user}, {"username", username}, {"union_openid", united}, {"member_role", "member"}}}});
    }
    void privateMessage(const std::string& user, const json& username, const std::string& united = {}) {
        adapter->dispatchForTest("C2C_MESSAGE_CREATE", {{"id", "private-" + std::to_string(++messageSequence)}, {"content", ".r"},
            {"author", {{"id", user}, {"user_openid", user}, {"username", username}, {"union_openid", united}}}});
    }
    PlayerProfileRow profile(const std::string& publicId) {
        auto rows = db.getStorage()->get_all<PlayerProfileRow>(orm::where(orm::c(&PlayerProfileRow::platform) == std::string("qq_official")
            and orm::c(&PlayerProfileRow::userId) == publicId), orm::limit(1));
        return rows.empty() ? PlayerProfileRow{} : rows.front();
    }
};
}

TEST(OfficialNames, GroupAndPrivateMessagesPublishActualNames) {
    NameFixture f;
    f.group("member", "群中昵称");
    ASSERT_EQ(f.messages.back().senderName, "群中昵称");
    ASSERT_EQ(f.profile(f.messages.back().senderId).nickname, "群中昵称");
    f.privateMessage("friend", "私聊昵称");
    ASSERT_EQ(f.messages.back().senderName, "私聊昵称");
    ASSERT_EQ(f.profile(f.messages.back().senderId).nickname, "私聊昵称");
}

TEST(OfficialNames, TemplateCacheLookupsNeverScheduleRequests) {
    NameFixture f;
    ASSERT_EQ(f.adapter->cachedUserName("unknown","unseen-group"), "");
    ASSERT_EQ(f.adapter->cachedGroupName("unseen-group"), "");
    ASSERT_TRUE(f.requests.empty());
    f.group("member", "真实昵称");
    const auto before = f.requests.size();
    ASSERT_EQ(f.adapter->cachedUserName("member","group-A"), "真实昵称");
    ASSERT_EQ(f.requests.size(), before);
}

TEST(OfficialNames, EmptyNullAndMalformedNamesRetainKnownNames) {
    NameFixture f;
    f.group("member", "真实名字");
    const auto publicId = f.messages.back().senderId;
    for (const auto& empty : {json(""), json(nullptr), json(123), json::object()}) {
        f.group("member", empty);
        ASSERT_EQ(f.messages.back().senderName, "真实名字");
        ASSERT_EQ(f.profile(publicId).nickname, "真实名字");
    }
    f.privateMessage("member", "");
    ASSERT_EQ(f.messages.back().senderName, "真实名字");
}

TEST(OfficialNames, UnknownFallbackDoesNotPoisonCaches) {
    NameFixture f;
    f.privateMessage("unknown", "");
    ASSERT_EQ(f.messages.back().senderName, "用户");
    ASSERT_TRUE(f.messages.back().extra.value("__sender_name_fallback", false));
    ASSERT_TRUE(f.adapter->getUserName("unknown").empty());
    ASSERT_TRUE(f.profile(f.messages.back().senderId).nickname.empty());
    f.privateMessage("unknown", "后来取得的名字");
    ASSERT_EQ(f.messages.back().senderName, "后来取得的名字");
    ASSERT_FALSE(f.messages.back().extra.value("__sender_name_fallback", false));
}

TEST(OfficialNames, FriendAddReusesTheMessageIdentityAndCachedName) {
    NameFixture f;
    f.group("known-user", "希亚");
    const auto publicId = f.messages.back().senderId;
    f.adapter->dispatchForTest("FRIEND_ADD", {{"openid", "known-user"}, {"timestamp", 1784570523}, {"author", {{"union_openid", ""}}}});
    ASSERT_EQ(f.events.back().userId, publicId);
    ASSERT_EQ(f.events.back().extra.value("user_name", std::string()), "希亚");
    ASSERT_EQ(f.events.back().extra.value("__identity_native_sender", std::string()), "known-user");
}

TEST(OfficialNames, UnionNameAssociationDoesNotMergeIdentitiesOrPermissions) {
    NameFixture f;
    f.group("group-user", "同一昵称", "trusted-union");
    const auto groupUser = f.messages.back().senderId;
    auto original = f.profile(groupUser); original.trustLevel = 9; original.favor = 12; f.db.getStorage()->update(original);
    f.adapter->dispatchForTest("FRIEND_ADD", {{"openid", "private-user"}, {"author", {{"union_openid", "trusted-union"}}}});
    const auto friendUser = f.events.back().userId;
    ASSERT_NE(friendUser, groupUser);
    ASSERT_EQ(f.events.back().extra.value("user_name", std::string()), "同一昵称");
    ASSERT_EQ(f.profile(friendUser).trustLevel, 0);
    ASSERT_EQ(f.profile(friendUser).favor, 0);
    ASSERT_EQ(f.profile(groupUser).trustLevel, 9);
    f.privateMessage("private-user", "新昵称", "trusted-union");
    f.group("group-user", "", "trusted-union");
    ASSERT_EQ(f.messages.back().senderName, "新昵称");
}

TEST(OfficialNames, DifferentAppsNeverShareRawOrUnionNames) {
    QQOfficialNameCache cache;
    cache.rememberUser("app-A", "openid", "A名字", "union");
    ASSERT_TRUE(cache.user("app-B", "openid").empty());
    ASSERT_TRUE(cache.rememberUser("app-B", "other", "", "union").empty());
    cache.rememberUser("app-B", "openid", "B名字", "union");
    ASSERT_EQ(cache.user("app-A", "openid"), "A名字");
    ASSERT_EQ(cache.user("app-B", "openid"), "B名字");
}

TEST(OfficialNames, RestartRestoresNamesAndUnionLinks) {
    NamesDir dir;
    const auto path = (dir.path / "dice.db").string();
    {
        Database db; ASSERT_TRUE(db.open(path)); AdapterManager manager(db);
        QQOfficialNameCache cache; cache.attach(db);
        cache.rememberUser("app-A", "group-user", "重启前昵称", "union");
        cache.rememberUser("app-A", "private-user", "", "union");
        cache.rememberGroup("app-A", "group", "重启前群名");
    }
    {
        Database db; ASSERT_TRUE(db.open(path)); AdapterManager manager(db);
        auto adapter = std::make_shared<QQOfficialAdapter>("official-A");
        adapter->configure({{"appId", "app-A"}, {"appSecret", "fixture"}}); manager.registerAdapter(adapter);
        ASSERT_EQ(adapter->getUserName("private-user"), "重启前昵称");
        ASSERT_EQ(adapter->getGroupName("group"), "重启前群名");
        BotEvent received; manager.onEvent([&](const BotEvent& event) { received = event; });
        adapter->dispatchForTest("FRIEND_ADD", {{"openid", "private-user"}, {"author", {{"union_openid", "union"}}}});
        ASSERT_EQ(received.extra.value("user_name", std::string()), "重启前昵称");
    }
}

TEST(OfficialNames, NicknameUpdatesDoNotResetProfileCountersOrTrust) {
    NameFixture f;
    f.group("member", "旧名"); const auto publicId = f.messages.back().senderId;
    auto profile = f.profile(publicId); profile.cmdCount = 345; profile.groupCmdCount = 123; profile.trustLevel = 9; profile.favor = 7;
    f.db.getStorage()->update(profile);
    f.group("member", "新名");
    const auto updated = f.profile(publicId);
    ASSERT_EQ(updated.nickname, "新名"); ASSERT_EQ(updated.cmdCount, 345); ASSERT_EQ(updated.groupCmdCount, 123);
    ASSERT_EQ(updated.trustLevel, 9); ASSERT_EQ(updated.favor, 7);
}

TEST(OfficialNames, QueryResultsFeedPublicProfilesAndGenericMemberLists) {
    NameFixture f;
    ASSERT_TRUE(f.adapter->getUserName("member", "group-A").empty());
    ASSERT_EQ(f.requests.at(0).path, "/v2/groups/group-A/members/member");
    f.answer(0, {{"member_openid", "member"}, {"username", "查询得到的名字"}, {"member_role", "owner"}, {"bot", false}});
    ASSERT_EQ(f.adapter->getUserName("member"), "查询得到的名字");
    const auto publicId = identity::BindingStore::instance().publicForOfficial(f.db, "app-A", "member", identity::Kind::User);
    ASSERT_EQ(f.profile(publicId).nickname, "查询得到的名字");
    ASSERT_TRUE(f.adapter->getMembers("group-A").empty()); // A single result is not a complete group list.
    f.answer(1, {{"members", json::array({{{"member_openid", "member"}, {"username", "查询得到的名字"}, {"member_role", "owner"}, {"bot", false}}})}, {"next_cursor", ""}});
    const auto members = f.adapter->getMembers("group-A");
    ASSERT_EQ(members.size(), size_t(1)); ASSERT_EQ(members[0]["user_id"].get<std::string>(), publicId);
    ASSERT_EQ(members[0]["nickname"].get<std::string>(), "查询得到的名字");
    ASSERT_EQ(members[0]["role"].get<std::string>(), "owner");
}

TEST(OfficialNames, ManualMemberQueriesAlsoWarmTheCache) {
    NameFixture f; json result;
    f.adapter->invokeActionAsync("qq_group_member", {{"groupId", "group-A"}, {"memberOpenId", "member"}}, [&](json reply) { result = std::move(reply); });
    f.answer(0, {{"data", {{"member_openid", "member"}, {"username", "手动查询昵称"}, {"member_role", "member"}}}, {"code", 0}});
    ASSERT_TRUE(result.value("ok", false)); ASSERT_EQ(f.adapter->getUserName("member"), "手动查询昵称");
}

TEST(OfficialNames, PaginationKeepsEveryPageNameAndCursor) {
    NameFixture f;
    ASSERT_TRUE(f.adapter->getMembers("group-A").empty());
    f.answer(0, {{"members", json::array({{{"member_openid", "one"}, {"username", "第一页"}, {"member_role", "member"}}})}, {"next_cursor", "next"}});
    ASSERT_EQ(f.requests.at(1).body.value("cursor", std::string()), "next");
    f.answer(1, {{"members", json::array({{{"member_openid", "two"}, {"username", "第二页"}, {"member_role", "admin"}}})}, {"next_cursor", ""}});
    ASSERT_EQ(f.adapter->getMembers("group-A").size(), size_t(2));
    ASSERT_EQ(f.adapter->getUserName("one"), "第一页"); ASSERT_EQ(f.adapter->getUserName("two"), "第二页");
    ASSERT_EQ(f.requests.size(), size_t(2));
}

TEST(OfficialNames, RepeatedPaginationCursorStopsWithoutLosingKnownNames) {
    NameFixture f; f.adapter->getMembers("group-A");
    const json page = {{"members", json::array({{{"member_openid", "one"}, {"username", "名字"}, {"member_role", "member"}}})}, {"next_cursor", "loop"}};
    f.answer(0, page); f.answer(1, page);
    ASSERT_EQ(f.requests.size(), size_t(2)); ASSERT_EQ(f.adapter->getUserName("one"), "名字");
    ASSERT_TRUE(f.adapter->getMembers("group-A").empty());
}

TEST(OfficialNames, GroupInfoWarmsPersistentGroupNameAndCount) {
    NameFixture f;
    ASSERT_TRUE(f.adapter->getGroupName("group-A").empty());
    ASSERT_EQ(f.requests.at(0).path, "/v2/groups/group-A/info");
    f.answer(0, {{"group_openid", "group-A"}, {"group_name", "骰子内测群"}, {"group_member_num", 256}});
    ASSERT_EQ(f.adapter->getGroupName("group-A"), "骰子内测群");
    ASSERT_EQ(f.adapter->getGroupMemberCount("group-A"), 256);
    const auto publicId = identity::BindingStore::instance().publicForOfficial(f.db, "app-A", "group-A", identity::Kind::Group);
    const auto rows = f.db.getStorage()->get_all<GroupSettingRow>(orm::where(orm::c(&GroupSettingRow::groupId) == publicId
        and orm::c(&GroupSettingRow::key) == std::string("name")));
    ASSERT_EQ(rows.size(), size_t(1)); ASSERT_EQ(rows.front().value, "骰子内测群");
    ASSERT_EQ(f.requests.size(), size_t(1));
}

TEST(OfficialNames, EmptyGroupResponseNeverOverwritesARealName) {
    NameFixture f;
    f.adapter->invokeActionAsync("qq_group_info", {{"groupId", "group-A"}}, [](json) {});
    f.answer(0, {{"group_name", "真实群名"}});
    f.adapter->invokeActionAsync("qq_group_info", {{"groupId", "group-A"}}, [](json) {});
    f.answer(1, {{"group_name", ""}});
    // getGroupName can start a background refresh, but the cached name is immediate.
    ASSERT_EQ(f.adapter->getGroupName("group-A"), "真实群名");
}

TEST(OfficialNames, PermissionErrorsOnHttp200AreNotSuccessAndBackOffPerApi) {
    NameFixture f; f.adapter->getGroupName("group-A");
    f.answer(0, {{"err_code", 11253}, {"message", "无接口权限"}});
    for (int index = 0; index < 10; ++index) f.adapter->getGroupName("other-" + std::to_string(index));
    ASSERT_EQ(f.requests.size(), size_t(1));
    // An info denial does not prevent a separately authorized member query.
    f.adapter->getUserName("member", "group-A");
    ASSERT_EQ(f.requests.size(), size_t(2));
    f.answer(1, {{"err_code", 11253}, {"message", "成员接口未授权"}});
    f.adapter->getUserName("other-member", "group-A"); ASSERT_EQ(f.requests.size(), size_t(2));
}

TEST(OfficialNames, BusinessErrorsMalformedBodiesAndHttpFailuresAreRejected) {
    ASSERT_FALSE(QQOfficialAdapter::apiResponseForTest(200, {{"err_code", 11253}}).value("ok", true));
    ASSERT_FALSE(QQOfficialAdapter::apiResponseForTest(200, {{"code", 11253}}).value("ok", true));
    ASSERT_FALSE(QQOfficialAdapter::apiResponseForTest(200, {{"err_code", "invalid"}}).value("ok", true));
    ASSERT_FALSE(QQOfficialAdapter::apiResponseForTest(429, json::object()).value("ok", true));
    ASSERT_FALSE(QQOfficialAdapter::apiResponseForTest(200, json::parse("bad", nullptr, false)).value("ok", true));
    ASSERT_TRUE(QQOfficialAdapter::apiResponseForTest(200, {{"err_code", 0}, {"group_name", "成功"}}).value("ok", false));
    ASSERT_TRUE(QQOfficialAdapter::apiResponseForTest(204, json::object()).value("ok", false));
}

TEST(OfficialNames, QueryDeduplicationAndAppRateBudgetsBoundRequests) {
    NameFixture f;
    for (int index = 0; index < 10; ++index) f.adapter->getUserName("same", "group-A");
    ASSERT_EQ(f.requests.size(), size_t(1));
    for (int index = 0; index < 50; ++index) f.adapter->getUserName("member-" + std::to_string(index), "group-A");
    ASSERT_EQ(f.requests.size(), size_t(28));
    for (int index = 0; index < 50; ++index) f.adapter->getGroupName("group-" + std::to_string(index));
    ASSERT_EQ(f.requests.size(), size_t(56));
}

TEST(OfficialNames, ASlowQueryCannotOverwriteANewerMessageName) {
    NameFixture f;
    f.adapter->getUserName("member", "group-A");
    f.group("member", "刚更新的新名字");
    f.answer(0, {{"member_openid", "member"}, {"username", "旧查询名字"}, {"member_role", "member"}});
    ASSERT_EQ(f.adapter->getUserName("member"), "刚更新的新名字");
    ASSERT_EQ(f.profile(f.messages.back().senderId).nickname, "刚更新的新名字");
}

TEST(OfficialNames, ReconfigurationRejectsOldAppQueryResults) {
    NameFixture f; f.adapter->getUserName("member", "group-A");
    f.adapter->configure({{"appId", "app-B"}, {"appSecret", "fixture"}});
    f.answer(0, {{"member_openid", "member"}, {"username", "A的名字"}, {"member_role", "member"}});
    ASSERT_TRUE(f.adapter->getUserName("member").empty());
}

TEST(OfficialNames, NamedEventsAndOperatorsUseNormalizedIdsWithoutChangingRequestFlags) {
    NameFixture f; f.group("member", "申请人"); f.group("inviter", "邀请人");
    f.adapter->dispatchForTest("GROUP_JOIN_REQUEST", {{"group_openid", "group-A"}, {"member_openid", "member"},
        {"invited_by", "inviter"}, {"join_request_id", "request"}});
    const auto& event = f.events.back();
    ASSERT_EQ(event.extra.value("user_name", std::string()), "申请人");
    ASSERT_EQ(event.extra.value("operator_name", std::string()), "邀请人");
    ASSERT_EQ(event.flag, std::string("group-A\x1fmember\x1frequest"));
    ASSERT_NE(event.userId, "member"); ASSERT_NE(event.operatorId, "inviter");
    ASSERT_EQ(eventInviterId(event), "inviter"); // Name normalization cannot change invitation authority.
}

TEST(OfficialNames, VerifiedMentionNamesAreCachedButQuotedAuthorsAreNot) {
    NameFixture f;
    f.adapter->dispatchForTest("GROUP_MESSAGE_CREATE", {{"content", "消息"}, {"group_openid", "group-A"},
        {"author", {{"member_openid", "sender"}, {"username", "发送人"}}},
        {"mentions", json::array({{{"member_openid", "mentioned"}, {"username", "被提及者"}}})},
        {"msg_elements", json::array({{{"author", {{"member_openid", "sender"}, {"username", "旧引用名字"}}}}})}});
    ASSERT_EQ(f.adapter->getUserName("mentioned"), "被提及者");
    ASSERT_EQ(f.adapter->getUserName("sender"), "发送人");
}

TEST(OfficialNames, UserIdFallbackAndActualLiteralUsernameAreHandledWithoutCrashes) {
    NameFixture f;
    f.adapter->dispatchForTest("C2C_MESSAGE_CREATE", {{"content", ".r"}, {"author", {{"id", "friend"}, {"username", "用户"}}}});
    ASSERT_EQ(f.messages.back().senderName, "用户");
    ASSERT_FALSE(f.messages.back().extra.value("__sender_name_fallback", false));
    ASSERT_EQ(f.adapter->getUserName("friend"), "用户");
    ASSERT_EQ(f.profile(f.messages.back().senderId).nickname, "用户");
}

TEST(OfficialNames, EmptyNewFriendEventRemainsUnknownAndHasNoInventedLookupApi) {
    NameFixture f;
    f.adapter->dispatchForTest("FRIEND_ADD", {{"openid", "new-friend"}, {"author", {{"union_openid", "unseen"}}}});
    ASSERT_TRUE(f.events.back().extra.value("user_name", std::string()).empty());
    ASSERT_TRUE(f.adapter->getUserName("new-friend").empty());
    ASSERT_TRUE(f.requests.empty());
}

TEST(OfficialNames, IdlePrewarmingDiscoversOnlyThisAppsPresentGroupsWithoutMessages) {
    NameFixture f; f.adapter->setNameTimeForTest(1000);
    auto& bindings = identity::BindingStore::instance();
    const auto present = bindings.observeOfficial(f.db, "app-A", "known-group", identity::Kind::Group);
    const auto left = bindings.observeOfficial(f.db, "app-A", "left-group", identity::Kind::Group);
    bindings.observeOfficial(f.db, "app-B", "foreign-group", identity::Kind::Group);
    GroupSettingRow shared; shared.platform = "qq_official"; shared.groupId = present; shared.key = "left"; shared.value = "1";
    f.db.getStorage()->insert(shared);
    GroupAccountSettingRow row; row.adapterId = "official-A"; row.platform = "qq_official";
    row.groupId = present; row.endpointId = "known-group"; row.key = "left"; row.value = "0"; f.db.getStorage()->insert(row);
    row.key = "enabled"; row.value = "0"; f.db.getStorage()->insert(row); // bot off still prewarms names.
    row.groupId = left; row.endpointId = "left-group"; row.key = "left"; row.value = "1"; f.db.getStorage()->insert(row);
    f.adapter->maintainNamesForTest(false); ASSERT_TRUE(f.requests.empty());
    f.adapter->maintainNamesForTest();
    ASSERT_EQ(f.requests.size(), size_t(1)); ASSERT_EQ(f.requests[0].path, "/v2/groups/known-group/info");
    f.answer(0, {{"group_name", "已知群"}});
    f.adapter->setNameTimeForTest(1003); f.adapter->maintainNamesForTest();
    ASSERT_EQ(f.requests.size(), size_t(2)); ASSERT_EQ(f.requests[1].path, "/v2/groups/known-group/members");
    f.answer(1, {{"members", json::array({{{"member_openid", "quiet-user"}, {"username", "从未发言的成员"}}})}, {"next_cursor", ""}});
    ASSERT_EQ(f.adapter->getUserName("quiet-user"), "从未发言的成员");
    ASSERT_TRUE(f.messages.empty());
}

TEST(OfficialNames, IdleWorkerYieldsForIncomingAndOutgoingMessagesAndSpacesRequests) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->setNameTimeForTest(1002); f.adapter->maintainNamesForTest(); ASSERT_TRUE(f.requests.empty());
    f.adapter->sendGroupMessage("group-A", "fixture-only; no token or network");
    f.adapter->setNameTimeForTest(1004); f.adapter->maintainNamesForTest(); ASSERT_TRUE(f.requests.empty());
    f.adapter->setNameTimeForTest(1005); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(1));
    f.answer(0, {{"group_name", "群"}});
    f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(1));
    f.adapter->setNameTimeForTest(1006); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(1));
    f.adapter->setNameTimeForTest(1007); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(2));
}

TEST(OfficialNames, BackgroundPaginationYieldsAfterEachPageAndCachesAllBeyond3000) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->getGroupName("group-A"); f.answer(0, {{"group_name", "大群"}});
    for (size_t page = 0; page <= 100; ++page) {
        f.adapter->setNameTimeForTest(1004 + static_cast<std::time_t>(page * 3));
        f.adapter->maintainNamesForTest();
        ASSERT_EQ(f.requests.size(), page + 2);
        ASSERT_EQ(f.requests[page + 1].body.value("cursor", std::string()), page ? std::to_string(page) : std::string());
        json members = json::array();
        for (size_t index = page * 30; index < (std::min)(size_t(3001), (page + 1) * 30); ++index)
            members.push_back({{"member_openid", "member-" + std::to_string(index)}, {"username", "名字" + std::to_string(index)}, {"member_role", "member"}});
        f.answer(page + 1, {{"members", members}, {"next_cursor", page == 100 ? std::string() : std::to_string(page + 1)}});
        ASSERT_EQ(f.requests.size(), page + 2); // No immediate burst, even with a synchronous test transport.
    }
    const auto members = f.adapter->getMembers("group-A");
    ASSERT_EQ(members.size(), size_t(3001)); ASSERT_EQ(f.adapter->getUserName("member-3000"), "名字3000");
    ASSERT_EQ(f.requests.size(), size_t(102));
}

TEST(OfficialNames, ExhaustedRateBudgetResumesSavedCursorInTheIdleWorker) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->getGroupName("group-A"); f.answer(0, {{"group_name", "群"}});
    f.adapter->getMembers("group-A");
    for (size_t page = 0; page < 58; ++page) {
        if (f.requests.size() < page + 2) f.adapter->getMembers("group-A");
        f.answer(page + 1, {{"members", json::array({{{"member_openid", "member-" + std::to_string(page)}, {"username", "分页成员"}}})},
            {"next_cursor", std::to_string(page + 1)}});
    }
    ASSERT_EQ(f.requests.size(), size_t(59));
    f.adapter->setNameTimeForTest(1004); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(59));
    f.adapter->setNameTimeForTest(1061); f.adapter->maintainNamesForTest();
    ASSERT_EQ(f.requests.size(), size_t(60)); ASSERT_EQ(f.requests[59].body.value("cursor", std::string()), "58");
    f.answer(59, {{"members", json::array({{{"member_openid", "last"}, {"username", "最后一页"}}})}, {"next_cursor", ""}});
    ASSERT_EQ(f.adapter->getMembers("group-A").size(), size_t(59));
}

TEST(OfficialNames, PrewarmingIsFairAcrossGroupsAndDoesNotRepeatFreshLists) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000);
    for (const auto& group : {"group-A", "group-B"}) f.adapter->dispatchForTest("GROUP_MESSAGE_CREATE",
        {{"group_openid", group}, {"author", {{"member_openid", "seed"}, {"username", "名字"}}}});
    for (int index = 0; index < 4; ++index) {
        f.adapter->setNameTimeForTest(1004 + index * 3); f.adapter->maintainNamesForTest();
        const auto group = index % 2 ? "group-B" : "group-A";
        ASSERT_EQ(f.requests[index].path, std::string("/v2/groups/") + group + (index < 2 ? "/info" : "/members"));
        f.answer(index, index < 2 ? json{{"group_name", "群"}} : json{{"members", json::array()}, {"next_cursor", ""}});
    }
    f.adapter->setNameTimeForTest(3000); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(4));
    f.adapter->setNameTimeForTest(4615); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(5));
}

TEST(OfficialNames, ForegroundQueriesKeepPriorityOverBackgroundWork) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->getUserName("unknown", "group-A");
    f.adapter->setNameTimeForTest(1004); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(1));
    f.answer(0, {{"member_openid", "unknown"}, {"username", "新名"}});
    f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(2));
    ASSERT_EQ(f.requests[1].path, "/v2/groups/group-A/info");
}

TEST(OfficialNames, PermissionDenialsBackOffWithoutStoppingOtherApiCategories) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->setNameTimeForTest(1004); f.adapter->maintainNamesForTest();
    f.answer(0, {{"err_code", 11253}});
    f.adapter->setNameTimeForTest(1007); f.adapter->maintainNamesForTest();
    ASSERT_EQ(f.requests[1].path, "/v2/groups/group-A/members"); f.answer(1, {{"err_code", 11253}});
    f.adapter->setNameTimeForTest(2000); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(2));
    f.adapter->setNameTimeForTest(4605); f.adapter->maintainNamesForTest(); ASSERT_EQ(f.requests.size(), size_t(3));
}

TEST(OfficialNames, FailedRefreshPreservesCompleteRosterAndKnownNamesUntilRetry) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000);
    f.adapter->getMembers("group-A"); f.answer(0, {{"members", json::array({{{"member_openid", "old"}, {"username", "旧成员"}}})}, {"next_cursor", ""}});
    f.adapter->setNameTimeForTest(1301); f.adapter->getMembers("group-A");
    f.answer(1, {{"members", json::array({{{"member_openid", "new"}, {"username", "新成员"}}})}, {"next_cursor", "second"}});
    f.answer(2, {{"message", "temporary failure"}}, 503);
    const auto members = f.adapter->getMembers("group-A");
    ASSERT_EQ(members.size(), size_t(1)); ASSERT_EQ(members[0].value("user_id", std::string()), "old");
    ASSERT_EQ(f.adapter->getUserName("new"), "新成员"); ASSERT_EQ(f.requests.size(), size_t(3));
    f.adapter->setNameTimeForTest(1362); f.adapter->getMembers("group-A"); ASSERT_EQ(f.requests.size(), size_t(4));
}

TEST(OfficialNames, NonConsecutiveCursorCyclesStopAndDuplicateMembersAreDeduplicated) {
    NameFixture f(false); f.adapter->getMembers("group-A");
    const auto page = json::array({{{"member_openid", "member"}, {"username", "名字"}, {"member_role", "member"}}});
    f.answer(0, {{"members", page}, {"next_cursor", "A"}});
    f.answer(1, {{"members", page}, {"next_cursor", "B"}});
    f.answer(2, {{"members", page}, {"next_cursor", "A"}});
    ASSERT_EQ(f.requests.size(), size_t(3)); ASSERT_TRUE(f.adapter->getMembers("group-A").empty());
    NameFixture unique(false); unique.adapter->getMembers("group-A");
    unique.answer(0, {{"members", page}, {"next_cursor", "next"}});
    unique.answer(1, {{"members", page}, {"next_cursor", ""}});
    ASSERT_EQ(unique.adapter->getMembers("group-A").size(), size_t(1));
}

TEST(OfficialNames, RetiredGroupsAndStoppedAdaptersCannotResumeOrAcceptLateResults) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->getMembers("group-A");
    f.adapter->dispatchForTest("GROUP_DEL_ROBOT", {{"group_openid", "group-A"}});
    f.answer(0, {{"members", json::array({{{"member_openid", "late"}, {"username", "迟到查询"}}})}, {"next_cursor", "more"}});
    f.adapter->setNameTimeForTest(2000); f.adapter->maintainNamesForTest();
    ASSERT_EQ(f.requests.size(), size_t(1)); ASSERT_TRUE(f.adapter->getUserName("late").empty());
    NameFixture stopped; stopped.adapter->getMembers("group-A"); stopped.adapter->stop();
    stopped.answer(0, {{"members", json::array({{{"member_openid", "late"}, {"username", "不应入库"}}})}, {"next_cursor", "more"}});
    stopped.adapter->maintainNamesForTest();
    ASSERT_EQ(stopped.requests.size(), size_t(1)); ASSERT_TRUE(stopped.adapter->getUserName("late").empty());
    ASSERT_TRUE(stopped.db.getStorage()->get_all<PlayerProfileRow>().empty());
}

TEST(OfficialNames, RejoiningDoesNotReviveAQueryFromBeforeLeaving) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->getMembers("group-A");
    f.adapter->dispatchForTest("GROUP_DEL_ROBOT", {{"group_openid", "group-A"}});
    f.group("seed", "名字");
    f.adapter->getMembers("group-A"); ASSERT_EQ(f.requests.size(), size_t(2));
    f.answer(0, {{"members", json::array({{{"member_openid", "stale"}, {"username", "旧请求"}, {"member_role", "owner"}}})}, {"next_cursor", ""}});
    ASSERT_TRUE(f.adapter->getUserName("stale").empty());
    ASSERT_TRUE(f.adapter->getMembers("group-A").empty()); ASSERT_EQ(f.requests.size(), size_t(2));
    f.answer(1, {{"members", json::array({{{"member_openid", "current"}, {"username", "新请求"}}})}, {"next_cursor", ""}});
    ASSERT_EQ(f.adapter->getMembers("group-A").size(), size_t(1));
    ASSERT_EQ(f.adapter->getUserName("current"), "新请求");
}

TEST(OfficialNames, LeavingCancelsPendingUserQueriesWithoutBlockingOtherGroups) {
    NameFixture f(false); f.adapter->setNameTimeForTest(1000); f.group("seed", "名字");
    f.adapter->dispatchForTest("GROUP_MESSAGE_CREATE", {{"group_openid", "group-B"}, {"author", {{"member_openid", "seed"}, {"username", "名字"}}}});
    f.adapter->getUserName("missing", "group-A");
    f.adapter->dispatchForTest("GROUP_DEL_ROBOT", {{"group_openid", "group-A"}});
    f.adapter->setNameTimeForTest(1004); f.adapter->maintainNamesForTest();
    ASSERT_EQ(f.requests.size(), size_t(2)); ASSERT_EQ(f.requests[1].path, "/v2/groups/group-B/info");
    f.answer(0, {{"member_openid", "missing"}, {"username", "旧查询"}});
    ASSERT_TRUE(f.adapter->getUserName("missing").empty());
}
