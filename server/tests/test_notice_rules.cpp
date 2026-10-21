#include "../src/service/notice_manager.h"
#include "../src/service/notice_rules.h"
#include "../src/service/notice_search.h"
#include "test_framework.h"
#include <chrono>
#include <filesystem>
using namespace dice::notice_rules;
namespace {
Json chat() {
    return {{"kind", "chat"},
            {"platform", "onebot_v11"},
            {"chat_id", "12345"},
            {"is_group", false},
            {"delivery", "auto"}};
}
Json rule() { return normalize({{"channels", Json::array({chat()})}}); }
bool rejected(const Json &r) {
    try {
        normalize(r);
        return false;
    } catch (...) {
        return true;
    }
}
} // namespace
TEST(NoticeRules, ImportedEmptyEventListsUseTheirMaskWithoutBeingRewritten) {
    Json old = {{"windows", Json::array({{{"platform", "onebot_v11"},
                                          {"chat_id", "12345"},
                                          {"is_group", false},
                                          {"level_mask", 10},
                                          {"events", Json::array()}}})}};
    const auto before = old.dump();
    const auto rows = all(old);
    ASSERT_EQ(rows.size(), size_t(1));
    ASSERT_EQ(rows[0]["subscription"], Json("levels"));
    ASSERT_FALSE(subscribed(rows[0], 1, "trust"));
    ASSERT_TRUE(subscribed(rows[0], 2, "friend_add"));
    ASSERT_TRUE(subscribed(rows[0], 8, "error"));
    ASSERT_EQ(before, old.dump());
}
TEST(NoticeRules, ExplicitEmptyEventsAndZeroMasksReallyDisableDelivery) {
    auto r = rule();
    r["subscription"] = "events";
    r["events"] = Json::array();
    ASSERT_FALSE(subscribed(r, 2, "friend_add"));
    r["subscription"] = "levels";
    r["level_mask"] = 0;
    ASSERT_FALSE(subscribed(r, 8, "error"));
    r["level_mask"] = 15;
    r["enabled"] = false;
    ASSERT_FALSE(subscribed(r, 8, "error"));
}
TEST(NoticeRules, SourceSelectionsAreIndependentAndSystemNotificationsAreExplicit) {
    auto r = rule();
    r["source"] = {{"mode", "accounts"}, {"adapter_ids", Json::array({"a", "b"})}, {"include_system", false}};
    ASSERT_TRUE(sourceMatches(r, "qq_official", "b"));
    ASSERT_FALSE(sourceMatches(r, "onebot_v11", "c"));
    ASSERT_FALSE(sourceMatches(r, "", ""));
    r["source"]["include_system"] = true;
    ASSERT_TRUE(sourceMatches(r, "", ""));
    r["source"] = {{"mode", "platforms"}, {"platforms", Json::array({"milky", "onebot_v11"})}};
    ASSERT_TRUE(sourceMatches(r, "milky", "a"));
    ASSERT_FALSE(sourceMatches(r, "qq_official", "a"));
}
TEST(NoticeRules, RuleValidationRejectsUnsafeAndIncompleteChannels) {
    auto r = rule();
    r["channels"] = Json::array();
    ASSERT_TRUE(rejected(r));
    r = rule();
    r["channels"][0]["delivery"] = "selected";
    ASSERT_TRUE(rejected(r));
    r = rule();
    r["channels"] = Json::array({{{"kind", "webhook"}, {"url", "https://example.com/\nheader=x"}}});
    ASSERT_TRUE(rejected(r));
    r["channels"] = Json::array({{{"kind", "smtp"}, {"to", "me@example.com\r\nBcc:evil@example.com"}}});
    ASSERT_TRUE(rejected(r));
    r = rule();
    r["source"] = {{"mode", "accounts"}, {"adapter_ids", Json::array()}};
    ASSERT_TRUE(rejected(r));
    r = rule();
    r["events"] = Json::array({"not-registered"});
    ASSERT_TRUE(rejected(r));
    r = rule();
    r["name"] = "数据库报警";
    r["channels"].push_back(
        {{"kind", "webhook"}, {"url", "https://example.com/private-path?key=fake-private-secret"}});
    ASSERT_TRUE(matchesSearch(r, "数据库"));
    ASSERT_TRUE(matchesSearch(r, "12345"));
    ASSERT_FALSE(matchesSearch(r, "fake-private-secret"));
    ASSERT_FALSE(matchesSearch(r, "private-path"));
}
TEST(NoticeRules, SharedSmtpIsReusedIndependentlyOfVerificationSwitchAndLegacyIsRetained) {
    Json root = {{"identity_email",
                  {{"host", "shared.example"}, {"enabled", false}, {"pass", "not-a-real-secret"}}}},
         old = {{"smtp", {{"host", "legacy.example"}, {"to", "old@example.com"}}}};
    auto s = smtpTransport(root, old, {{"to", "new@example.com"}});
    ASSERT_EQ(s["host"], Json("shared.example"));
    ASSERT_EQ(s["to"], Json("new@example.com"));
    ASSERT_EQ(s["pass"], Json("not-a-real-secret"));
    ASSERT_EQ(smtpTransport(root, old, {{"transport", "legacy"}})["host"], Json("legacy.example"));
    ASSERT_EQ(smtpTransport(Json::object(), old, Json::object())["host"], Json("legacy.example"));
}
TEST(NoticeRules, EditingLegacyRuleDoesNotLeaveTheOldDeliveryEnabled) {
    Json old = {{"smtp", {{"host", "legacy.example"}, {"to", "a@example.com"}, {"enabled", true}}},
                {"webhook", {{"url", "https://example.com/hook"}, {"enabled", true}}}};
    ASSERT_EQ(all(old).size(), size_t(2));
    ASSERT_TRUE(remove(old, "legacy-smtp"));
    ASSERT_EQ(all(old).size(), size_t(1));
    ASSERT_EQ(old["smtp"]["host"], Json("legacy.example"));
    auto r = rule();
    r["id"] = "rule-new";
    old["rules"].push_back(r);
    ASSERT_EQ(all(old).size(), size_t(2));
    ASSERT_TRUE(remove(old, "rule-new"));
    ASSERT_FALSE(remove(old, "not-present"));
}
TEST(NoticeRules, CommandOffKeepsOtherDestinationsAndSelectedAccounts) {
    auto r = rule();
    r["channels"][0]["delivery"] = "selected";
    r["channels"][0]["adapter_ids"] = Json::array({"a", "b"});
    r["channels"].push_back({{"kind", "smtp"}, {"to", "a@example.com"}});
    Json conf = {{"rules", Json::array({r})}};
    removeChat(conf, "onebot_v11", "a", "12345", false);
    ASSERT_EQ(conf["rules"][0]["channels"].size(), size_t(2));
    ASSERT_EQ(conf["rules"][0]["channels"][0]["adapter_ids"], Json::array({"b"}));
    removeChat(conf, "onebot_v11", "b", "12345", false);
    ASSERT_EQ(conf["rules"][0]["channels"].size(), size_t(1));
    ASSERT_EQ(conf["rules"][0]["channels"][0]["kind"], Json("smtp"));
}

namespace {
class NoticeAdapter final : public dice::IAdapter {
    std::string id_, platform_;

  public:
    bool online = true;
    std::map<std::string, std::string> users, groups;
    std::vector<dice::Message> sent;
    NoticeAdapter(std::string id, std::string platform)
        : id_(std::move(id)), platform_(std::move(platform)) {}
    std::string id() const override { return id_; }
    std::string name() const override { return id_; }
    std::string platform() const override { return platform_; }
    std::string version() const override { return "test"; }
    bool configure(const Json &) override { return true; }
    bool start() override { return true; }
    void stop() override {}
    bool isConnected() const override { return online; }
    std::string lastError() const override { return {}; }
    void sendMessage(const dice::Message &m) override { sent.push_back(m); }
    void sendReply(const dice::Message &, const std::string &) override {}
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return id_; }
    std::string getLoginName() const override { return id_; }
    std::string getGroupName(const std::string &id) const override {
        auto it = groups.find(id);
        return it == groups.end() ? std::string() : it->second;
    }
    std::string getUserName(const std::string &id, const std::string & = {}) const override {
        auto it = users.find(id);
        return it == users.end() ? std::string() : it->second;
    }
    std::vector<std::string> getGroupMemberList(const std::string &) const override { return {}; }
    bool isGroupAdmin(const std::string &, const std::string &) const override { return false; }
    bool isGroupOwner(const std::string &, const std::string &) const override { return false; }
    void setGroupKick(const std::string &, const std::string &) override {}
    void setGroupBan(const std::string &, const std::string &, int) override {}
};
struct NoticePaths {
    std::filesystem::path before = std::filesystem::current_path(),
                          root =
                              std::filesystem::temp_directory_path() /
                              ("dicenext-notice-" +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    NoticePaths() {
        std::filesystem::create_directories(root);
        std::filesystem::current_path(root);
    }
    ~NoticePaths() {
        std::filesystem::current_path(before);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
} // namespace
TEST(NoticeRules, ActualDispatcherRespectsSelectedAccountsAndDoesNotFallbackOrSendTwice) {
    NoticePaths paths;
    dice::Database db;
    ASSERT_TRUE(db.open((paths.root / "test.db").string()));
    dice::ConfigManager cfg((paths.root / "config").string());
    dice::AdapterManager adapters(db);
    auto a = std::make_shared<NoticeAdapter>("1", "onebot_v11"),
         b = std::make_shared<NoticeAdapter>("2", "onebot_v11"),
         other = std::make_shared<NoticeAdapter>("3", "qq_official");
    adapters.registerAdapter(a);
    adapters.registerAdapter(b);
    adapters.registerAdapter(other);
    auto r = rule();
    r["id"] = "rule-test";
    r["channels"][0]["delivery"] = "selected";
    r["channels"][0]["adapter_ids"] = Json::array({"1", "2", "3"});
    b->online = false;
    cfg.set<Json>("dice/notice", {{"rules", Json::array({r, r})}});
    dice::notice::notify(cfg, adapters, 2, "fake notification", "onebot_v11", "elsewhere", "friend_add", "2");
    ASSERT_EQ(a->sent.size(), size_t(1));
    ASSERT_EQ(b->sent.size(), size_t(0));
    ASSERT_EQ(other->sent.size(), size_t(0));
    ASSERT_EQ(a->sent[0].targetId, std::string("12345"));
    a->online = false;
    b->online = true;
    r["channels"][0]["adapter_ids"] = Json::array({"1"});
    cfg.set<Json>("dice/notice", {{"rules", Json::array({r})}});
    dice::notice::notify(cfg, adapters, 2, "fake notification", "onebot_v11", "elsewhere", "friend_add", "2");
    ASSERT_EQ(b->sent.size(), size_t(0));
    r["channels"][0]["delivery"] = "auto";
    cfg.set<Json>("dice/notice", {{"rules", Json::array({r})}});
    dice::notice::notify(cfg, adapters, 2, "fake notification", "onebot_v11", "elsewhere", "friend_add", "1");
    ASSERT_EQ(b->sent.size(), size_t(1));
}
TEST(NoticeRules, ActualDispatcherDoesNotSendExplicitlyEmptySubscriptionsAndTargetsTests) {
    NoticePaths paths;
    dice::Database db;
    ASSERT_TRUE(db.open((paths.root / "test.db").string()));
    dice::ConfigManager cfg((paths.root / "config").string());
    dice::AdapterManager adapters(db);
    auto a = std::make_shared<NoticeAdapter>("1", "onebot_v11");
    adapters.registerAdapter(a);
    auto r = rule();
    r["id"] = "only";
    r["subscription"] = "events";
    cfg.set<Json>("dice/notice", {{"rules", Json::array({r})}});
    dice::notice::notify(cfg, adapters, 2, "fake notification", "", "", "friend_add");
    ASSERT_TRUE(a->sent.empty());
    dice::notice::notify(cfg, adapters, 2, "fake test", "", "", "test", "", "different");
    ASSERT_TRUE(a->sent.empty());
    dice::notice::notify(cfg, adapters, 2, "fake test", "", "", "test", "", "only");
    ASSERT_EQ(a->sent.size(), size_t(1));
}

TEST(NoticeContent, ContextContainsRealNamesAccountGroupPluginTriggerAndErrorDetails) {
    dice::ProcessingOrigin origin;
    origin.platform = "onebot_v11";
    origin.adapterId = "7";
    origin.accountId = "10000";
    origin.accountName = "小羊";
    origin.userId = "12345";
    origin.userName = "随春生";
    origin.groupId = "90001";
    origin.groupName = "周末调查团";
    origin.messageId = "incoming-42";
    dice::ProcessingContext processing{
        {{"Lua 插件", "求签", "plugin/求签/main.lua", "draw"}}, ".求签", "执行失败", origin};
    dice::notice::ErrorReport report{{{"id", "log-123"},
                                      {"module", "Lua 插件"},
                                      {"message", "main.lua:27: attempt to index a nil value"},
                                      {"details", {{"reason", "缺少签文配置"}, {"code", 42}}}},
                                     processing};
    const auto text =
        dice::notice::notificationText(8, "error", report.text(), processing, "2026-10-21 04:00:00");
    for (const auto *expected :
         {"随春生(12345)", "周末调查团(90001)", "小羊(10000)", "Lua 插件「求签」", "plugin/求签/main.lua",
          ".求签", "main.lua:27", "缺少签文配置", "错误码：42", "log-123", "incoming-42"})
        ASSERT_TRUE(text.find(expected) != std::string::npos);
}
TEST(NoticeContent, PrivacyAndMalformedUtf8AreHandledWithoutEnablingCqActions) {
    const auto text = dice::notice::readable(
        std::string("报错 token=not-a-real-secret [CQ:at,qq=all] bad-byte-") + char(0xba));
    ASSERT_TRUE(text.find("not-a-real-secret") == std::string::npos);
    ASSERT_TRUE(text.find("[CQ:") == std::string::npos);
    ASSERT_TRUE(text.find("qq=all") != std::string::npos);
    ASSERT_TRUE(Json::parse(Json(text).dump()).is_string());
    ASSERT_TRUE(dice::notice::triggerText("用户消息：.master 123456").find("123456") == std::string::npos);
    ASSERT_TRUE(dice::notice::triggerText("。email verify 123456").find("123456") == std::string::npos);
    ASSERT_EQ(dice::notice::identity("12345", "12345"), std::string("用户(12345)"));
    ASSERT_TRUE(dice::notice::identity("昵称\n伪造标题", "12345").find('\n') == std::string::npos);
    const auto shortened = dice::notice::readable("测试测试测试", 5);
    ASSERT_TRUE(Json::parse(Json(shortened).dump()).is_string());
}
TEST(NoticeContent, CensorNoticeDoesNotPublishInterceptedTextFromProcessingContext) {
    dice::ProcessingContext processing{
        {{"内置指令", ".r 被拦截原文", "", ""}}, "用户消息：被拦截原文", "已拦截"};
    const auto text = dice::notice::notificationText(2, "censor", "命中 2 条规则，详情仅保存在本机日志。",
                                                     processing, "2026-10-21");
    ASSERT_TRUE(text.find("命中 2 条") != std::string::npos);
    ASSERT_TRUE(text.find("被拦截原文") == std::string::npos);
}

TEST(NoticeContent, NamesUseOnlyOriginAccountCacheThenPersistentProfiles) {
    NoticePaths paths;
    dice::Database db;
    ASSERT_TRUE(db.open((paths.root / "test.db").string()));
    dice::AdapterManager adapters(db);
    auto a = std::make_shared<NoticeAdapter>("1", "onebot_v11"),
         b = std::make_shared<NoticeAdapter>("2", "qq_official");
    a->users["123"] = "正确账号的昵称";
    a->groups["456"] = "正确的群名";
    b->users["123"] = "另一个帐号的名字";
    adapters.registerAdapter(a);
    adapters.registerAdapter(b);
    dice::Message message;
    message.platform = "onebot_v11";
    message.adapterId = "1";
    message.senderId = "123";
    message.targetId = "456";
    auto origin = dice::notice::messageOrigin(message, adapters);
    ASSERT_EQ(origin.userName, std::string("正确账号的昵称"));
    ASSERT_EQ(origin.groupName, std::string("正确的群名"));
    dice::PlayerProfileRow row;
    row.platform = "onebot_v11";
    row.userId = "789";
    row.nickname = "历史昵称";
    db.getStorage()->insert(row);
    origin.userId = "789";
    origin.userName.clear();
    adapters.enrichLogOrigin(origin, true);
    ASSERT_EQ(origin.userName, std::string("历史昵称"));
    origin.userId.clear();
    origin.userName.clear();
    origin.targetType = "user";
    origin.targetId = "789";
    adapters.enrichLogOrigin(origin, true);
    ASSERT_EQ(origin.targetName, std::string("历史昵称"));
    ASSERT_TRUE(dice::notice::contextText(origin).find("操作／触发者") == std::string::npos);
    ASSERT_TRUE(dice::notice::contextText(origin).find("历史昵称(789)") != std::string::npos);
}
TEST(NoticeContent, OriginSurvivesNestedAndDeferredPluginScopesWithoutLeakingToNextMessage) {
    dice::ProcessingContext deferred;
    {
        dice::ProcessingOrigin origin;
        origin.userId = "123";
        origin.groupId = "456";
        dice::ProcessingScope incoming(dice::ProcessingContext{{}, ".求签", "处理中", origin});
        {
            dice::ProcessingScope plugin(dice::ProcessingSource{"Lua 插件", "求签", "draw.lua", "draw"});
            deferred = dice::activeProcessingContext;
        }
        ASSERT_EQ(dice::activeProcessingOrigin.userId, std::string("123"));
    }
    ASSERT_TRUE(dice::activeProcessingOrigin.userId.empty());
    std::string group;
    std::thread worker([&] {
        dice::ProcessingScope resumed(deferred);
        group = dice::activeProcessingOrigin.groupId;
    });
    worker.join();
    ASSERT_EQ(group, std::string("456"));
    ASSERT_EQ(deferred.sources[0].name, std::string("求签"));
    ASSERT_TRUE(dice::activeProcessingOrigin.groupId.empty());
}
TEST(NoticeContent, DistinctPluginAndGroupErrorsAreNotGloballySilencedAndRepeatsAreCounted) {
    long long now = 1000;
    std::vector<dice::notice::ErrorReport> reports;
    dice::notice::ErrorNotifySink sink([&](const auto &r) { reports.push_back(r); }, [&] { return now; });
    const spdlog::details::log_msg error{{}, "Lua 插件", spdlog::level::err, "nil value"};
    dice::ProcessingOrigin origin;
    origin.groupId = "100";
    dice::ProcessingScope first(
        dice::ProcessingContext{{{"Lua 插件", "求签", "q.lua", "draw"}}, ".求签", "失败", origin});
    sink.log(error);
    sink.log(error);
    ASSERT_EQ(reports.size(), size_t(1));
    {
        auto context = dice::activeProcessingContext;
        context.origin.groupId = "200";
        dice::ProcessingScope second(context);
        sink.log(error);
    }
    {
        dice::ProcessingScope other(dice::ProcessingSource{"JS 插件", "DailyNews", "news.js", "news"});
        sink.log(error);
    }
    ASSERT_EQ(reports.size(), size_t(3));
    now = 1061;
    sink.log(error);
    ASSERT_EQ(reports.size(), size_t(4));
    ASSERT_EQ(reports.back().suppressed, size_t(1));
    {
        dice::ProcessingScope notification(dice::ProcessingSource{"通知", "错误通知", "", ""});
        sink.log(error);
    }
    ASSERT_EQ(reports.size(), size_t(4));
}
TEST(NoticeContent, ErrorSinkSnapshotsSanitizedStructuredRecordBeforeDeferredDelivery) {
    std::vector<dice::notice::ErrorReport> reports;
    auto sink = std::make_shared<dice::notice::ErrorNotifySink>([&](const auto &r) { reports.push_back(r); });
    auto logger = dice::getLogger();
    logger->sinks().push_back(sink);
    struct Cleanup {
        decltype(logger) log;
        decltype(sink) target;
        ~Cleanup() {
            auto &s = log->sinks();
            s.erase(std::remove(s.begin(), s.end(), target), s.end());
        }
    } cleanup{logger, sink};
    {
        dice::ProcessingScope plugin(dice::ProcessingSource{"Lua 插件", "求签", "q.lua", "draw"});
        dice::writeLog({}, dice::LogLevel::Error, "Lua 插件", "attempt to index a nil value",
                       {{"reason", "签文读取失败"}, {"password", "fake-password"}});
    }
    ASSERT_EQ(reports.size(), size_t(1));
    ASSERT_TRUE(!reports[0].record.value("id", std::string()).empty());
    ASSERT_TRUE(reports[0].record.dump().find("fake-password") == std::string::npos);
    ASSERT_EQ(reports[0].processing.sources[0].name, std::string("求签"));
    ASSERT_TRUE(reports[0].text().find("签文读取失败") != std::string::npos);
    ASSERT_TRUE(dice::currentLogRecord().empty());
    dice::writeLog({}, dice::LogLevel::Error, "自动更新", "下载失败", {{"notice_event", "update_error"}});
    ASSERT_EQ(reports.size(), size_t(1));
    dice::writeLog({}, dice::LogLevel::Error, "系统", "普通错误", {{"notice_event", 42}});
    ASSERT_EQ(reports.size(), size_t(2));
}
