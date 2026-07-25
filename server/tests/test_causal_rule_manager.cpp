// ─── C#29: CausalRuleManager Unit Tests ───────────────────────
// Tests rule matching, condition evaluation (AND/OR), scope checking,
// cooldown integration, counter actions, and reply selection.
// Uses in-memory SQLite DB + real CooldownManager + CounterStore.

#include "test_framework.h"
#include "../src/core/causal/causal_rule_manager.h"
#include "../src/core/causal/cooldown_manager.h"
#include "../src/core/causal/counter_store.h"
#include "../src/storage/database.h"
#include "../src/config/config_manager.h"

using namespace dice;

static std::unique_ptr<Database> makeDb() {
    auto db = std::make_unique<Database>();
    db->open(":memory:");
    return db;
}

static std::unique_ptr<ConfigManager> makeCfg() {
    auto cfg = std::make_unique<ConfigManager>("config/default_config.json");
    cfg->load();
    return cfg;
}

// ─── Helper: create a simple rule with one keyword condition + reply action
static CausalRule makeKeywordRule(const std::string& keyword, const std::string& reply) {
    CausalRule rule;
    rule.name = "test_rule";
    rule.scope = "global";
    rule.enabled = true;
    rule.priority = 100;
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = keyword;
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {reply};
    rule.actions.push_back(action);
    return rule;
}

// ─── Text Matching Tests ──────────────────────────────────────

TEST(CausalMatch, KeywordExactMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("hello", "Hi there!");
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("hello", "user1", "group1", "Nick", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "Hi there!");
}

TEST(CausalMatch, KeywordCaseInsensitive) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("Hello", "matched");
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("hello", "user1", "group1", "Nick", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "matched");
}

TEST(CausalMatch, KeywordNoMatchForDifferentText) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("hello", "matched");
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("world", "user1", "group1", "Nick", true);
    ASSERT_FALSE(result.matched);
}

TEST(CausalMatch, KeywordNoMatchForSubstring) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("hello", "matched");
    mgr.addRule(rule);

    // Keyword is exact match — "hello world" should NOT match
    auto result = mgr.matchAndExecute("hello world", "user1", "group1", "Nick", true);
    ASSERT_FALSE(result.matched);
}

TEST(CausalMatch, PrefixMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "prefix_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Prefix;
    cond.content = ".sign";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"signed!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute(".sign in", "user1", "group1", "Nick", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "signed!");
}

TEST(CausalMatch, PrefixNoMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "prefix_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Prefix;
    cond.content = ".sign";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"signed!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute(".r 1d100", "user1", "group1", "Nick", true);
    ASSERT_FALSE(result.matched);
}

TEST(CausalMatch, SearchMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "search_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Search;
    cond.content = "dice";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"found dice!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("roll the dice now", "user1", "group1", "Nick", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "found dice!");
}

TEST(CausalMatch, RegexMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "regex_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Regex;
    cond.content = "\\d+";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"number found!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("hello 123 world", "user1", "group1", "Nick", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "number found!");
}

TEST(CausalMatch, RegexInvalidPatternNoCrash) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "bad_regex";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Regex;
    cond.content = "[invalid";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"matched"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // Invalid regex should not crash — should return false
    auto result = mgr.matchAndExecute("test", "user1", "group1", "Nick", true);
    ASSERT_FALSE(result.matched);
}

// ─── AND/OR Logic Tests ───────────────────────────────────────

TEST(CausalLogic, ORLogicMatchesAny) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "or_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition c1;
    c1.type = CausalCondType::Keyword;
    c1.content = "hello";
    rule.conditions.push_back(c1);
    CausalCondition c2;
    c2.type = CausalCondType::Keyword;
    c2.content = "world";
    rule.conditions.push_back(c2);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"matched"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // First condition matches
    auto r1 = mgr.matchAndExecute("hello", "u1", "g1", "N", true);
    ASSERT_TRUE(r1.matched);

    // Second condition matches
    auto r2 = mgr.matchAndExecute("world", "u1", "g1", "N", true);
    ASSERT_TRUE(r2.matched);

    // Neither matches
    auto r3 = mgr.matchAndExecute("foo", "u1", "g1", "N", true);
    ASSERT_FALSE(r3.matched);
}

TEST(CausalLogic, ANDLogicMatchesAll) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "and_test";
    rule.scope = "global";
    rule.logic = "and";
    // Condition 1: prefix match
    CausalCondition c1;
    c1.type = CausalCondType::Prefix;
    c1.content = ".sign";
    rule.conditions.push_back(c1);
    // Condition 2: search match
    CausalCondition c2;
    c2.type = CausalCondType::Search;
    c2.content = "daily";
    rule.conditions.push_back(c2);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"signed!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // Both match
    auto r1 = mgr.matchAndExecute(".sign daily", "u1", "g1", "N", true);
    ASSERT_TRUE(r1.matched);

    // Only prefix matches
    auto r2 = mgr.matchAndExecute(".sign weekly", "u1", "g1", "N", true);
    ASSERT_FALSE(r2.matched);

    // Only search matches (no prefix)
    auto r3 = mgr.matchAndExecute("do daily sign", "u1", "g1", "N", true);
    ASSERT_FALSE(r3.matched);
}

TEST(CausalLogic, EmptyConditionsAlwaysMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "no_cond";
    rule.scope = "global";
    rule.logic = "or";
    // No conditions
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"always fires"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("anything", "u1", "g1", "N", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "always fires");
}

// ─── Scope Tests ──────────────────────────────────────────────

TEST(CausalScope, GlobalScopeMatchesAll) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("ping", "pong");
    rule.scope = "global";
    mgr.addRule(rule);

    // Should match for any user/group
    auto r1 = mgr.matchAndExecute("ping", "user1", "group1", "N", true);
    ASSERT_TRUE(r1.matched);

    auto r2 = mgr.matchAndExecute("ping", "user2", "group2", "N", true);
    ASSERT_TRUE(r2.matched);

    auto r3 = mgr.matchAndExecute("ping", "user1", "", "N", true);
    ASSERT_TRUE(r3.matched);
}

TEST(CausalScope, GroupScopeMatchesOnlySpecifiedGroups) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("ping", "pong");
    rule.scope = "group";
    rule.scopeIds = {"group1"};
    mgr.addRule(rule);

    // Should match for group1
    auto r1 = mgr.matchAndExecute("ping", "u1", "group1", "N", true);
    ASSERT_TRUE(r1.matched);

    // Should NOT match for group2
    auto r2 = mgr.matchAndExecute("ping", "u1", "group2", "N", true);
    ASSERT_FALSE(r2.matched);

    // Should NOT match for no group (private message)
    auto r3 = mgr.matchAndExecute("ping", "u1", "", "N", true);
    ASSERT_FALSE(r3.matched);
}

TEST(CausalScope, UserScopeMatchesOnlySpecifiedUsers) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("ping", "pong");
    rule.scope = "user";
    rule.scopeIds = {"user1"};
    mgr.addRule(rule);

    // Should match for user1
    auto r1 = mgr.matchAndExecute("ping", "user1", "group1", "N", true);
    ASSERT_TRUE(r1.matched);

    // Should NOT match for user2
    auto r2 = mgr.matchAndExecute("ping", "user2", "group1", "N", true);
    ASSERT_FALSE(r2.matched);
}

TEST(CausalScope, GroupScopeAnyGroupWhenNoIds) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("ping", "pong");
    rule.scope = "group";
    rule.scopeIds = {};  // empty = any group
    mgr.addRule(rule);

    auto r1 = mgr.matchAndExecute("ping", "u1", "anyGroup", "N", true);
    ASSERT_TRUE(r1.matched);

    // Should NOT match for no group
    auto r2 = mgr.matchAndExecute("ping", "u1", "", "N", true);
    ASSERT_FALSE(r2.matched);
}

// ─── Filter Tests ─────────────────────────────────────────────

TEST(CausalFilter, UserWhitelistAllowsListedUser) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "whitelist_test";
    rule.scope = "global";
    rule.logic = "and";
    CausalCondition c1;
    c1.type = CausalCondType::UserFilter;
    c1.content = "whitelist:user1,user2";
    rule.conditions.push_back(c1);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"allowed"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto r1 = mgr.matchAndExecute("test", "user1", "g1", "N", true);
    ASSERT_TRUE(r1.matched);

    auto r2 = mgr.matchAndExecute("test", "user3", "g1", "N", true);
    ASSERT_FALSE(r2.matched);
}

TEST(CausalFilter, UserBlacklistBlocksListedUser) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "blacklist_test";
    rule.scope = "global";
    rule.logic = "and";
    CausalCondition c1;
    c1.type = CausalCondType::UserFilter;
    c1.content = "blacklist:user1";
    rule.conditions.push_back(c1);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"allowed"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto r1 = mgr.matchAndExecute("test", "user1", "g1", "N", true);
    ASSERT_FALSE(r1.matched);

    auto r2 = mgr.matchAndExecute("test", "user2", "g1", "N", true);
    ASSERT_TRUE(r2.matched);
}

TEST(CausalFilter, GroupWhitelist) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "group_whitelist";
    rule.scope = "global";
    rule.logic = "and";
    CausalCondition c1;
    c1.type = CausalCondType::GroupFilter;
    c1.content = "whitelist:groupA,groupB";
    rule.conditions.push_back(c1);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"allowed"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto r1 = mgr.matchAndExecute("test", "u1", "groupA", "N", true);
    ASSERT_TRUE(r1.matched);

    auto r2 = mgr.matchAndExecute("test", "u1", "groupC", "N", true);
    ASSERT_FALSE(r2.matched);
}

// ─── Counter Action Tests ─────────────────────────────────────

TEST(CausalCounter, CounterAddAction) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "counter_add_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "sign";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::CounterAdd;
    action.counterName = "days";
    action.counterScope = "per-user";
    action.counterDelta = 1;
    rule.actions.push_back(action);
    CausalAction replyAction;
    replyAction.type = CausalActionType::Reply;
    replyAction.replies = {"signed! Days: {counter:days}"};
    rule.actions.push_back(replyAction);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("sign", "user1", "g1", "Nick", false);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "signed! Days: 1");
    ASSERT_EQ((int)result.counterChanges.size(), 1);
    ASSERT_EQ(result.counterChanges[0].name, "days");
    ASSERT_EQ(result.counterChanges[0].oldValue, 0);
    ASSERT_EQ(result.counterChanges[0].newValue, 1);

    // Second sign should add to the counter
    auto result2 = mgr.matchAndExecute("sign", "user1", "g1", "Nick", false);
    ASSERT_TRUE(result2.matched);
    ASSERT_EQ(result2.reply, "signed! Days: 2");
}

TEST(CausalCounter, CounterSetAction) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    // Pre-set a counter value
    ctrStore.set("1:count:user:user1", 50);

    CausalRule rule;
    rule.id = 1;
    rule.name = "counter_set_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "reset";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::CounterSet;
    action.counterName = "count";
    action.counterScope = "per-user";
    action.counterDelta = 0;
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("reset", "user1", "g1", "N", false);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ((int)result.counterChanges.size(), 1);
    ASSERT_EQ(result.counterChanges[0].oldValue, 50);
    ASSERT_EQ(result.counterChanges[0].newValue, 0);
}

TEST(CausalCounter, CounterResetAction) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    // Pre-set a counter value
    ctrStore.set("1:count:user:user1", 42);

    CausalRule rule;
    rule.id = 1;
    rule.name = "counter_reset_test";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "reset";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::CounterReset;
    action.counterName = "count";
    action.counterScope = "per-user";
    rule.actions.push_back(action);
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("reset", "user1", "g1", "N", false);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ((int)result.counterChanges.size(), 1);
    ASSERT_EQ(result.counterChanges[0].oldValue, 42);
    ASSERT_EQ(result.counterChanges[0].newValue, 0);

    // Verify the counter is now 0
    ASSERT_EQ(ctrStore.get("1:count:user:user1"), 0);
}

TEST(CausalCounter, CounterCheckCondition) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    // Pre-set a counter value
    ctrStore.set("1:days:user:user1", 5);

    CausalRule rule;
    rule.id = 1;
    rule.name = "counter_check_test";
    rule.scope = "global";
    rule.logic = "and";
    // Condition 1: keyword match
    CausalCondition c1;
    c1.type = CausalCondType::Keyword;
    c1.content = "check";
    rule.conditions.push_back(c1);
    // Condition 2: counter >= 3
    CausalCondition c2;
    c2.type = CausalCondType::CounterCheck;
    c2.counterName = "days";
    c2.op = ">=";
    c2.value = 3;
    rule.conditions.push_back(c2);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"veteran!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // user1 has 5 days (>= 3) → should match
    auto r1 = mgr.matchAndExecute("check", "user1", "g1", "N", true);
    ASSERT_TRUE(r1.matched);

    // user2 has 0 days (< 3) → should NOT match
    auto r2 = mgr.matchAndExecute("check", "user2", "g1", "N", true);
    ASSERT_FALSE(r2.matched);
}

TEST(CausalCounter, CounterCheckAllOperators) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    ctrStore.set("1:val:user:user1", 10);

    // Test each operator with value=10
    struct OpTest { std::string op; int value; bool expected; };
    std::vector<OpTest> tests = {
        {">=", 10, true},  {">=", 11, false},
        {"<=", 10, true},  {"<=", 9, false},
        {"==", 10, true},  {"==", 11, false},
        {"!=", 10, false}, {"!=", 11, true},
        {">",  9, true},   {">",  10, false},
        {"<",  11, true},  {"<",  10, false},
    };

    for (auto& t : tests) {
        auto db2 = makeDb();
        auto cfg2 = makeCfg();
        CooldownManager cd2;
        CounterStore cs2(*db2);
        CausalRuleManager m2(*db2, *cfg2, cd2, cs2);
        cs2.set("1:val:user:user1", 10);

        CausalRule rule;
        rule.id = 1;
        rule.name = "op_test";
        rule.scope = "global";
        rule.logic = "and";
        CausalCondition c;
        c.type = CausalCondType::CounterCheck;
        c.counterName = "val";
        c.op = t.op;
        c.value = t.value;
        rule.conditions.push_back(c);
        CausalAction action;
        action.type = CausalActionType::Reply;
        action.replies = {"hit"};
        rule.actions.push_back(action);
        m2.addRule(rule);

        auto result = m2.matchAndExecute("anything", "user1", "g1", "N", true);
        EXPECT_EQ(result.matched, t.expected);
    }
}

// ─── Cooldown Integration Tests ───────────────────────────────

TEST(CausalCooldown, RuleCoolsDownAfterTrigger) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "cooldown_test";
    rule.scope = "global";
    rule.cooldownMs = 10000;  // 10 seconds
    rule.cooldownKey = "per-user";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "spam";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"hit!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // First trigger: should match
    auto r1 = mgr.matchAndExecute("spam", "user1", "g1", "N", false);
    ASSERT_TRUE(r1.matched);

    // Second trigger: should be on cooldown (same user)
    auto r2 = mgr.matchAndExecute("spam", "user1", "g1", "N", false);
    ASSERT_FALSE(r2.matched);

    // Different user: should NOT be on cooldown
    auto r3 = mgr.matchAndExecute("spam", "user2", "g1", "N", false);
    ASSERT_TRUE(r3.matched);
}

TEST(CausalCooldown, DryRunDoesNotTriggerCooldown) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "dryrun_test";
    rule.scope = "global";
    rule.cooldownMs = 10000;
    rule.cooldownKey = "per-user";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "test";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"hit!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // dryRun=true should NOT trigger cooldown
    auto r1 = mgr.matchAndExecute("test", "user1", "g1", "N", true);
    ASSERT_TRUE(r1.matched);

    // Second dry run should still match
    auto r2 = mgr.matchAndExecute("test", "user1", "g1", "N", true);
    ASSERT_TRUE(r2.matched);
}

TEST(CausalCooldown, ZeroCooldownNeverBlocks) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "no_cooldown";
    rule.scope = "global";
    rule.cooldownMs = 0;  // no cooldown
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "ping";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"pong!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // Should match repeatedly
    for (int i = 0; i < 5; i++) {
        auto r = mgr.matchAndExecute("ping", "user1", "g1", "N", false);
        ASSERT_TRUE(r.matched);
    }
}

TEST(CausalCooldown, PerGroupCooldownIsolation) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "group_cd";
    rule.scope = "global";
    rule.cooldownMs = 10000;
    rule.cooldownKey = "per-group";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "ping";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"pong!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // group1: should match
    auto r1 = mgr.matchAndExecute("ping", "u1", "g1", "N", false);
    ASSERT_TRUE(r1.matched);

    // group1: should be on cooldown
    auto r2 = mgr.matchAndExecute("ping", "u1", "g1", "N", false);
    ASSERT_FALSE(r2.matched);

    // group2: different group, should match
    auto r3 = mgr.matchAndExecute("ping", "u1", "g2", "N", false);
    ASSERT_TRUE(r3.matched);
}

TEST(CausalCooldown, GlobalCooldownAppliesToAll) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "global_cd";
    rule.scope = "global";
    rule.cooldownMs = 10000;
    rule.cooldownKey = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "ping";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::Reply;
    action.replies = {"pong!"};
    rule.actions.push_back(action);
    mgr.addRule(rule);

    // First trigger: any user/group
    auto r1 = mgr.matchAndExecute("ping", "u1", "g1", "N", false);
    ASSERT_TRUE(r1.matched);

    // Second trigger: different user+group — still on cooldown (global)
    auto r2 = mgr.matchAndExecute("ping", "u2", "g2", "N", false);
    ASSERT_FALSE(r2.matched);
}

// ─── Priority Tests ───────────────────────────────────────────

TEST(CausalPriority, HigherPriorityMatchesFirst) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    // Low priority rule
    auto rule1 = makeKeywordRule("test", "low priority reply");
    rule1.name = "low_priority";
    rule1.priority = 10;
    mgr.addRule(rule1);

    // High priority rule
    auto rule2 = makeKeywordRule("test", "high priority reply");
    rule2.name = "high_priority";
    rule2.priority = 200;
    mgr.addRule(rule2);

    // Both match "test" but high priority should win
    auto result = mgr.matchAndExecute("test", "u1", "g1", "N", true);
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, "high priority reply");
}

// ─── Disabled Rule Tests ──────────────────────────────────────

TEST(CausalDisabled, DisabledRuleDoesNotMatch) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("ping", "pong");
    rule.enabled = false;
    mgr.addRule(rule);

    auto result = mgr.matchAndExecute("ping", "u1", "g1", "N", true);
    ASSERT_FALSE(result.matched);
}

// ─── JSON Serialization Tests ─────────────────────────────────

TEST(CausalJson, RuleToJSONAndFromJSON) {
    CausalRule rule;
    rule.id = 42;
    rule.name = "test_rule";
    rule.scope = "group";
    rule.scopeIds = {"g1", "g2"};
    rule.enabled = true;
    rule.priority = 50;
    rule.cooldownMs = 5000;
    rule.cooldownKey = "per-user";
    rule.logic = "and";

    CausalCondition c;
    c.type = CausalCondType::Keyword;
    c.content = "hello";
    rule.conditions.push_back(c);

    CausalCondition c2;
    c2.type = CausalCondType::CounterCheck;
    c2.counterName = "count";
    c2.op = ">=";
    c2.value = 5;
    rule.conditions.push_back(c2);

    CausalAction a;
    a.type = CausalActionType::Reply;
    a.replies = {"reply1", "reply2"};
    rule.actions.push_back(a);

    CausalAction a2;
    a2.type = CausalActionType::CounterAdd;
    a2.counterName = "count";
    a2.counterScope = "per-user";
    a2.counterDelta = 1;
    rule.actions.push_back(a2);

    json j = rule.toJSON();
    ASSERT_EQ(j["name"], "test_rule");
    ASSERT_EQ(j["scope"], "group");
    ASSERT_EQ(j["priority"], 50);
    ASSERT_EQ(j["conditions"].size(), 2);
    ASSERT_EQ(j["actions"].size(), 2);

    CausalRule restored = CausalRule::fromJSON(j);
    ASSERT_EQ(restored.name, "test_rule");
    ASSERT_EQ(restored.scope, "group");
    ASSERT_EQ((int)restored.scopeIds.size(), 2);
    ASSERT_EQ(restored.priority, 50);
    ASSERT_EQ((int)restored.conditions.size(), 2);
    ASSERT_EQ((int)restored.conditions[0].type, (int)CausalCondType::Keyword);
    ASSERT_EQ(restored.conditions[0].content, "hello");
    ASSERT_EQ((int)restored.conditions[1].type, (int)CausalCondType::CounterCheck);
    ASSERT_EQ(restored.conditions[1].op, ">=");
    ASSERT_EQ(restored.conditions[1].value, 5);
    ASSERT_EQ((int)restored.actions.size(), 2);
    ASSERT_EQ((int)restored.actions[0].type, (int)CausalActionType::Reply);
    ASSERT_EQ((int)restored.actions[0].replies.size(), 2);
    ASSERT_EQ((int)restored.actions[1].type, (int)CausalActionType::CounterAdd);
    ASSERT_EQ(restored.actions[1].counterDelta, 1);
}

// ─── CRUD Tests ───────────────────────────────────────────────

TEST(CausalCRUD, AddAndGetRule) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("test", "reply");
    int id = mgr.addRule(rule);
    ASSERT_TRUE(id > 0);

    const CausalRule* retrieved = mgr.getRuleById(id);
    ASSERT_TRUE(retrieved != nullptr);
    ASSERT_EQ(retrieved->name, "test_rule");
}

TEST(CausalCRUD, DeleteRule) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("test", "reply");
    int id = mgr.addRule(rule);
    ASSERT_TRUE(id > 0);

    bool deleted = mgr.deleteRule(id);
    ASSERT_TRUE(deleted);

    ASSERT_TRUE(mgr.getRuleById(id) == nullptr);
}

TEST(CausalCRUD, ToggleRule) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("test", "reply");
    rule.enabled = true;
    int id = mgr.addRule(rule);

    mgr.toggleRule(id);
    const CausalRule* r = mgr.getRuleById(id);
    ASSERT_TRUE(r != nullptr);
    ASSERT_FALSE(r->enabled);

    mgr.toggleRule(id);
    r = mgr.getRuleById(id);
    ASSERT_TRUE(r != nullptr);
    ASSERT_TRUE(r->enabled);
}

TEST(CausalCRUD, UpdateRule) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    auto rule = makeKeywordRule("old", "old reply");
    int id = mgr.addRule(rule);

    auto updated = makeKeywordRule("new", "new reply");
    updated.name = "updated_rule";
    bool success = mgr.updateRule(id, updated);
    ASSERT_TRUE(success);

    const CausalRule* r = mgr.getRuleById(id);
    ASSERT_TRUE(r != nullptr);
    ASSERT_EQ(r->name, "updated_rule");
    ASSERT_EQ(r->conditions[0].content, "new");
}

TEST(CausalCRUD, DeleteRuleRemovesCounters) {
    auto db = makeDb();
    auto cfg = makeCfg();
    CooldownManager cdMgr;
    CounterStore ctrStore(*db);
    CausalRuleManager mgr(*db, *cfg, cdMgr, ctrStore);

    CausalRule rule;
    rule.name = "counter_rule";
    rule.scope = "global";
    rule.logic = "or";
    CausalCondition cond;
    cond.type = CausalCondType::Keyword;
    cond.content = "inc";
    rule.conditions.push_back(cond);
    CausalAction action;
    action.type = CausalActionType::CounterAdd;
    action.counterName = "val";
    action.counterScope = "per-user";
    action.counterDelta = 1;
    rule.actions.push_back(action);
    int id = mgr.addRule(rule);

    // Trigger to create a counter
    mgr.matchAndExecute("inc", "user1", "g1", "N", false);

    // Verify counter exists
    auto counters = ctrStore.listByRule(id);
    ASSERT_EQ((int)counters.size(), 1);

    // Delete the rule
    mgr.deleteRule(id);

    // Counters should be removed
    auto countersAfter = ctrStore.listByRule(id);
    ASSERT_EQ((int)countersAfter.size(), 0);
}
