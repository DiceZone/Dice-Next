#include "test_framework.h"
#include "core/command_router.h"
#include "core/plugin_command_priority.h"
#include "storage/group_account_settings.h"
#include "common/weighted_reply.h"
#include "core/reply/poke_reply.h"

#include <chrono>
#include <condition_variable>
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

    explicit LegacyFixture(identity_email::Sender sender = {}, cloud_cards::Transport cloud = {})
        : router{db, cfg, engine, i18n, resolver, cards, deck, adapters, std::move(sender), std::move(cloud)} {
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

class BindingAdapter final : public IAdapter {
public:
    std::mutex mu; std::condition_variable cv; bool replied = false;
    std::string id() const override { return "log-test"; }
    std::string name() const override { return id(); }
    std::string platform() const override { return "qq_official"; }
    std::string version() const override { return "test"; }
    bool configure(const json&) override { return true; }
    bool start() override { return true; }
    void stop() override {}
    bool isConnected() const override { return true; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message&) override {}
    void sendReply(const Message&, const std::string&) override {
        std::lock_guard lock(mu); replied = true; cv.notify_all();
    }
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return "9000"; }
    std::string getLoginName() const override { return "骰娘"; }
    std::string getGroupName(const std::string&) const override { return {}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
};
} // namespace

TEST(LegacyMod, GlobalManagementIsNotTheGroupPluginSwitch) {
    LegacyFixture f;
    std::vector<CommandRouter::ModEntry> mods = {
        {"demo", "Demo", "author", "1.0", "brief", true, false, true, 2, 3, 1}
    };
    std::vector<std::string> actions;
    f.router.setModProvider([&]() { return mods; },
        [&](const std::string& action, const std::string& name, std::string&) {
            actions.push_back(action + ":" + name);
            if (action == "off") mods[0].enabled = false;
            if (action == "on") mods[0].enabled = true;
            return true;
        });
    f.run(".mod off demo");  // Group admin is not a global mod administrator.
    ASSERT_TRUE(actions.empty());
    f.master();
    f.run(".mod off demo");
    ASSERT_EQ(actions.size(), static_cast<size_t>(1));
    ASSERT_EQ(actions.back(), std::string("off:demo"));
    ASSERT_FALSE(mods[0].enabled);
    f.run(".mod Demo on");
    ASSERT_EQ(actions.back(), std::string("on:demo"));
    ASSERT_TRUE(mods[0].enabled);
    const auto count = actions.size();
    f.run(".mod on demo");
    f.run(".mod off missing");
    f.run(".mod get demo");
    ASSERT_EQ(actions.size(), count);
    f.run(".mod reload demo");
    ASSERT_EQ(actions.back(), std::string("reload:demo"));
    f.run(".mod del demo");
    ASSERT_EQ(actions.back(), std::string("delete:demo"));
}

TEST(IdentityBind, EmailFlowBindsOnlyTheRequestingNativeIdentityOnAllSupportedPlatforms) {
    for (const auto* platform : {"qq_official", "discord", "kook"}) {
        std::string deliveredCode, recipient;
        auto adapter = std::make_shared<BindingAdapter>();
        LegacyFixture f{[&](const json&, const std::string& to, const std::string& body) {
            std::lock_guard lock(adapter->mu); recipient = to;
            std::istringstream input(body.substr(body.find(".bind confirm ")));
            std::string cmd, sub; input >> cmd >> sub >> deliveredCode; return true;
        }};
        f.adapters.registerAdapter(adapter);
        f.cfg.set<json>("identity_email", {{"enabled", true}, {"host", "smtp.example.com"}, {"port", 465}, {"ssl", true},
            {"user", "dice@example.com"}, {"pass", "test-only"}, {"from", "dice@example.com"}});
        f.msg.platform = platform; f.msg.type = MessageType::kPrivate;
        auto& store = identity::BindingStore::instance();
        const bool official = f.msg.platform == "qq_official";
        const auto setNative = [&](const std::string& native) {
            f.msg.senderId = official ? store.observeOfficial(f.db, "test", native, identity::Kind::User)
                : store.observeVirtual(f.db, platform, f.msg.adapterId, native, identity::Kind::User);
            f.msg.targetId = f.msg.senderId;
            f.msg.extra = {{"__identity_transport", platform}, {"__identity_native_sender", native},
                {"official_bot_id", "test"}, {"__identity_local_sender", "QQ-Official-test:" + native}};
        };
        setNative("requester");
        ASSERT_EQ(f.run(".bind qq 160703953"), "identity_email.queued");
        {
            std::unique_lock lock(adapter->mu);
            ASSERT_TRUE(adapter->cv.wait_for(lock, std::chrono::seconds(5), [&] { return adapter->replied; }));
            ASSERT_EQ(recipient, std::string("160703953@qq.com"));
        }
        setNative("other-user");
        ASSERT_EQ(f.run(".bind confirm " + deliveredCode), "identity_email.missing");
        setNative("requester");
        ASSERT_EQ(f.run(".bind confirm"), "identity_email.email_confirm_usage");
        ASSERT_EQ(f.run(".bind qq 160703954 " + deliveredCode), "identity_email.missing");
        ASSERT_EQ(f.run(".bind confirm " + deliveredCode), "identity_email.success");
        setNative("requester");
        ASSERT_EQ(f.msg.senderId, std::string("160703953"));
        ASSERT_EQ(f.run(".bind email 160703954"), "identity_email.conflict");
    }
}

TEST(PersonaPermission, QQOfficialOrdinaryMemberCannotSwitchButInviterCan) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    PersonaManager personas{f.db, f.i18n, f.cfg};
    f.router.setPersonaManager(&personas);
    const int pid = personas.createTemplate("官方群人格", "");
    ASSERT_TRUE(pid > 0);

    f.msg.platform = "qq_official";
    f.msg.adapterId = "official-test";
    f.msg.type = MessageType::kGroup;
    f.msg.targetId = "group-openid";
    f.msg.senderId = "normalized-user";
    f.msg.extra = {{"__identity_native_sender", "member-openid"}};

    ASSERT_EQ(f.run(".rpmode set 官方群人格"), f.i18n.tr(Locale::kZhHans, "persona.no_perm_admin"));
    ASSERT_FALSE(personas.hasGroupPersonaOverride(f.msg.targetId, f.msg.platform));

    setAccountGroupSetting(*f.db.getStorage(), f.msg.adapterId, f.msg.platform,
        f.msg.targetId, f.msg.targetId, "inviter", "member-openid");
    ASSERT_TRUE(f.run(".rpmode set 官方群人格").find("官方群人格") != std::string::npos);
    ASSERT_EQ(personas.getActivePersona(f.msg.targetId, f.msg.platform), pid);
}

TEST(PersonaPermission, PrivateSelectionIsPersonalAndNeverChangesGlobalPersona) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    PersonaManager personas{f.db, f.i18n, f.cfg};
    f.router.setPersonaManager(&personas);
    const int pid = personas.createTemplate("私聊人格", "");
    ASSERT_TRUE(pid > 0);

    f.msg.type = MessageType::kPrivate;
    f.msg.targetId = f.msg.senderId;
    f.msg.extra = json::object();

    ASSERT_TRUE(f.run(".rpmode set 私聊人格").find("私聊人格") != std::string::npos);
    ASSERT_EQ(f.cfg.get<int>("persona/global", 0), 0);
    ASSERT_TRUE(f.run(".rpmode").find("私聊人格") != std::string::npos);

    ASSERT_TRUE(f.run(".rpmode inherit").find("基础") != std::string::npos);
    ASSERT_EQ(f.cfg.get<int>("persona/global", 0), 0);
}

TEST(IdentityBind, BareCommandUsesCurrentHelpInEveryLocale) {
    LegacyFixture f;
    f.msg.extra["__identity_transport"] = "onebot_v11";
    ASSERT_TRUE(f.i18n.load());
    for (auto loc : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa}) {
        for (const auto* command : {".bind", ".bind help", ".bind qq", ".bind qqgroup", ".bind confirm"}) {
            f.msg.content = command;
            const auto reply = f.router.handleMessage(f.msg, loc);
            ASSERT_EQ(reply, f.i18n.tr(loc, "help.topic.bind"));
            ASSERT_TRUE(reply.find(".cloud auth") != std::string::npos);
            ASSERT_TRUE(reply.find(".bind qq <") != std::string::npos);
            ASSERT_TRUE(reply.find("安全绑定用法（在 OneBot") == std::string::npos);
        }
    }
}

TEST(IdentityBind, ExplicitEmailAndLegacyAliasesWorkWithAClientKeyWithoutOAuthIO) {
    for (bool legacy : {false, true}) {
        std::string code;
        std::atomic<int> cloudCalls{0};
        auto adapter = std::make_shared<BindingAdapter>();
        LegacyFixture f{[&](const json&, const std::string&, const std::string& body) {
            std::lock_guard lock(adapter->mu);
            std::istringstream input(body.substr(body.find(".bind confirm ")));
            std::string command, sub; input >> command >> sub >> code; return true;
        }, [&](const cloud_cards::Request&) { ++cloudCalls; return cloud_cards::Response{}; }};
        f.adapters.registerAdapter(adapter);
        f.cfg.set<json>("adapters", json::array({{{"id", f.msg.adapterId}, {"heart_api_key", "bdc_test_key"}}}));
        f.cfg.set<json>("identity_email", {{"enabled", true}, {"host", "smtp.example.com"}, {"port", 465}, {"ssl", true},
            {"user", "dice@example.com"}, {"pass", "test-only"}, {"from", "dice@example.com"}});
        f.msg.platform = "qq_official"; f.msg.type = MessageType::kPrivate;
        auto& bindings = identity::BindingStore::instance();
        f.msg.senderId = bindings.observeOfficial(f.db, "aliases", "native", identity::Kind::User);
        f.msg.extra = {{"__identity_transport", "qq_official"}, {"__identity_native_sender", "native"},
            {"official_bot_id", "aliases"}, {"__identity_local_sender", "QQ-Official-aliases:native"}};
        ASSERT_EQ(f.run(legacy ? ".bind email 160703953" : ".bind qq 160703953 email"), "identity_email.queued");
        {
            std::unique_lock lock(adapter->mu);
            ASSERT_TRUE(adapter->cv.wait_for(lock, std::chrono::seconds(5), [&] { return adapter->replied; }));
        }
        ASSERT_EQ(f.run(std::string(legacy ? ".bind email 160703953 " : ".bind qq 160703953 ") + code), "identity_email.success");
        ASSERT_EQ(cloudCalls.load(), 0);
    }
}

TEST(IdentityBind, MissingNativeIdentityDoesNotBind) {
    LegacyFixture f;
    f.msg.type = MessageType::kPrivate;
    f.msg.platform = "qq_official";
    f.msg.extra = {{"__identity_transport", "qq_official"},
        {"__identity_local_sender", "QQ-Official-test:missing"}};
    const auto before = f.db.getStorage()->get_all<IdentityEndpointRow>().size();
    const auto reply = f.run(".bind qq 160703953");
    ASSERT_EQ(reply, "identity_email.no_identity");
    ASSERT_EQ(f.db.getStorage()->get_all<IdentityEndpointRow>().size(), before);
}

TEST(IdentityBind, GroupCodesStayOutOfTranscriptsAndNeverBind) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.msg.extra["__identity_transport"] = "qq_official";
    ASSERT_EQ(f.run(".bind qq 160703953 12345678"), "identity_email.private_only");
    ASSERT_TRUE(f.router.isIdentityEmailCommand(f.msg));
    f.router.recordMessage(f.msg, "private only");
    ASSERT_TRUE(f.messages().empty());
    f.msg.content = ".bind email 160703953 12345678";
    ASSERT_TRUE(f.router.isIdentityEmailCommand(f.msg));
    ASSERT_EQ(f.run(".bind confirm 12345678"), "identity_email.private_only");
    ASSERT_TRUE(f.router.isIdentityEmailCommand(f.msg));
    f.router.recordMessage(f.msg, "private only");
    ASSERT_TRUE(f.messages().empty());
}

TEST(IdentityBind, ClientKeyPrefersOAuthWithoutSendingMail) {
    LegacyFixture f{{}, [](const cloud_cards::Request&) { return cloud_cards::Response{0, json::object()}; }};
    f.adapters.registerAdapter(std::make_shared<BindingAdapter>());
    f.cfg.set<json>("adapters", json::array({{{"id", f.msg.adapterId}, {"heart_api_key", "bdc_test_key"}}}));
    f.msg.type = MessageType::kPrivate; f.msg.platform = "qq_official";
    auto& bindings = identity::BindingStore::instance();
    f.msg.senderId = bindings.observeOfficial(f.db, "test", "native", identity::Kind::User);
    f.msg.extra = {{"__identity_transport", "qq_official"}, {"__identity_local_sender", "QQ-Official-test:native"},
        {"__identity_native_sender", "native"}, {"official_bot_id", "test"}};
    ASSERT_EQ(f.run(".bind qq 160703953"), "identity_email.oauth_queued");
    ASSERT_EQ(f.run(".bind confirm 12345678"), "identity_email.oauth_confirm_usage");
    ASSERT_EQ(f.run(".bind qq 160703953 email"), "identity_email.disabled");
    ASSERT_EQ(f.run(".bind email 160703953"), "identity_email.disabled");
}

TEST(IdentityBind, InfoDoesNotRecommendUnsafeDirectOfficialGroupBinding) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    f.msg.platform = "qq_official";
    f.msg.extra = {{"__identity_transport", "qq_official"}, {"official_bot_id", "test"},
        {"__identity_native_target", "group-openid"}, {"__identity_native_sender", "user-openid"}};
    auto groupInfo = f.run(".info qqgroup");
    ASSERT_TRUE(groupInfo.find(".bind qqgroup QQ-Official-test:group-openid") != std::string::npos);
    ASSERT_TRUE(groupInfo.find(".bind qqgroup <真实QQ号>") == std::string::npos);
    auto userInfo = f.run(".info qq");
    ASSERT_TRUE(userInfo.find(".bind confirm") != std::string::npos);
}

TEST(SampleTemplate, RollReplyUsesChosenTextAndActualResult) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    f.i18n.setOverride(Locale::kZhHans, "dice.roll.result", "{res} {sample:效果拔群|干得漂亮}");
    const auto reply = f.run(".r 1d1");
    ASSERT_TRUE(reply.find("1") != std::string::npos);
    ASSERT_TRUE(reply.find("效果拔群") != std::string::npos || reply.find("干得漂亮") != std::string::npos);
    ASSERT_TRUE(reply.find("sample:") == std::string::npos);
}

TEST(SampleTemplate, CustomReplyRetainsShorthandAndSelectedVariables) {
    LegacyFixture f;
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{user}|{user}}", "", MatchType::kKeyword), "1000");
    ASSERT_EQ(f.router.renderReply(f.msg, "{甲|甲}", "", MatchType::kKeyword), "甲");
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{roll:1d1}|{roll:1d1}}", "", MatchType::kKeyword), "1");
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{$1}}", "^(.+)$", MatchType::kRegex), "");
    f.msg.content = "{sample:不执行|不执行}";
    ASSERT_EQ(f.router.renderReply(f.msg, "{sample:{$1}}", "^(.+)$", MatchType::kRegex), f.msg.content);
}

TEST(LegacyReply, WeightedAnswersStripTheMarkerAfterDatabaseReload) {
    LegacyFixture f;
    ReplyManager replies{f.db, f.cfg};
    ReplyRule rule;
    rule.matchContent = "hello";
    rule.results = {"::5::你好"};
    ASSERT_TRUE(replies.addRule(rule) > 0);
    replies.loadRules();
    const auto matched = replies.matchMessage("hello");
    ASSERT_EQ(matched.size(), size_t(1));
    ASSERT_EQ(replies.pickResult(matched.front()), "你好");
}

TEST(LegacyReply, WeightTicketsAndInvalidMarkersMatchLegacyNumericSemantics) {
    const std::vector<std::string> answers = {"::2::甲", "乙", "::3::丙"};
    const std::vector<std::string> expected = {"甲", "甲", "乙", "丙", "丙", "丙"};
    for (uint64_t ticket = 0; ticket < expected.size(); ++ticket) {
        uint64_t totalSeen = 0;
        const auto result = weighted_reply::pick(answers, [&](uint64_t total) { totalSeen = total; return ticket; });
        ASSERT_EQ(totalSeen, uint64_t(6));
        ASSERT_EQ(result, expected[ticket]);
    }
    for (const auto* literal : {"::0::甲", "::-1::甲", "::abc::甲", "::2x::甲", "::1000000::甲", "::2甲"})
        ASSERT_EQ(weighted_reply::pick({literal}), literal);
    ASSERT_EQ(weighted_reply::pick({"::999999::甲"}), "甲");
    ASSERT_EQ(weighted_reply::pick({"前缀::2::甲"}), "甲");
    ASSERT_EQ(weighted_reply::pick({}), "");
}

TEST(LegacyReply, BareDeckReferencesWorkWithoutChangingVariablesOrUnknownText) {
    LegacyFixture f;
    std::ofstream(f.dir.path / "deck.json") << R"({"测试牌堆":["牌面"],"嵌套牌堆":["{测试牌堆}"],"user":["不应覆盖变量"]})";
    f.deck.loadDir(u8str(f.dir.path));
    ASSERT_EQ(f.deck.drawFromDeck("嵌套牌堆").value_or(""), "牌面");
    ASSERT_EQ(f.router.renderReply(f.msg, "{测试牌堆}/{%测试牌堆}/{嵌套牌堆}", "", MatchType::kKeyword, true), "牌面/牌面/牌面");
    ASSERT_EQ(f.router.renderReply(f.msg, "{测试牌堆}", "", MatchType::kKeyword), "{测试牌堆}");
    ASSERT_EQ(f.router.renderReply(f.msg, "{deck:测试牌堆}/{deck:%测试牌堆}/{deck:嵌套牌堆}", "", MatchType::kKeyword), "牌面/牌面/牌面");
    ASSERT_EQ(f.router.renderReply(f.msg, "{draw:测试牌堆}/{user}/{不存在的牌堆}", "", MatchType::kKeyword), "牌面/1000/{不存在的牌堆}");
    f.msg.content = "{测试牌堆}";
    ASSERT_EQ(f.router.renderReply(f.msg, "{$1}", "(.+)", MatchType::kRegex), "{测试牌堆}");
    ASSERT_EQ(f.router.renderReply(f.msg, "\\{测试牌堆}", "", MatchType::kKeyword), "\\{测试牌堆}");
}

TEST(LegacyReply, BareDeckReferencesShareDepletionOnlyWithinOneReply) {
    LegacyFixture f;
    std::ofstream(f.dir.path / "deck.json") << R"({"双牌":["甲","乙"],"单牌":["甲"]})";
    f.deck.loadDir(u8str(f.dir.path));
    for (int i = 0; i < 5; ++i) {
        const auto reply = f.router.renderReply(f.msg, "{双牌}{双牌}", "", MatchType::kKeyword, true);
        ASSERT_TRUE(reply == "甲乙" || reply == "乙甲");
        const auto explicitReply = f.router.renderReply(f.msg, "{deck:双牌}{deck:双牌}", "", MatchType::kKeyword);
        ASSERT_TRUE(explicitReply == "甲乙" || explicitReply == "乙甲");
    }
    ASSERT_EQ(f.router.renderReply(f.msg, "{%单牌}{%单牌}", "", MatchType::kKeyword, true), "甲甲");
    ASSERT_EQ(f.router.renderReply(f.msg, "{deck:%单牌}{deck:%单牌}", "", MatchType::kKeyword), "甲甲");
    ASSERT_EQ(f.router.renderReply(f.msg, "\\{deck:单牌}", "", MatchType::kKeyword), "\\{deck:单牌}");
}

TEST(LegacyReply, ImportedCompatibilityFlagPersistsAndNoticesCarryIt) {
    LegacyFixture f;
    ReplyManager replies{f.db, f.cfg};
    ReplyRule old;
    old.conditions = {{MatchType::kKeyword, "hello"}};
    old.results = {"::3::{旧牌堆}"}; old.legacyReferences = true;
    old.cooldownSec = 60; old.cooldownNotice = "{旧牌堆}";
    const auto id = replies.addRule(old);
    replies.loadRules();
    auto rules = replies.matchMessage("hello");
    ASSERT_EQ(rules.size(), size_t(1));
    ASSERT_TRUE(rules.front().legacyReferences);
    ASSERT_EQ(rules.front().resultWeights.front(), 3);
    ReplyCtx ctx{"onebot_v11", "123", "1000"};
    ASSERT_TRUE(replies.pickReply("hello", ctx).rule.has_value());
    const auto pick = replies.pickReply("hello", ctx);
    ASSERT_TRUE(pick.noticeLegacyReferences);
    ASSERT_EQ(pick.noticeRuleId, id);
}

TEST(LegacyReply, ExplicitHelpAndGlobalTextUseTheirOwnNamespaces) {
    LegacyFixture f;
    std::ofstream(f.dir.path / "deck.json") << R"({"同名":["牌面"]})";
    f.deck.loadDir(u8str(f.dir.path));
    f.i18n.setOverride(Locale::kZhHans, "help.topic.同名", "帮助内容");
    f.i18n.setOverride(Locale::kZhHans, "legacy.同名", "全局内容");
    f.i18n.setOverride(Locale::kZhHans, "dice.crit", "优秀");
    ASSERT_EQ(f.router.renderReply(f.msg, "{text:strRollCriticalSuccess}", "", MatchType::kKeyword), "优秀");
    ASSERT_EQ(f.router.renderReply(f.msg, "{deck:同名}/{help:同名}/{text:同名}", "", MatchType::kKeyword), "牌面/帮助内容/全局内容");
    ASSERT_EQ(f.router.renderReply(f.msg, "{help:缺失}/{text:缺失}/{deck:缺失}", "", MatchType::kKeyword), "{help:缺失}/{text:缺失}/{deck:缺失}");
    ASSERT_EQ(f.router.renderReply(f.msg, "\\{help:同名}/\\{text:同名}", "", MatchType::kKeyword), "\\{help:同名}/\\{text:同名}");
}

TEST(ReplyWeights, FormalWeightsPersistZeroAndDoNotParseLiteralMarkers) {
    LegacyFixture f;
    ReplyManager replies{f.db, f.cfg};
    auto rule = reply_definition::replyRuleFromJson({{"matchContent", "hello"},
        {"results", {"不抽取", "::5::原文"}}, {"resultWeights", {0, 3}}});
    ASSERT_EQ(reply_definition::replyRuleValidate(rule), "");
    const auto id = replies.addRule(rule);
    ASSERT_TRUE(id > 0);
    replies.loadRules();
    const auto loaded = replies.matchMessage("hello");
    ASSERT_EQ(loaded.size(), size_t(1));
    ASSERT_EQ(loaded.front().resultWeights[0], 0);
    ASSERT_EQ(loaded.front().resultWeights[1], 3);
    ASSERT_EQ(replies.pickResult(loaded.front()), "::5::原文");
    auto row = f.db.getStorage()->get<ReplyRuleRow>(id);
    ASSERT_TRUE(json::parse(row.results).front().is_object());
}

TEST(ReplyWeights, InvalidWeightsAndEmptyAnswersAreRejected) {
    auto rule = reply_definition::replyRuleFromJson({{"matchContent", "hello"}, {"results", {"甲", "乙"}}, {"resultWeights", {3, 1}}});
    ASSERT_EQ(reply_definition::replyRuleValidate(rule), "");
    rule.resultWeights = {0, 0};
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
    rule.resultWeights = {1};
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
    rule.resultWeights = {-1, 1};
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
    rule.resultWeights = {1000000, 1};
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
    rule.results = {"", "乙"}; rule.resultWeights = {1, 1};
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
    bool rejected = false;
    try { reply_definition::replyRuleFromJson({{"results", {"甲"}}, {"resultWeights", {1.5}}}); }
    catch (const std::exception&) { rejected = true; }
    ASSERT_TRUE(rejected);
    rule = reply_definition::replyRuleFromJson({{"matchContent", "hello"}, {"results", {""}}});
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
}

TEST(PokeReply, UsesAdvancedLimitsWithoutTextMatchingAndSeparatesAccounts) {
    LegacyFixture f;
    ReplyManager replies{f.db, f.cfg};
    auto rule = reply_definition::replyRuleFromJson({{"results", {"甲", "乙"}}, {"resultWeights", {0, 1}},
        {"cooldownSec", 30}, {"cooldownNotice", "冷却"}, {"dayLimit", 1}});
    ASSERT_EQ(reply_definition::replyRuleValidate(rule, true), "");
    ASSERT_FALSE(reply_definition::replyRuleValidate(rule).empty());
    const ReplyCtx ctx{f.msg.platform, f.msg.targetId, f.msg.senderId};
    auto first = replies.pickEventReply(rule, ctx, "poke|account-1|");
    ASSERT_TRUE(first.rule.has_value());
    ASSERT_EQ(replies.pickResult(*first.rule), "乙");
    ASSERT_EQ(replies.pickEventReply(rule, ctx, "poke|account-1|").notice, "冷却");
    ASSERT_TRUE(replies.pickEventReply(rule, ctx, "poke|account-2|").rule.has_value());
    ASSERT_TRUE(replies.matchMessage("戳一戳").empty());
    rule.enabled = false;
    ASSERT_FALSE(replies.pickEventReply(rule, ctx, "disabled|").rule.has_value());
    rule.enabled = true; rule.prob = 0;
    ASSERT_FALSE(replies.pickEventReply(rule, ctx, "prob-zero|").rule.has_value());
    rule.prob = 100; rule.scopeMode = "allow"; rule.scopeIds = "9999";
    ASSERT_FALSE(replies.pickEventReply(rule, ctx, "outside|").rule.has_value());
}

TEST(PokeReply, CloserLegacyConfigurationStillOverridesInheritedNewReply) {
    json all = {{"events", {{"poke_reply", {{"results", {"全局"}}, {"resultWeights", {1}}}}}}};
    all["dice"]["scoped_overrides"]["account"]["1"]["events"] = {{"poke", "旧账号"}, {"poke_command", ".jrrp"}};
    all["dice"]["scoped_overrides"]["account"]["2"]["events"] = {{"poke_enabled", false}};
    auto ev = poke_reply::resolveEvents(all, "onebot_v11", "1");
    ASSERT_FALSE(ev.contains("poke_reply"));
    auto definition = poke_reply::definition(ev, "默认");
    ASSERT_EQ(definition["results"][0].get<std::string>(), "旧账号");
    ASSERT_EQ(definition["command"].get<std::string>(), ".jrrp");
    ev = poke_reply::resolveEvents(all, "onebot_v11", "2");
    ASSERT_TRUE(ev.contains("poke_reply"));
    ASSERT_FALSE(poke_reply::definition(ev, "默认")["enabled"].get<bool>());
}

TEST(SampleTemplate, TextCommandSupportsNestedChoicesAndDice) {
    LegacyFixture f;
    ASSERT_EQ(f.run(".text {sample:{sample:{user}}}"), "1000");
    ASSERT_EQ(f.run(".text {sample:{1d1}|{1d1}}"), "1");
}

TEST(BotText, PersistsPerAccountAndConversationAndShowsUsageWithoutChangingState) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    ASSERT_TRUE(f.run(".bot text").find(".bot text plain") != std::string::npos);
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.run("。bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text invalid");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text rich 9999"); // Another bot's setting must stay untouched.
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    auto other = f.msg;
    other.adapterId = "other-bot";
    ASSERT_FALSE(f.router.conversationPlainText(other));
    other = f.msg; other.targetId = "other-group";
    ASSERT_FALSE(f.router.conversationPlainText(other));
    other = f.msg; other.type = MessageType::kPrivate;
    ASSERT_FALSE(f.router.conversationPlainText(other));
    f.msg.type = MessageType::kPrivate;
    f.run(".bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text rich");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.type = MessageType::kGroup;
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    CommandRouter reloaded(f.db, f.cfg, f.engine, f.i18n, f.resolver, f.cards, f.deck, f.adapters);
    ASSERT_TRUE(reloaded.conversationPlainText(f.msg));
    f.run(".bot off");
    f.run(".bot text rich");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    ASSERT_TRUE(f.router.isGroupDisabled(f.msg));
}

TEST(BotText, PermissionChecksIncludeOfficialGroupsWithoutTrustedRoles) {
    LegacyFixture f;
    f.msg.extra = {{"role", "member"}};
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.platform = "qq_official";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.master();
    f.msg.extra["__identity_native_sender"] = f.msg.senderId;
    f.run(".bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
}

TEST(BotText, CustomMarkdownTemplateDowngradesBeforeDiceValuesAreInserted) {
    LegacyFixture f;
    f.i18n.load();
    f.i18n.setOverride(Locale::kZhHans, "dice.roll.result", "**{nick}**：`{res}`", ContentFormat::kMarkdown);
    f.run(".bot text plain");
    I18n::beginOutboundCapture(f.router.conversationPlainText(f.msg)
        ? ContentFormat::kPlainText : ContentFormat::kMarkdown);
    const auto reply = f.run(".r 1d1");
    const auto format = I18n::endOutboundCapture();
    ASSERT_TRUE(reply.find("**") == std::string::npos);
    ASSERT_TRUE(reply.find('`') == std::string::npos);
    ASSERT_EQ(static_cast<int>(format), static_cast<int>(ContentFormat::kPlainText));
    f.run(".bot text rich");
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    ASSERT_TRUE(f.run(".r 1d1").find("**") != std::string::npos);
    ASSERT_EQ(static_cast<int>(I18n::endOutboundCapture()), static_cast<int>(ContentFormat::kMarkdown));
}

TEST(BotText, OfficialInviterUsesNativeIdentityAndOnlyControlsTheInvitedBotAndGroup) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.msg.extra = {{"__identity_native_sender", "inviter-openid"}};
    f.setting("inviter", "inviter-openid");
    f.run(".bot text plain");
    ASSERT_TRUE(f.router.conversationPlainText(f.msg));
    f.run(".bot text rich");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));

    f.msg.extra["__identity_native_sender"] = "another-openid";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.senderId = "inviter-openid"; // A public ID must not substitute for the native identity.
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.extra.erase("__identity_native_sender");
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));

    f.msg.extra["__identity_native_sender"] = "inviter-openid";
    f.msg.adapterId = "another-bot";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
    f.msg.adapterId = "log-test";
    f.msg.targetId = "another-group";
    f.run(".bot text plain");
    ASSERT_FALSE(f.router.conversationPlainText(f.msg));
}


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

TEST(QQCharacterStatus, UsesStoredValuesAndActualAttributeChanges) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.cfg.set<bool>("dice/auto_card", false);
    f.run(".pc new 调查员");
    f.run(".st hp14/14 san65/99 mp12/12");
    auto reply = f.run(".st hp-2");
    auto card = f.router.lastOfficialCard();
    ASSERT_TRUE(card != nullptr);
    ASSERT_EQ(card->original, reply);
    ASSERT_EQ(card->vitals[0].current, 12);
    ASSERT_EQ(card->vitals[0].maximum.value_or(-1), 14);
    ASSERT_EQ(card->changes[0].before.value_or(-1), 14);
    ASSERT_EQ(card->changes[0].after, 12);
    const auto before = f.cards.getAttrs(f.msg.senderId, f.msg.targetId);
    reply = f.run(".pc status");
    ASSERT_TRUE(reply.find("12/14") != std::string::npos);
    ASSERT_TRUE(f.cards.getAttrs(f.msg.senderId, f.msg.targetId) == before);
    ASSERT_TRUE(f.router.lastOfficialCard()->statusOnly);
    f.run(".pc nonsense");
    ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
    f.msg.platform = "onebot_v11";
    ASSERT_TRUE(f.run(".pc status").find("12/14") != std::string::npos);
    ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
}

TEST(QQCharacterStatus, ShortcutsFollowConfiguredPrefixAndNeverIncludeUserText) {
    LegacyFixture f;
    f.msg.platform = "qq_official";
    f.cfg.set<bool>("dice/auto_card", false);
    f.run(".st hp10/10");
    f.cfg.set<json>("dice/command_prefixes", json::array({"!"}));
    f.run("!pc status");
    auto card = f.router.lastOfficialCard();
    ASSERT_TRUE(card != nullptr);
    ASSERT_EQ(card->commands[0].text, "!pc status");
    ASSERT_EQ(card->commands[2].text, "!st ");
    f.msg.atList = {"other-user"};
    ASSERT_EQ(f.run("!pc status"), "qq_rich.status_usage");
    ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
}

TEST(QQCharacterStatus, CompactionPreservesCustomRepliesAndOtherPlatforms) {
    LegacyFixture f;
    ASSERT_TRUE(f.i18n.load());
    f.cfg.set<bool>("dice/auto_card", false);
    for (const auto* platform : {"qq_official", "onebot_v11", "milky", "discord", "kook"}) {
        f.msg.platform = platform;
        f.run(".st hp14/14");
        const auto reply = f.run(".st hp-2");
        ASSERT_EQ(reply, f.i18n.tr(Locale::kZhHans, "card.st.done", {{"nick", "<玩家>"}, {"detail", "生命值:12"}}));
        auto card = f.router.lastOfficialCard();
        if (f.msg.platform == "qq_official") {
            ASSERT_TRUE(card != nullptr);
            ASSERT_TRUE(card->body.has_value());
            ASSERT_TRUE(card->body->empty());
            ASSERT_EQ(card->original, reply);
        } else ASSERT_TRUE(card == nullptr);
    }
    f.msg.platform = "qq_official";
    f.i18n.setOverride(Locale::kZhHans, "card.st.done", "自定义回执：{detail}；请记入跑团笔记");
    const auto custom = f.run(".st hp+1");
    ASSERT_FALSE(f.router.lastOfficialCard()->body.has_value());
    ASSERT_TRUE(qq_rich::render(*f.router.lastOfficialCard(), true, false).find("请记入跑团笔记") != std::string::npos);
    ASSERT_EQ(f.router.lastOfficialCard()->original, custom);
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
