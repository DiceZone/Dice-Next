#include "test_framework.h"
#include "core/reply/reply_definition.h"
#include "core/reply/reply_manager.h"

#include <chrono>
#include <filesystem>

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
