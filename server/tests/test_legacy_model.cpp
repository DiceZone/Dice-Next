#include "test_framework.h"
#include "common/model_formula.h"
#include "common/legacy_model.h"
#include "common/legacy_mod_limit.h"
#include "common/session_deck.h"
#include "core/mod/legacy_log_context.h"
#include "adapter/adapter_interface.h"
#include <chrono>
#include <fstream>
using namespace dice;

TEST(SessionDeck, LegacyPermutationRemainingCountsAndResetArePreserved) {
    nlohmann::json deck = {{"meta", {"already-drawn", "remaining"}}, {"idxs", {1, 0}}, {"size", 1}};
    std::mt19937_64 rng(7); std::string error;
    ASSERT_TRUE(session_deck::valid(deck, error));
    auto result = session_deck::draw(deck, rng);
    ASSERT_TRUE(result.ok); ASSERT_EQ(result.text, std::string("remaining")); ASSERT_EQ(deck["size"], 0);
    ASSERT_TRUE(session_deck::draw(deck, rng).empty);
    ASSERT_TRUE(session_deck::reset(deck, error)); ASSERT_EQ(deck["size"], 2);
    std::set<std::string> values;
    values.insert(session_deck::draw(deck, rng).text); values.insert(session_deck::draw(deck, rng).text);
    ASSERT_EQ(values.size(), size_t(2)); ASSERT_TRUE(values.count("already-drawn") != 0);
    ASSERT_TRUE(session_deck::draw(deck, rng).empty);
}

TEST(SessionDeck, WeightsAreCopiesAndEmptyOrInvalidPoolsFailClosed) {
    std::string error;
    auto deck = session_deck::create({"::[1d1]::a", "::2::b", "::0::not-drawn"},
        [](const auto& text) { return text == "[1d1]" ? std::string("1") : text; }, error);
    ASSERT_TRUE(deck.is_object()); ASSERT_EQ(deck["idxs"].size(), size_t(3));
    std::mt19937_64 rng(1); std::map<std::string, int> counts;
    for (int i = 0; i < 3; ++i) ++counts[session_deck::draw(deck, rng).text];
    ASSERT_EQ(counts["a"], 1); ASSERT_EQ(counts["b"], 2); ASSERT_EQ(counts["not-drawn"], 0);
    ASSERT_TRUE(session_deck::draw(deck, rng).empty);
    for (const auto* card : {"::100001::too-many", "::0::empty", "::-1::negative", "::1.5::fraction"}) {
        error.clear(); ASSERT_FALSE(session_deck::create({card}, {}, error).is_object()); ASSERT_TRUE(!error.empty());
    }
}

TEST(SessionDeck, CorruptStatesNeverProduceIndicesOrFallbackPools) {
    for (const auto* raw : {R"({"meta":["a"],"idxs":[-1],"size":1})", R"({"meta":["a"],"idxs":[1],"size":1})",
            R"({"meta":["a"],"idxs":[0],"size":2})", R"({"meta":["a"],"idxs":[0],"size":-1})",
            R"({"meta":["a"],"idxs":[0],"size":0.5})", R"({"meta":[{}],"idxs":[0],"size":1})"}) {
        auto deck = nlohmann::json::parse(raw), original = deck; std::string error; std::mt19937_64 rng(0);
        ASSERT_FALSE(session_deck::draw(deck, rng).ok); ASSERT_EQ(deck, original);
        ASSERT_FALSE(session_deck::reset(deck, error));
    }
    std::string error; nlohmann::json result;
    ASSERT_FALSE(session_deck::decode("not-json", result, error));
    error.clear(); const auto deck = session_deck::create({"a"}, {}, error);
    ASSERT_FALSE(session_deck::validCollection({{"A", deck}, {"a", deck}}, error));
    ASSERT_TRUE(session_deck::validCollection({{"中文", deck}}, error));
    ASSERT_EQ(session_deck::findName({{"Test", deck}}, "TEST"), std::string("Test"));
}

TEST(LegacyHooks, LogEndFieldsReferToTheRealExportAndKeepTheSource) {
    Message message;
    message.senderId = "user"; message.targetId = "group"; message.senderName = "Nick";
    message.platform = "p"; message.adapterId = "a"; message.selfId = "bot";
    message.id = "m"; message.content = ".log end";
    const auto path = std::filesystem::path("data/logs/legacy-log-12.txt");
    const auto event = legacy_log::context(message, 12, "../../display title only", path, 7, 123);
    ASSERT_EQ(event["log_file"], "legacy-log-12.txt"); ASSERT_EQ(event["log_name"], "../../display title only");
    ASSERT_EQ(event["log_count"], 7); ASSERT_EQ(event["log_id"], 12);
    ASSERT_EQ(event["fromUser"], "user"); ASSERT_EQ(event["fromQQ"], "user"); ASSERT_EQ(event["fromGroup"], "group");
    ASSERT_EQ(event["platform"], "p"); ASSERT_EQ(event["adapterId"], "a"); ASSERT_EQ(event["selfId"], "bot");
    ASSERT_EQ(event["msgid"], "m"); ASSERT_EQ(event["time"], 123);
    ASSERT_EQ(event["log_path"], legacy_log::utf8(std::filesystem::absolute(path).lexically_normal()));
    ASSERT_EQ(legacy_log::context(message, 12, "", path, 0, 123)["log_name"], "log12");
}

TEST(LegacyHooks, PokePoliciesAndLuaContextUseTheActorNotTheBotRecipient) {
    BotEvent event;
    event.type = EventType::kPoke; event.userId = "bot"; event.operatorId = "actor";
    ASSERT_EQ(eventTriggerUserId(event), std::string("actor"));
    event.operatorId.clear();
    ASSERT_EQ(eventTriggerUserId(event), std::string("bot"));
    event.operatorId = "moderator"; event.userId = "member";
    for (const auto type : {EventType::kGroupDecrease, EventType::kGroupIncrease,
            EventType::kFriendRequest, EventType::kGroupRequest}) {
        event.type = type;
        ASSERT_EQ(eventTriggerUserId(event), std::string("member"));
    }
}

TEST(LegacyModel, NumericFormulasKeepIntegerDslAndExplicitFloatingFunctions) {
    const std::map<std::string, int> values = {{"力量", 9}, {"体型", 11}, {"zero", 0}};
    auto lookup = [&](const std::string& name) -> std::optional<int> {
        const auto found = values.find(name); return found == values.end() ? std::nullopt : std::optional<int>(found->second);
    };
    auto eval = [&](const std::string& text) { return model_formula::evaluate(text, lookup).value_or(-999); };
    ASSERT_EQ(eval("力量/2*2"), 8);
    ASSERT_EQ(eval("Math.floor(this.力量/2-5)"), -1);
    ASSERT_EQ(eval("Math.ceil(-3/2)"), -1);
    ASSERT_EQ(eval("Math.round(-1.5)"), -1);
    ASSERT_EQ(eval("Math.max(2, Math.min(5, pc.力量))"), 5);
    ASSERT_EQ(eval("abs(-3)+pow(2,3)+sqrt(9)"), 14);
    ASSERT_EQ(eval("1==3>2"), 1);
    ASSERT_EQ(eval("(this.力量 || 0) + (missing || 0)"), 9);
    ASSERT_EQ(eval("99-missing?0"), 99);
    ASSERT_EQ(eval("zero && missing"), 0);
    ASSERT_EQ(eval("力量>=9 ? 力量+体型 : missing"), 20);
    ASSERT_EQ(eval("zero ? missing : (力量>5 ? 3 : 4)"), 3);
    for (const auto* text : {"1/0", "2147483647+1", "1+", "(2+3", "1 trailing"}) ASSERT_FALSE(model_formula::evaluate(text, lookup).has_value());
}

TEST(LegacyModel, NeverExecutesScriptsOrReadsUnusedBranches) {
    int lookups = 0;
    auto lookup = [&](const std::string&) -> std::optional<int> { ++lookups; return 7; };
    for (const auto* text : {"eval('1')", "(()=>1)()", "this['x']", "let x=1; x", "process.exit()", "while(true){}", "this.X=1", "require('fs')", "new Function('return 1')()"}) {
        ASSERT_FALSE(model_formula::supported(text)); ASSERT_FALSE(model_formula::evaluate(text, lookup).has_value());
    }
    ASSERT_EQ(lookups, 0);
    ASSERT_EQ(model_formula::evaluate("0 && hidden", lookup).value_or(-1), 0);
    ASSERT_EQ(model_formula::evaluate("1 || hidden", lookup).value_or(-1), 1);
    ASSERT_EQ(model_formula::evaluate("1 ? 9 : hidden", lookup).value_or(-1), 9);
    ASSERT_EQ(lookups, 0);
    ASSERT_FALSE(model_formula::supported(std::string(100, '(') + "1" + std::string(100, ')')));
}

TEST(LegacyModel, PropertiesHandleWhitespaceCommentsCdataEntitiesAndDiagnostics) {
    const auto doc = legacy_model::parse(R"XML(
<model name="Synthetic">
<!-- <property><any name='never' text='javascript'>1</any></property> -->
<script>never_execute_a_model_script()</script>
<property>
  <any name = '力量' alias = "STR / 力量别名" />
  <any name='派生' alias="DM" text = 'javascript'><![CDATA[Math.floor(this.力量/2)]]></any>
  <any name='条件' text='javascript'>this.力量 &gt; 9 ? 1 : 0</any>
  <any name='&#x4E2D;' alias='entity' text='dicexp'>&amp;力量</any>
  <any name='unsafe' text='javascript'>let x=1; return globalThis.secret</any>
</property></model>
)XML");
    ASSERT_TRUE(doc.valid); ASSERT_EQ(doc.properties.size(), size_t(5));
    ASSERT_EQ(doc.properties[0].alias, std::string("STR / 力量别名"));
    ASSERT_EQ(doc.properties[1].formula, std::string("Math.floor(this.力量/2)"));
    ASSERT_EQ(doc.properties[2].formula, std::string("this.力量 > 9 ? 1 : 0"));
    ASSERT_EQ(doc.properties[3].name, std::string("中")); ASSERT_EQ(doc.properties[3].formula, std::string("&力量"));
    ASSERT_TRUE(doc.properties[4].formula.empty()); ASSERT_EQ(doc.warnings.size(), size_t(2));
    ASSERT_FALSE(legacy_model::parse("<!DOCTYPE x [<!ENTITY x SYSTEM 'file:///never'>]><property/>").valid);
    ASSERT_FALSE(legacy_model::parse("<property><any name='x' text='javascript'>1").valid);
}

TEST(LegacyModel, FileOrderHonorsNestedResourcesAndDisabledDescriptors) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / ("dice-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{base};
    const auto root = base / "mod";
    for (const auto* folder : {"a/model/deep", "z/model", "off/model", "parent.disabled/child/model", "pack/child/model"}) fs::create_directories(root / folder);
    auto write = [](const fs::path& path, const std::string& text) { std::ofstream out(path, std::ios::binary); out << text; };
    for (const auto* file : {"a/model/deep/A.xml", "z/model/Z.xml", "off/model/O.xml", "parent.disabled/child/model/P.xml", "pack/child/model/C.xml"}) write(root / file, "<model/>");
    write(root / "off.json.disabled", "{}"); write(base / "mod_order.json", "[\"z\",\"a\",\"pack.child\"]");
    const auto files = legacy_model::orderedFiles(root);
    ASSERT_EQ(files.size(), size_t(3)); ASSERT_EQ(files[0].filename().string(), std::string("Z.xml"));
    ASSERT_EQ(files[1].filename().string(), std::string("A.xml")); ASSERT_EQ(files[2].filename().string(), std::string("C.xml"));
}

TEST(LegacyModLimit, InvalidConditionsNeverBecomeUnrestrictedRules) {
    for (const auto& raw : {R"({"user_var":{"x":{"unknown":true}}})", R"({"cd":{"user":-1}})", R"({"prob":101})", R"({"lock":{"unknown":"x"}})", R"({"unsupported":true})"}) {
        legacy_mod::Limits limits; std::string error;
        ASSERT_FALSE(legacy_mod::parseLimits(nlohmann::json::parse(raw), limits, error)); ASSERT_TRUE(!error.empty());
    }
    legacy_mod::Limits limits; std::string error;
    ASSERT_TRUE(legacy_mod::parseLimits("prob:50;grp_id:!0;cd:shared@global=10&@echo=wait;today:2;lock:shared@user;user_var:trust=2+&count=3-;dicemaid:off", limits, error));
    ASSERT_EQ(limits.probability, 50); ASSERT_FALSE(legacy_mod::idMatches(limits.groups, ""));
    ASSERT_EQ(limits.cooldowns.size(), size_t(1)); ASSERT_EQ(limits.cooldownNotice, std::string("wait"));
    ASSERT_TRUE(legacy_mod::matches(2, limits.userVariables["trust"])); ASSERT_FALSE(legacy_mod::matches(1, limits.userVariables["trust"]));
    ASSERT_TRUE(legacy_mod::matches(3, limits.userVariables["count"])); ASSERT_FALSE(legacy_mod::matches(4, limits.userVariables["count"]));
    ASSERT_FALSE(legacy_mod::parseLimits("cd:x@unsafe=10", limits, error));
}

TEST(LegacyModLimit, MixedLuaTablesKeepDefaultAndNamedCounterScopes) {
    legacy_mod::Limits limits; std::string error;
    const auto declaration = nlohmann::json::parse(R"({
        "cd":{"1":5,"echo":"wait","global":9,"user":{"1":2,"shared":3}},
        "lock":{"1":"first","2":"second","user":"personal"}
    })");
    ASSERT_TRUE(legacy_mod::parseLimits(declaration, limits, error));
    ASSERT_EQ(limits.cooldowns.size(), size_t(4));
    ASSERT_EQ(limits.cooldownNotice, std::string("wait"));
    auto has = [](const auto& counters, auto scope, const std::string& name, long long amount) {
        return std::any_of(counters.begin(), counters.end(), [&](const auto& counter) {
            return counter.scope == scope && counter.name == name && counter.amount == amount;
        });
    };
    using S = legacy_mod::LimitScope;
    ASSERT_TRUE(has(limits.cooldowns, S::Chat, "", 5));
    ASSERT_TRUE(has(limits.cooldowns, S::Global, "", 9));
    ASSERT_TRUE(has(limits.cooldowns, S::User, "", 2));
    ASSERT_TRUE(has(limits.cooldowns, S::User, "shared", 3));
    ASSERT_EQ(limits.locks.size(), size_t(3));
    ASSERT_TRUE(has(limits.locks, S::Global, "first", 1));
    ASSERT_TRUE(has(limits.locks, S::Global, "second", 1));
    ASSERT_TRUE(has(limits.locks, S::User, "personal", 1));
}
