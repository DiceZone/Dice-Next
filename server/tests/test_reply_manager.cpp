#include "test_framework.h"
#include "core/reply/reply_definition.h"
#include "core/reply/reply_manager.h"

#include <chrono>
#include <filesystem>
#include <sqlite3.h>

using namespace dice;

namespace {

struct ReplyTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-reply-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ReplyTestDir() {
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("cannot create reply test directory");
    }
    ~ReplyTestDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct ReplyFixture {
    ReplyTestDir dir;
    Database db;
    ConfigManager config{(dir.path / "config").string()};

    ReplyFixture() {
        if (!db.open((dir.path / "test.db").string()))
            throw std::runtime_error("cannot open reply test database");
        config.resetDefault();
    }
};

ReplyRule sampleRule() {
    return reply_definition::replyRuleFromJson({
        {"conditions", {{{"type", "keyword"}, {"content", "hello"}}}},
        {"logic", "or"},
        {"results", {"你好", "欢迎"}},
        {"resultWeights", {3, 1}},
        {"priority", 120},
        {"prob", 80},
        {"cooldownSec", 15},
        {"scopeMode", "allow"},
        {"scopeIds", "100,200"},
        {"cooldownNotice", "冷却中"},
        {"dayLimit", 3},
        {"dayLimitNotice", "今天够啦"},
        {"scopeUsersMode", "deny"},
        {"scopeUsers", "300"},
    });
}

} // namespace

TEST(ReplyChannelScope, PersistsSeparatelyAndUsesSpecificRulesAtEqualPriority) {
    ReplyFixture fixture;
    ReplyManager replies{fixture.db, fixture.config};
    auto global = reply_definition::replyRuleFromJson({{"matchContent", "hello"}, {"replyContent", "hi"}});
    const int globalId = replies.addRule(global);
    auto platform = global;
    platform.channelScope = "adapter"; platform.channelTarget = "onebot_v11";
    const int platformId = replies.addRule(platform);
    auto account = global;
    account.channelScope = "account"; account.channelTarget = "bot:A:1";
    const int accountId = replies.addRule(account);
    ASSERT_TRUE(globalId > 0 && platformId > 0 && accountId > 0);
    ASSERT_EQ(replies.listRules()->size(), size_t(3)); // No cross-scope deduplication.
    replies.loadRules(); // Exercise DB round-trip and startup deduplication.
    ASSERT_EQ(replies.listRules()->size(), size_t(3));
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "100", "200", "bot:A:1"}, false).rule->id, accountId);
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "100", "200", "bot:B"}, false).rule->id, platformId);
    ASSERT_EQ(replies.pickReply("hello", {"qq_official", "100", "200", "bot:C"}, false).rule->id, globalId);
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "100", "200"}, false).rule->id, platformId);
    auto candidates = replies.matchMessage("hello", {"qq_official", "100", "200", "bot:C"});
    ASSERT_EQ(candidates.size(), size_t(1)); ASSERT_EQ(candidates.front().id, globalId);
    const auto row = fixture.db.getStorage()->get<ReplyRuleRow>(accountId);
    ASSERT_EQ(row.channelScope, std::string("account")); ASSERT_EQ(row.channelTarget, std::string("bot:A:1"));

    account.scopeMode = "allow"; account.scopeIds = "999";
    ASSERT_TRUE(replies.updateRule(accountId, account));
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "100", "200", "bot:A:1"}, false).rule->id, platformId);
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "999", "200", "bot:A:1"}, false).rule->id, accountId);
    account.scopeUsersMode = "deny"; account.scopeUsers = "200";
    ASSERT_TRUE(replies.updateRule(accountId, account));
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "999", "200", "bot:A:1"}, false).rule->id, platformId);

    global.priority = 101;
    ASSERT_TRUE(replies.updateRule(globalId, global));
    ASSERT_EQ(replies.pickReply("hello", {"onebot_v11", "999", "201", "bot:A:1"}, false).rule->id, globalId);
}

TEST(ReplyChannelScope, RejectsInvalidScopesInsteadOfWideningAndKeepsAccountLimitsIndependent) {
    ReplyFixture fixture;
    ReplyManager replies{fixture.db, fixture.config};
    auto rule = reply_definition::replyRuleFromJson({{"matchContent", "hello"}, {"replyContent", "hi"}});
    for (const auto& scope : {"unknown", "account", "adapter"}) {
        rule.channelScope = scope;
        ASSERT_TRUE(!reply_definition::replyRuleValidate(rule).empty());
        ASSERT_EQ(replies.addRule(rule), -1);
    }
    rule.channelScope = "global"; rule.channelTarget = "unexpected";
    ASSERT_EQ(replies.addRule(rule), -1);
    rule.channelTarget.clear(); rule.cooldownSec = 60; rule.dayLimit = 1;
    ASSERT_TRUE(replies.addRule(rule) > 0);
    const ReplyCtx a{"onebot_v11", "same-group", "user", "bot:A"};
    const ReplyCtx b{"onebot_v11", "same-group", "user", "bot:B"};
    ASSERT_TRUE(replies.pickReply("hello", a).rule.has_value());
    ASSERT_FALSE(replies.pickReply("hello", a).rule.has_value());
    ASSERT_TRUE(replies.pickReply("hello", b).rule.has_value());
}

TEST(ReplyChannelScope, UpgradesExistingDatabaseWithoutLosingGlobalRules) {
    ReplyTestDir dir;
    const auto path = (dir.path / "legacy.db").string();
    sqlite3* old = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &old), SQLITE_OK);
    const char* schema = R"sql(
        CREATE TABLE reply_rules (id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
          match_type INTEGER NOT NULL, match_content TEXT NOT NULL, reply_content TEXT NOT NULL,
          enabled INTEGER NOT NULL, priority INTEGER NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
          conditions TEXT NOT NULL, logic TEXT NOT NULL, results TEXT NOT NULL,
          prob INTEGER NOT NULL DEFAULT 100, cooldown_sec INTEGER NOT NULL DEFAULT 0,
          scope_mode TEXT NOT NULL DEFAULT '', scope_ids TEXT NOT NULL DEFAULT '', cooldown_notice TEXT NOT NULL DEFAULT '',
          day_limit INTEGER NOT NULL DEFAULT 0, day_limit_notice TEXT NOT NULL DEFAULT '',
          scope_users_mode TEXT NOT NULL DEFAULT '', scope_users TEXT NOT NULL DEFAULT '');
        INSERT INTO reply_rules (id,match_type,match_content,reply_content,enabled,priority,created_at,updated_at,conditions,logic,results)
          VALUES (42,0,'hello','legacy',1,123,'old','old','','or','');
    )sql";
    const int result = sqlite3_exec(old, schema, nullptr, nullptr, nullptr);
    sqlite3_close(old);
    ASSERT_EQ(result, SQLITE_OK);
    Database db;
    ASSERT_TRUE(db.open(path));
    auto rows = db.getStorage()->get_all<ReplyRuleRow>();
    ASSERT_EQ(rows.size(), size_t(1)); ASSERT_EQ(rows.front().id, 42);
    ASSERT_EQ(rows.front().replyContent, std::string("legacy")); ASSERT_EQ(rows.front().priority, 123);
    ASSERT_EQ(rows.front().channelScope, std::string("global")); ASSERT_TRUE(rows.front().channelTarget.empty());
}

TEST(ReplyDeduplication, ReusesExactRuleButKeepsBehavioralVariants) {
    ReplyFixture fixture;
    ReplyManager replies{fixture.db, fixture.config};
    const auto rule = sampleRule();

    bool deduplicated = true;
    const int originalId = replies.addRule(rule, &deduplicated);
    ASSERT_TRUE(originalId > 0);
    ASSERT_FALSE(deduplicated);

    const int duplicateId = replies.addRule(rule, &deduplicated);
    ASSERT_EQ(duplicateId, originalId);
    ASSERT_TRUE(deduplicated);
    ASSERT_EQ(static_cast<int>(fixture.db.getStorage()->count<ReplyRuleRow>()), 1);

    auto variant = rule;
    variant.priority++;
    const int variantId = replies.addRule(variant, &deduplicated);
    ASSERT_TRUE(variantId > 0);
    ASSERT_TRUE(variantId != originalId);
    ASSERT_FALSE(deduplicated);
    ASSERT_EQ(static_cast<int>(fixture.db.getStorage()->count<ReplyRuleRow>()), 2);

    ASSERT_TRUE(replies.updateRule(variantId, rule, &deduplicated));
    ASSERT_TRUE(deduplicated);
    ASSERT_EQ(static_cast<int>(fixture.db.getStorage()->count<ReplyRuleRow>()), 1);
    ASSERT_EQ(replies.listRules()->front().id, variantId);
}

TEST(ReplyDeduplication, RemovesHistoricalRowsOnLoadAndKeepsOldestId) {
    ReplyFixture fixture;
    ReplyRuleRow row;
    row.matchType = static_cast<int>(MatchType::kKeyword);
    row.matchContent = "重复";
    row.replyContent = "内容";
    row.enabled = true;
    row.priority = 100;
    row.logic = "or";
    row.createdAt = "2026-01-01T00:00:00Z";
    row.updatedAt = row.createdAt;

    const int oldestId = static_cast<int>(fixture.db.getStorage()->insert(row));
    row.createdAt = "2026-02-01T00:00:00Z";
    row.updatedAt = row.createdAt;
    ASSERT_TRUE(fixture.db.getStorage()->insert(row) > oldestId);

    ReplyManager replies{fixture.db, fixture.config};
    ASSERT_EQ(replies.listRules()->size(), size_t(1));
    ASSERT_EQ(replies.listRules()->front().id, oldestId);
    ASSERT_EQ(static_cast<int>(fixture.db.getStorage()->count<ReplyRuleRow>()), 1);
}
