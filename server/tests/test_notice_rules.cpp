#include "../src/service/notice_manager.h"
#include "../src/service/notice_rules.h"
#include "../src/service/notice_search.h"
#include "../src/service/notice_windows.h"
#include "../src/adapter/outbound_logging.h"
#include "../src/adapter/group_notice_semantics.h"
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
    std::string sendError;
    bool canLeave = false;
    std::string leaveError;
    std::vector<std::string> leaveCalls;
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
    void sendMessage(const dice::Message &m) override {
        if (!sendError.empty())
            throw std::runtime_error(sendError);
        sent.push_back(m);
    }
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
    bool supportsGroupLeave() const override { return canLeave; }
    void leaveGroup(const std::string& id) override {
        if (!leaveError.empty()) throw std::runtime_error(leaveError);
        leaveCalls.push_back(id);
    }
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

TEST(NoticeContent, ErrorContainsBusinessContextAndOriginalCauseWithoutDiagnosticReport) {
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
    dice::notice::ErrorReport report{
        {{"id", "log-123"},
         {"module", "Lua 插件"},
         {"message", "main.lua:27: attempt to index a nil value"},
         {"details", {{"operation", "执行"}, {"reason", "缺少签文配置"}, {"code", 42}}}},
        processing};
    const auto text =
        dice::notice::notificationText(8, "error", report.text(), processing, "2026-10-21 04:00:00");
    for (const auto *expected : {"[插件错误] 插件「求签」执行失败", "随春生(12345)", "周末调查团(90001)",
                                 "OneBot:10000", "main.lua:27: attempt to index a nil value", "缺少签文配置"})
        ASSERT_TRUE(text.find(expected) != std::string::npos);
    for (const auto *absent :
         {"log-123", "incoming-42", "触发内容", ".求签", "时间：", "级别：", "错误码：", "关联日志"})
        ASSERT_TRUE(text.find(absent) == std::string::npos);
    ASSERT_TRUE(text.size() < size_t(500));
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
    dice::ProcessingOrigin wholeGroup;
    wholeGroup.userId = "0"; // Whole-group mute uses 0, not a real acting user.
    ASSERT_TRUE(dice::notice::contextText(wholeGroup).empty());
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

namespace {
class NoticeRecordSink final : public spdlog::sinks::base_sink<std::mutex> {
  public:
    std::vector<Json> errors;

  protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        if (msg.level >= spdlog::level::err)
            errors.push_back(dice::currentLogRecord());
    }
    void flush_() override {}
};
struct NoticeErrorCapture {
    std::vector<dice::notice::ErrorReport> reports;
    std::shared_ptr<NoticeRecordSink> records = std::make_shared<NoticeRecordSink>();
    std::shared_ptr<dice::notice::ErrorNotifySink> notices = std::make_shared<dice::notice::ErrorNotifySink>(
        [this](const auto &report) { reports.push_back(report); });
    std::shared_ptr<spdlog::logger> logger = dice::getLogger();
    NoticeErrorCapture() {
        logger->sinks().push_back(records);
        logger->sinks().push_back(notices);
    }
    ~NoticeErrorCapture() {
        auto &sinks = logger->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), records), sinks.end());
        sinks.erase(std::remove(sinks.begin(), sinks.end(), notices), sinks.end());
    }
};
} // namespace
TEST(NoticeContent, AsynchronousNotificationFailureIsLocalOnlyAndDoesNotQuoteFailedBody) {
    NoticeErrorCapture capture;
    dice::OutboundAttempt attempt;
    {
        dice::ProcessingScope source(dice::ProcessingSource{"通知", "更新结果", "", ""});
        auto prepared = std::make_shared<dice::PreparedOutput>(
            "群测试群(192499947)", "【更新结果】secret-notification-body", dice::activeProcessingContext,
            "OneBot:114514");
        dice::OutboundScope scope(prepared);
        attempt =
            dice::OutboundAttempt::prepare("OneBotV11「llbot」", "群 192499947", "secret-notification-body");
    }
    // The real OneBot response runs after the sender's scopes have gone away.
    std::thread worker([&] { attempt.failed("平台拒绝请求（错误码 1200）：你已被移出该群"); });
    worker.join();
    ASSERT_TRUE(capture.reports.empty());
    ASSERT_EQ(capture.records->errors.size(), size_t(1));
    const auto &record = capture.records->errors[0];
    ASSERT_EQ(record["message"].get<std::string>(),
              "向群测试群(192499947)发送通知失败，请检查适配器 OneBot:114514 是否可触达该通知窗口。");
    ASSERT_TRUE(record["details"]["notification_delivery"].get<bool>());
    ASSERT_EQ(record["details"]["actual_content"].get<std::string>(), "secret-notification-body");
    ASSERT_TRUE(record["message"].get<std::string>().find("msgID") == std::string::npos);
    ASSERT_TRUE(record["details"]["error"].get<std::string>().find("1200") != std::string::npos);
    // A new notice, ID and content still represent the same unreachable window.
    {
        dice::ProcessingScope source(dice::ProcessingSource{"通知", "错误通知", "", ""});
        auto prepared = std::make_shared<dice::PreparedOutput>(
            "群测试群(192499947)", "different body", dice::activeProcessingContext, "OneBot:114514");
        dice::OutboundScope scope(prepared);
        auto next = dice::OutboundAttempt::prepare("unused", "unused", "different body");
        std::thread callback([&] { next.failed("平台拒绝请求（错误码 1200）：你已被移出该群"); });
        callback.join();
    }
    ASSERT_EQ(capture.records->errors.size(), size_t(1));
    ASSERT_TRUE(capture.reports.empty());
}
TEST(NoticeContent, NotificationFailureMergesRepeatsWithoutSilencingOtherTargetsOrReasons) {
    const auto key = dice::newLogCorrelationId();
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::seconds(100);
    ASSERT_TRUE(dice::admitNotificationFailure(key, start).first);
    ASSERT_FALSE(dice::admitNotificationFailure(key, start + std::chrono::seconds(1)).first);
    ASSERT_FALSE(dice::admitNotificationFailure(key, start + std::chrono::seconds(59)).first);
    const auto next = dice::admitNotificationFailure(key, start + std::chrono::seconds(60));
    ASSERT_TRUE(next.first);
    ASSERT_EQ(next.second, size_t(2));
    ASSERT_TRUE(dice::admitNotificationFailure(key + "other-target", start).first);
    ASSERT_TRUE(dice::admitNotificationFailure(key + "other-reason", start).first);
}
TEST(NoticeContent, DifferentPlatformActionsRemainReadableAndAreNotMergedAsTheSameFailure) {
    NoticeErrorCapture capture;
    auto kick = dice::prepareActionLog("OneBot:114517","set_group_kick",{{"group_id","90006"},{"user_id","12347"}});
    auto mute = dice::prepareActionLog("OneBot:114517","set_group_ban",{{"group_id","90006"},{"user_id","12347"},{"duration",60}});
    ASSERT_TRUE(kick.has_value());
    ASSERT_TRUE(mute.has_value());
    kick->failed("平台拒绝请求：权限不足");
    mute->failed("平台拒绝请求：权限不足");
    ASSERT_EQ(capture.reports.size(),size_t(2));
    ASSERT_TRUE(capture.reports[0].text().find("移出群成员") != std::string::npos);
    ASSERT_TRUE(capture.reports[1].text().find("设置成员禁言") != std::string::npos);
    for (const auto& report : capture.reports) {
        const auto text = report.text();
        ASSERT_TRUE(text.find("[操作错误]") != std::string::npos);
        ASSERT_TRUE(text.find("权限不足") != std::string::npos);
        ASSERT_TRUE(text.find("90006") != std::string::npos);
        ASSERT_TRUE(text.find("12347") != std::string::npos);
        ASSERT_TRUE(text.find("发送消息失败") == std::string::npos);
    }
}
TEST(NoticeContent, DeferredNormalMessageFailureUsesCapturedOriginAndStableErrorIdentity) {
    NoticeErrorCapture capture;
    dice::ProcessingOrigin origin;
    origin.platform = "onebot_v11";
    origin.adapterId = "send-test";
    origin.accountId = "114515";
    origin.groupId = "90002";
    origin.groupName = "测试团";
    origin.userId = "12346";
    origin.userName = "测试玩家";
    auto prepare = [&](const std::string &content, const std::string &target) {
        dice::ProcessingScope source(
            dice::ProcessingContext{{{"Lua 插件", "求签", "q.lua", "draw"}}, ".求签", "请求发送", origin});
        dice::OutboundScope independent(nullptr);
        return dice::OutboundAttempt::prepare("OneBot:114515", target, content);
    };
    auto first = prepare("content one", "群90002");
    auto second = prepare("content two", "群90002");
    std::thread worker([&] {
        dice::ProcessingScope unrelated(dice::ProcessingSource{"JS 插件", "其他插件", "other.js", ""});
        first.failed("平台拒绝请求：不在群内");
        second.failed("平台拒绝请求：不在群内");
    });
    worker.join();
    ASSERT_EQ(capture.reports.size(), size_t(1));
    ASSERT_EQ(capture.records->errors.size(), size_t(2)); // Real failures still have local records.
    const auto &report = capture.reports[0];
    ASSERT_EQ(report.processing.sources[0].name, std::string("求签"));
    ASSERT_EQ(report.processing.origin.groupName, std::string("测试团"));
    const auto text = report.text();
    for (const auto *expected : {"[消息错误]", "不在群内", "90002", "求签", "测试玩家(12346)"})
        ASSERT_TRUE(text.find(expected) != std::string::npos);
    for (const auto *absent : {"其他插件", "content one", "content two", "关联日志", "msgID"})
        ASSERT_TRUE(text.find(absent) == std::string::npos);
    auto other = prepare("third body", "群90003");
    other.failed("平台拒绝请求：不在群内");
    ASSERT_EQ(capture.reports.size(), size_t(2));
    other.failed("连接中断");
    ASSERT_EQ(capture.reports.size(), size_t(3));
}
TEST(NoticeContent, PluginErrorsRetainOriginalInterpreterMessageAndDoNotInventCauses) {
    NoticeErrorCapture capture;
    {
        dice::ProcessingScope source(
            dice::ProcessingSource{"JS 插件", "合成大西瓜", "watermelon.js", "加载"});
        dice::LogOperation operation(DICE_LOG_SOURCE, "js", "load_plugin", "watermelon.js");
        dice::logPluginFailure("加载", "SyntaxError: unexpected token\n    at watermelon.js:123:7");
        operation.fail("script evaluation failed", true);
    }
    ASSERT_EQ(capture.reports.size(), size_t(1));
    auto text = capture.reports[0].text();
    ASSERT_TRUE(text.find("[插件错误] 插件「合成大西瓜」加载失败：SyntaxError: unexpected token") !=
                std::string::npos);
    ASSERT_TRUE(text.find("watermelon.js:123:7") != std::string::npos);
    ASSERT_TRUE(text.find("    | ") == std::string::npos);
    for (const auto *absent : {"中文逗号", "英文逗号", "请替换", "关联日志", "模块：", "触发内容："})
        ASSERT_TRUE(text.find(absent) == std::string::npos);
    {
        dice::ProcessingScope source(dice::ProcessingSource{"Lua 插件", "求签", "q.lua", "draw"});
        dice::logPluginText(dice::LogLevel::Error, "插件主动打印：签文不存在");
    }
    ASSERT_EQ(capture.reports.size(), size_t(2));
    text = capture.reports[1].text();
    ASSERT_TRUE(text.find("求签") != std::string::npos);
    ASSERT_TRUE(text.find("插件主动打印：签文不存在") != std::string::npos);
    ASSERT_TRUE(text.find("执行失败") == std::string::npos);
}
TEST(NoticeContent, OldNestedErrorBodiesAndMalformedDetailsDoNotBecomeNotificationReports) {
    dice::ProcessingContext source;
    const std::string prefix = "向群90004发送消息失败：你已被移出该群";
    dice::notice::ErrorReport report{{{"module", "消息"},
                                      {"message", prefix + "；实际内容：【运行错误】nested-body (msgID:old)"},
                                      {"details", {{"code", 1200}, {"error", Json::array()}}}},
                                     source};
    ASSERT_EQ(report.text(), "[消息错误] " + prefix);
    const auto restored = dice::processingContextFromJson(
        {{"sources", Json::array({nullptr, {{"kind", "通知"}, {"name", 123}}})},
         {"origin", {{"group_id", 12}, {"user_name", Json::array()}}}});
    ASSERT_TRUE(dice::isNotificationProcessing(restored));
    ASSERT_TRUE(restored.origin.groupId.empty());
    ASSERT_TRUE(restored.origin.userName.empty());
}
TEST(NoticeContent, NormalUpdateNotificationKeepsReleaseNotesWithoutReportBoilerplate) {
    const auto text = dice::notice::notificationText(
        2, "update_available",
        "发现新版本 "
        "v3.0.0-beta.test\n本次更新内容：\n修复通知递归与预览格式\n发布页：https://example.com/release",
        {}, "2026-10-21 04:00:00");
    for (const auto *expected :
         {"[发现新版本]", "本次更新内容：", "修复通知递归与预览格式", "https://example.com/release"})
        ASSERT_TRUE(text.find(expected) != std::string::npos);
    for (const auto *absent : {"时间：", "级别：", "关联日志", "触发内容"})
        ASSERT_TRUE(text.find(absent) == std::string::npos);
}
TEST(NoticeContent, OfflineAndThrowingNotificationChannelsAreLocalErrorsNotRecursiveNotices) {
    NoticePaths paths;
    dice::ConfigManager cfg((paths.root / "config").string());
    dice::Database db;
    ASSERT_TRUE(db.open((paths.root / "test.db").string()));
    dice::AdapterManager adapters(db);
    auto adapter = std::make_shared<NoticeAdapter>("114516", "onebot_v11");
    adapter->groups["90005"] = "通知测试群";
    adapters.registerAdapter(adapter);
    auto r = rule();
    r["channels"][0]["chat_id"] = "90005";
    r["channels"][0]["is_group"] = true;
    r["channels"][0]["delivery"] = "selected";
    r["channels"][0]["adapter_ids"] = Json::array({"114516"});
    cfg.set<Json>("dice/notice", {{"rules", Json::array({r})}});
    NoticeErrorCapture capture;
    adapter->online = false;
    dice::notice::notify(cfg, adapters, 2, "offline test", "", "", "friend_add");
    adapter->online = true;
    adapter->sendError = "模拟发送接口异常";
    dice::notice::notify(cfg, adapters, 2, "throw test", "", "", "friend_add");
    ASSERT_TRUE(adapter->sent.empty());
    ASSERT_TRUE(capture.reports.empty());
    ASSERT_EQ(capture.records->errors.size(), size_t(2));
    for (const auto &record : capture.records->errors) {
        ASSERT_TRUE(record["message"].get<std::string>().find("群通知测试群(90005)") != std::string::npos);
        ASSERT_TRUE(record["message"].get<std::string>().find("OneBot:114516") != std::string::npos);
        ASSERT_TRUE(record["details"]["notification_delivery"].get<bool>());
    }
}

namespace {
dice::BotEvent departure(const std::string& type) {
    dice::BotEvent e; e.type = dice::EventType::kGroupDecrease; e.subType = type;
    e.groupId = "792364383"; e.selfId = "3626455591"; e.userId = e.selfId;
    e.operatorId = "3569827618"; return e;
}
dice::notice::LeaveIntent dismissIntent() {
    dice::notice::LeaveIntent intent; intent.operation = "dismiss"; intent.cause = "指令 .dismiss";
    intent.groupName = "未归星璇"; intent.actor.userId = "1707642"; intent.actor.userName = "希亚";
    return intent;
}
}
TEST(NoticeEvents, KickerAndDissolverAlwaysOverrideRecentLeaveRequest) {
    for (const auto* subtype : {"kick", "kick_me", "removed"}) {
        const auto text = dice::notice::groupDeparture(departure(subtype), "未归星璇", "管理员小明", dismissIntent());
        ASSERT_EQ(text.operation, std::string("group_left"));
        ASSERT_EQ(text.title, std::string("骰娘被踢出群聊"));
        ASSERT_EQ(text.body, std::string("骰娘被管理员小明(3569827618)踢出群未归星璇(792364383)。"));
        ASSERT_TRUE(text.body.find("dismiss") == std::string::npos);
    }
    const auto text = dice::notice::groupDeparture(departure("disband"), "未归星璇", "群主", dismissIntent());
    ASSERT_EQ(text.title, std::string("群聊已解散"));
    ASSERT_TRUE(text.body.find("群主(3569827618)解散") != std::string::npos);
    ASSERT_TRUE(text.body.find("dismiss") == std::string::npos);
}
TEST(NoticeEvents, SubmittedRequestIsNotConfirmationAndUnknownEventsNeverInventCause) {
    auto intent = dismissIntent(); intent.reason = "骰主主动请离";
    const auto requested = dice::notice::leaveRequested("792364383", intent);
    ASSERT_EQ(requested.operation, std::string("dismiss"));
    ASSERT_TRUE(requested.body.find("希亚(1707642)通过指令 .dismiss") != std::string::npos);
    ASSERT_TRUE(requested.body.find("等待平台确认") != std::string::npos);
    auto event = departure("leave");
    const auto confirmed = dice::notice::groupDeparture(event, "", "", intent);
    ASSERT_EQ(confirmed.title, std::string("骰娘退群已确认"));
    ASSERT_TRUE(confirmed.body.find("未归星璇(792364383)") != std::string::npos);
    ASSERT_TRUE(confirmed.body.find("骰主主动请离") != std::string::npos);
    const auto external = dice::notice::groupDeparture(event, "未归星璇", "");
    ASSERT_EQ(external.title, std::string("骰娘主动退出群聊"));
    ASSERT_TRUE(external.body.find("未关联") != std::string::npos);
    event.subType.clear();
    const auto unknown = dice::notice::groupDeparture(event, "未归星璇", "", intent);
    ASSERT_EQ(unknown.title, std::string("骰娘离群（原因未知）"));
    ASSERT_TRUE(unknown.body.find("不能据此认定") != std::string::npos);
    event.subType = "kick_me"; event.operatorId.clear();
    ASSERT_TRUE(dice::notice::groupDeparture(event, "", "").body.find("平台未提供操作者") != std::string::npos);
}
TEST(NoticeEvents, LeaveCorrelationIsConsumedScopedExpiredAndBounded) {
    dice::notice::LeaveIntents records;
    const auto time = dice::notice::LeaveIntents::Clock::now();
    auto old = records.remember("a", "onebot_v11", "g", dismissIntent(), time);
    auto newer = records.remember("a", "onebot_v11", "g", dismissIntent(), time);
    records.cancel(old);
    ASSERT_FALSE(records.take("b", "onebot_v11", "g", time).has_value());
    ASSERT_FALSE(records.take("a", "milky", "g", time).has_value());
    ASSERT_TRUE(records.take("a", "onebot_v11", "g", time).has_value());
    ASSERT_FALSE(records.take("a", "onebot_v11", "g", time).has_value());
    records.remember("a", "onebot_v11", "g", dismissIntent(), time);
    ASSERT_FALSE(records.take("a", "onebot_v11", "g", time + std::chrono::minutes(5)).has_value());
    for (int i = 0; i != 513; ++i)
        records.remember("a", "onebot_v11", std::to_string(i), dismissIntent(), time + std::chrono::milliseconds(i));
    ASSERT_FALSE(records.take("a", "onebot_v11", "0", time + std::chrono::seconds(1)).has_value());
    ASSERT_TRUE(records.take("a", "onebot_v11", "512", time + std::chrono::seconds(1)).has_value());
}
TEST(NoticeEvents, ModerationDistinguishesDurationUnmuteWholeGroupAndAdminChanges) {
    auto e = departure("ban"); e.type = dice::EventType::kGroupBan; e.extra = {{"duration", 90061}};
    auto text = dice::notice::groupModeration(e, "未归星璇", "小明");
    ASSERT_TRUE(text.body.find("被小明(3569827618)禁言1天1小时1分钟1秒") != std::string::npos);
    ASSERT_EQ(dice::notice::durationText("3600"), std::string("（平台未提供有效时长）"));
    ASSERT_EQ(dice::notice::durationText(-1), std::string("（平台未提供有效时长）"));
    ASSERT_EQ(dice::notice::durationText(UINT64_MAX), std::string("（平台未提供有效时长）"));
    e.subType = "lift_ban";
    ASSERT_EQ(dice::notice::groupModeration(e, "未归星璇", "小明").title, std::string("骰娘禁言已解除"));
    e.subType = "whole_ban"; e.userId = "0";
    ASSERT_TRUE(dice::notice::groupModeration(e, "未归星璇", "小明").body.find("全体禁言") != std::string::npos);
    e.type = dice::EventType::kGroupAdmin; e.subType = "set";
    ASSERT_EQ(dice::notice::groupModeration(e, "未归星璇", "小明").title, std::string("骰娘获得群管理权限"));
    e.subType = "unset"; e.operatorId.clear();
    text = dice::notice::groupModeration(e, "未归星璇", "");
    ASSERT_TRUE(text.body.find("取消了管理员权限") != std::string::npos);
    ASSERT_TRUE(text.body.find("平台未提供操作者") != std::string::npos);
    e.subType = "unknown";
    ASSERT_TRUE(dice::notice::groupModeration(e, "未归星璇", "").body.find("未提供设置或取消") != std::string::npos);
}
TEST(NoticeEvents, AdaptersNormalizeOnlyReportedModerationAndDepartureSemantics) {
    dice::BotEvent e;
    ASSERT_FALSE(dice::oneBotModerationEvent(e, "group_ban", Json::array()));
    ASSERT_TRUE(dice::oneBotModerationEvent(e, "group_ban", {{"group_id", 12}, {"user_id", 34}, {"operator_id", 56}, {"sub_type", "lift_ban"}}));
    ASSERT_TRUE(e.type == dice::EventType::kGroupBan);
    ASSERT_EQ(e.operatorId, std::string("56")); ASSERT_EQ(e.subType, std::string("lift_ban"));
    ASSERT_TRUE(dice::milkyModerationEvent(e, "group_mute", {{"duration", 0}}));
    ASSERT_EQ(e.subType, std::string("lift_ban"));
    ASSERT_TRUE(dice::milkyModerationEvent(e, "group_mute", {{"duration", "0"}}));
    ASSERT_EQ(e.subType, std::string("unknown"));
    ASSERT_TRUE(dice::milkyModerationEvent(e, "group_admin_change", {{"is_set", true}}));
    ASSERT_TRUE(e.type == dice::EventType::kGroupAdmin); ASSERT_EQ(e.subType, std::string("set"));
    ASSERT_TRUE(dice::milkyModerationEvent(e, "group_whole_mute", {{"is_mute", false}}));
    ASSERT_EQ(e.userId, std::string("0")); ASSERT_EQ(e.subType, std::string("whole_lift_ban"));
    ASSERT_EQ(dice::milkyDepartureType("group_member_decrease", {{"operator_id", 56}}), std::string("kick"));
    ASSERT_EQ(dice::milkyDepartureType("group_member_decrease", Json::object()), std::string("leave"));
    ASSERT_EQ(dice::milkyDepartureType("group_disband", Json::object()), std::string("disband"));
}
TEST(NoticeEvents, RequestDecisionIncludesOutcomeReasonAndCommentWithoutClaimingApprovalConfirmed) {
    dice::BotEvent e; e.type = dice::EventType::kFriendRequest; e.userId = "1707642"; e.comment = "跑团邀请";
    auto text = dice::notice::requestDecision(e, "希亚", "", "等待人工处理", "当前好友策略为手动处理");
    ASSERT_EQ(text.operation, std::string("friend_req"));
    for (const auto* expected : {"希亚(1707642)", "等待人工处理", "手动处理", "跑团邀请"})
        ASSERT_TRUE(text.body.find(expected) != std::string::npos);
    e.type = dice::EventType::kGroupRequest; e.subType = "invite"; e.groupId = "792364383";
    text = dice::notice::requestDecision(e, "希亚", "未归星璇", "已提交自动同意", "当前策略允许加群");
    ASSERT_EQ(text.operation, std::string("group_invite"));
    ASSERT_TRUE(text.body.find("邀请骰娘加入群未归星璇(792364383)") != std::string::npos);
    ASSERT_TRUE(text.body.find("已提交") != std::string::npos);
    e.subType = "add";
    ASSERT_EQ(dice::notice::requestDecision(e, "希亚", "未归星璇", "等待人工处理", "").operation, std::string("group_request"));
}
TEST(NoticeEvents, ManagementTitlesRemainSpecificAndPluginActorIsPreserved) {
    const dice::ProcessingContext plugin{{{"JS 插件", "群管助手", "helper.js", ""}}, "", ""};
    for (int list = 0; list != 3; ++list) {
        const auto event = dice::notice::listChanged(1, list, "792364383", "未归星璇", true, "骰主操作");
        const auto text = dice::notice::notificationText(event.level, event.operation, event.body, plugin, "", event.title);
        ASSERT_TRUE(text.find("插件「群管助手」：已将群未归星璇(792364383)加入") != std::string::npos);
    }
    ASSERT_EQ(dice::notice::levelNames(15), std::string("例行、重要、关键、错误"));
    ASSERT_EQ(dice::notice::levelNames(10), std::string("重要、错误"));
    ASSERT_TRUE(dice::notice::ruleChanged("", "删除", 1).body.find("未命名规则") != std::string::npos);
    dice::ScheduledTaskRow task; task.name = "每日通知"; task.triggerType = "interval"; task.intervalMin = 30; task.enabled = 1;
    ASSERT_TRUE(dice::notice::scheduleChanged(task, "新增").body.find("每 30 分钟") != std::string::npos);
}
TEST(NoticeEvents, LeaveSubmissionValidatesAccountCapabilityAndCancelsFailures) {
    NoticePaths paths; dice::Database db;
    ASSERT_TRUE(db.open((paths.root / "test.db").string()));
    dice::ConfigManager cfg((paths.root / "config").string()); dice::AdapterManager adapters(db);
    auto a = std::make_shared<NoticeAdapter>("notice-leave-test", "onebot_v11");
    adapters.registerAdapter(a); cfg.set<Json>("dice/notice", {{"rules", Json::array({rule()})}});
    a->groups["792364383"] = "未归星璇";
    auto intent = dismissIntent();
    ASSERT_FALSE(dice::notice::requestGroupLeave(cfg, adapters, a, "792364383", intent));
    ASSERT_TRUE(a->leaveCalls.empty());
    a->canLeave = true; a->leaveError = "权限不足";
    ASSERT_FALSE(dice::notice::requestGroupLeave(cfg, adapters, a, "792364383", intent));
    ASSERT_FALSE(dice::notice::LeaveIntents::instance().take(a->id(), a->platform(), "792364383").has_value());
    a->leaveError.clear();
    ASSERT_TRUE(dice::notice::requestGroupLeave(cfg, adapters, a, "792364383", intent));
    ASSERT_EQ(a->leaveCalls.size(), size_t(1));
    ASSERT_TRUE(dice::notice::LeaveIntents::instance().take(a->id(), a->platform(), "792364383").has_value());
    ASSERT_TRUE(a->sent.back().content.find("等待平台确认") != std::string::npos);
    a->online = false;
    ASSERT_FALSE(dice::notice::requestGroupLeave(cfg, adapters, a, "792364383", intent));
    ASSERT_EQ(a->leaveCalls.size(), size_t(1));
}

TEST(NoticeEvents, EverySubscriptionHasAHumanTitleAndAnEntryInTheReviewDocument) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "docs" / "notification-templates.md";
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.good());
    const std::string review{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::set<std::string> seen;
    for (const auto& [op, level] : dice::notice_rules::catalog()) {
        ASSERT_TRUE(seen.insert(op).second);
        ASSERT_TRUE(dice::notice::eventTitle(op).find("通知 · ") != 0);
        ASSERT_TRUE(review.find("| `" + op + "` |") != std::string::npos);
        ASSERT_TRUE(level == 1 || level == 2 || level == 4 || level == 8);
    }
}

TEST(NoticeWindows, SameDestinationIsOneWindowWithoutBroadeningSourceAndEventPairs) {
    auto a = rule(), b = rule(); a["id"] = "a"; b["id"] = "b";
    a["subscription"] = b["subscription"] = "events";
    a["events"] = Json::array({"friend_add"}); b["events"] = Json::array({"error"});
    a["source"] = {{"mode", "accounts"}, {"adapter_ids", Json::array({"source-a"})}, {"include_system", false}};
    b["source"] = {{"mode", "accounts"}, {"adapter_ids", Json::array({"source-b"})}, {"include_system", false}};
    b["channels"][0]["delivery"] = "selected"; b["channels"][0]["adapter_ids"] = Json::array({"sender-b"});
    Json conf{{"rules", Json::array({a, b})}}; const auto before = conf.dump();
    const auto windows = dice::notice_windows::all(conf);
    ASSERT_EQ(windows.size(), size_t(1)); ASSERT_EQ(windows[0]["subscriptions"].size(), size_t(2));
    ASSERT_TRUE(windows[0]["mixed_delivery"].get<bool>()); ASSERT_EQ(before, conf.dump());
    const auto normalized = dice::notice_windows::normalize(windows[0]);
    dice::notice_windows::erase(conf, windows[0]["channel"]); dice::notice_windows::store(conf, normalized, "saved");
    ASSERT_EQ(all(conf).size(), size_t(2));
    auto matches = [&](const std::string& account, const std::string& event) {
        for (const auto& r : all(conf)) if (sourceMatches(r, "onebot_v11", account) && subscribed(r, event == "error" ? 8 : 2, event)) return true;
        return false;
    };
    ASSERT_TRUE(matches("source-a", "friend_add")); ASSERT_TRUE(matches("source-b", "error"));
    ASSERT_FALSE(matches("source-a", "error")); ASSERT_FALSE(matches("source-b", "friend_add"));
}
TEST(NoticeWindows, TestingAPausedWindowDoesNotDeliverToOtherDestinationsInItsHistoricalRule) {
    NoticePaths paths; dice::Database db;
    ASSERT_TRUE(db.open((paths.root / "test.db").string()));
    dice::ConfigManager cfg((paths.root / "config").string()); dice::AdapterManager adapters(db);
    auto a = std::make_shared<NoticeAdapter>("notice-window-test", "onebot_v11"); adapters.registerAdapter(a);
    auto r = rule(); r["id"] = "shared-old-rule"; r["enabled"] = false;
    auto other = chat(); other["chat_id"] = "54321"; r["channels"].push_back(other);
    cfg.set<Json>("dice/notice", {{"rules", Json::array({r})}});
    const std::set<std::string> ids{"shared-old-rule"}; const auto destination = chat();
    dice::notice::notify(cfg, adapters, 2, "fixture", "", "", "test", "", "", "", &ids, &destination);
    ASSERT_EQ(a->sent.size(), size_t(1)); ASSERT_EQ(a->sent[0].targetId, std::string("12345"));
    a->sent.clear();
    dice::notice::notify(cfg, adapters, 2, "global fixture", "", "", "test");
    ASSERT_TRUE(a->sent.empty()); // A global test must not revive paused destinations.
}
TEST(NoticeWindows, RemovingOneDestinationPreservesOtherChannelsAndLegacyTransportSettings) {
    auto r = rule(); r["id"] = "multi";
    r["channels"].push_back({{"kind", "smtp"}, {"to", "owner@example.com"}});
    Json conf{{"rules", Json::array({r})}, {"webhook", {{"url", "https://example.com/hook"}, {"enabled", true}}},
        {"smtp", {{"host", "old.example.com"}, {"to", "owner@example.com"}, {"enabled", true}}}};
    dice::notice_windows::erase(conf, chat());
    ASSERT_EQ(conf["rules"][0]["channels"].size(), size_t(1));
    dice::notice_windows::erase(conf, {{"kind", "smtp"}, {"to", "owner@example.com"}});
    ASSERT_TRUE(conf["rules"].empty()); ASSERT_TRUE(conf["smtp"]["rule_hidden"].get<bool>());
    ASSERT_EQ(conf["smtp"]["host"], Json("old.example.com")); ASSERT_TRUE(conf["webhook"]["enabled"].get<bool>());
    auto other = chat(); other["adapter_ids"] = Json::array({"one", "two"});
    ASSERT_EQ(dice::notice_windows::key(chat()), dice::notice_windows::key(other));
    other["is_group"] = true; ASSERT_NE(dice::notice_windows::key(chat()), dice::notice_windows::key(other));
}
TEST(NoticeWindows, PausingAndResumingWindowPreservesEachSubscriptionAndDoesNotHashSecretsIntoItsId) {
    auto a = rule(); a["id"] = "random-rule-id";
    a["channels"] = Json::array({{{"kind", "webhook"}, {"url", "https://example.com/hook?key=fake-secret"}}});
    auto window = dice::notice_windows::all({{"rules", Json::array({a})}})[0];
    ASSERT_TRUE(window["id"].get<std::string>().find("fake-secret") == std::string::npos);
    window["enabled"] = false;
    Json conf; dice::notice_windows::store(conf, dice::notice_windows::normalize(window), "paused");
    ASSERT_FALSE(all(conf)[0]["enabled"].get<bool>());
    auto restored = dice::notice_windows::all(conf)[0];
    ASSERT_TRUE(restored["subscriptions"][0]["enabled"].get<bool>());
    restored["enabled"] = true;
    Json next; dice::notice_windows::store(next, dice::notice_windows::normalize(restored), "resumed");
    ASSERT_TRUE(all(next)[0]["enabled"].get<bool>());
}
TEST(NoticeWebhook, ProviderPayloadsAndJsonVariablesAreSafeTypedAndSinglePass) {
    const Json vars{{"message", "引号\"\n[CQ:at,qq=all] {{title}}"}, {"title", "秘密标题"}, {"level", 8}, {"event", "error"}};
    for (const auto* provider : {"generic", "wecom", "dingtalk", "feishu"}) {
        const auto request = dice::notice_webhook::build({{"url", "https://example.com/hook"}, {"profile", provider}}, vars, 1700000000123);
        const auto payload = Json::parse(request.body);
        ASSERT_EQ(request.method, std::string("POST"));
        const auto text = std::string(provider) == "generic" ? payload["msg"] : std::string(provider) == "feishu" ? payload["content"]["text"] : payload["text"]["content"];
        ASSERT_EQ(text, vars["message"]); // Message text is not recursively interpreted.
        if (std::string(provider) == "generic") ASSERT_EQ(payload["level"], Json(8));
    }
    const auto custom = dice::notice_webhook::build({{"url", "https://example.com/hook"}, {"profile", "custom"}, {"method", "PATCH"},
        {"headers", {{"Authorization", "Bearer fixture-only"}}}, {"body_template", {{"notice", "prefix {{message}} suffix"}, {"severity", "{{level}}"}}}}, vars, 1);
    ASSERT_EQ(Json::parse(custom.body)["severity"], Json(8)); ASSERT_EQ(custom.method, std::string("PATCH"));
    ASSERT_EQ(custom.headers["Authorization"], Json("Bearer fixture-only"));
}
TEST(NoticeWebhook, SigningMatchesPublishedAlgorithmsAndDoesNotDuplicateSigningParameters) {
    Json ch{{"url", "https://oapi.dingtalk.com/robot/send?access_token=fixture&timestamp=old&sign=old"}, {"profile", "dingtalk"}, {"sign_secret", "SEC-fixture"}};
    const auto ding = dice::notice_webhook::build(ch, {{"message", "fixture"}}, 1700000000123);
    ASSERT_TRUE(ding.url.find("timestamp=old") == std::string::npos);
    ASSERT_TRUE(ding.url.find("sign=HypwBg7%2Bd%2F1yzbjKhHpE2kzhvyT36kAR1SC64mTcd7A%3D") != std::string::npos);
    ch["profile"] = "feishu"; ch["url"] = "https://open.feishu.cn/open-apis/bot/v2/hook/fixture";
    const auto feishu = Json::parse(dice::notice_webhook::build(ch, {{"message", "fixture"}}, 1700000000123).body);
    ASSERT_EQ(feishu["timestamp"], Json("1700000000")); ASSERT_EQ(feishu["sign"], Json("EtEghfEyD6PJDsQ43Sr7V7m7W2OgDUN/eUUDEDRvcWY="));
}
TEST(NoticeWebhook, InvalidUrlsHeadersMethodsAndTemplatesAreRejectedWithoutLeakingCredentialsThroughSearch) {
    auto rejects = [](Json c) { try { dice::notice_webhook::normalize(c); return false; } catch (...) { return true; } };
    const Json valid{{"url", "https://example.com/hook"}};
    for (const auto* url : {"file:///etc/passwd", "https://user:pass@example.com", "https://example.com/\nconfig=x", "https://{{message}}/hook"}) {
        auto c = valid; c["url"] = url; ASSERT_TRUE(rejects(c));
    }
    auto c = valid; c["headers"] = {{"Host", "internal"}}; ASSERT_TRUE(rejects(c));
    c = valid; c["headers"] = {{"X-Token", "abc\r\nX-Other:injected"}}; ASSERT_TRUE(rejects(c));
    c = valid; c["method"] = "GET"; ASSERT_TRUE(rejects(c));
    c = valid; c["body_template"] = {{"text", "{{unknown}}"}}; ASSERT_TRUE(rejects(c));
    c = valid; c["body_template"] = {{"text", "{{message"}}; ASSERT_TRUE(rejects(c));
    c = valid; c["headers"] = {{"X-Api-Key", "header-secret-fixture"}}; c["body_template"] = {{"password", "body-secret-fixture"}};
    auto r = rule(); r["channels"] = Json::array({c});
    ASSERT_FALSE(matchesSearch(r, "header-secret-fixture")); ASSERT_FALSE(matchesSearch(r, "body-secret-fixture"));
    const auto clipped = dice::notice_webhook::clipped(std::string(2100, 'a'), 2048);
    ASSERT_TRUE(clipped.size() <= size_t(2048)); ASSERT_TRUE(Json::parse(Json(clipped).dump()).is_string());
}
