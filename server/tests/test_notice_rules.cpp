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
    std::string getGroupName(const std::string &) const override { return {}; }
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
