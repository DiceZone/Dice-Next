// 日志列表的用量统计：在数据库内聚合，并在消息只增不减时增量更新。

#include "test_framework.h"
#include "../src/service/log_usage.h"
#include "../src/storage/database.h"

#include <chrono>
#include <filesystem>

using namespace dice;
namespace fs = std::filesystem;

namespace {

struct LogDb {
    fs::path root;
    Database db;
    LogDb() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("dice_next_log_usage_" + std::to_string(nonce));
        fs::create_directories(root);
        db.open((root / "dice.db").string());
    }
    ~LogDb() {
        db.close();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    auto* logs() { return db.getLogStorage(); }
    sqlite3* raw() { return db.getLogConnection(); }
    int add(int logId, const std::string& content, const std::string& at) {
        GameLogMessageRow row;
        row.logId = logId; row.sender = "希亚"; row.userId = "10001";
        row.content = content; row.createdAt = at; row.images = "[]";
        return logs()->insert(row);
    }
};

std::uint64_t bytesOf(const std::string& sender, const std::string& user, const std::string& content,
                      const std::string& at, const std::string& images) {
    return sizeof(int) * 2 + sender.size() + user.size() + content.size() + at.size() + images.size();
}

} // namespace

TEST(LogUsage, LogMessagesHaveLogIdIndex) {
    LogDb t;
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(t.raw(), "SELECT 1 FROM sqlite_master WHERE type='index' "
        "AND name='idx_game_log_messages_log_id'", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    sqlite3_finalize(stmt);
}

TEST(LogUsage, AggregatesCountsLastTimeAndUtf8Bytes) {
    LogDb t;
    t.add(1, "掷骰", "2026-10-19T10:00:00");
    t.add(1, "abc", "2026-10-19T11:00:00");
    t.add(2, "x", "2026-10-18T09:00:00");
    log_usage::UsageMap usage;
    ASSERT_TRUE(log_usage::aggregate(t.raw(), 0, usage));
    ASSERT_EQ(usage[1].count, 2);
    ASSERT_EQ(usage[1].lastAt, std::string("2026-10-19T11:00:00"));
    ASSERT_EQ(usage[1].databaseBytes,
        bytesOf("希亚", "10001", "掷骰", "2026-10-19T10:00:00", "[]")
        + bytesOf("希亚", "10001", "abc", "2026-10-19T11:00:00", "[]"));
    ASSERT_EQ(usage[2].count, 1);
}

TEST(LogUsage, CacheAddsOnlyNewMessagesAndRecountsAfterDelete) {
    LogDb t;
    log_usage::Cache cache;
    t.add(1, "a", "2026-10-19T10:00:00");
    log_usage::UsageMap usage;
    ASSERT_TRUE(cache.snapshot(t.raw(), usage));
    ASSERT_EQ(usage[1].count, 1);

    t.add(1, "b", "2026-10-19T10:01:00");
    const int other = t.add(3, "c", "2026-10-19T10:02:00");
    ASSERT_TRUE(cache.snapshot(t.raw(), usage));
    ASSERT_EQ(usage[1].count, 2);
    ASSERT_EQ(usage[1].lastAt, std::string("2026-10-19T10:01:00"));
    ASSERT_EQ(usage[3].count, 1);

    t.logs()->remove<GameLogMessageRow>(other);
    t.add(1, "d", "2026-10-19T10:03:00");
    ASSERT_TRUE(cache.snapshot(t.raw(), usage));
    ASSERT_EQ(usage[1].count, 3);
    ASSERT_TRUE(usage.find(3) == usage.end());
}
