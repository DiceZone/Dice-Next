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

TEST(LogUsage, DatabasesUseWriteAheadLogging) {
    LogDb t;
    auto journalMode = [](sqlite3* db) {
        sqlite3_stmt* stmt = nullptr;
        std::string mode;
        if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, nullptr) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW)
            mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        return mode;
    };
    ASSERT_EQ(journalMode(t.raw()), std::string("wal"));
    // 日志模式记录在库文件里：另开连接看到的主库同样是 WAL。
    sqlite3* other = nullptr;
    ASSERT_EQ(sqlite3_open((t.root / "dice.db").string().c_str(), &other), SQLITE_OK);
    ASSERT_EQ(journalMode(other), std::string("wal"));
    sqlite3_close(other);
}

TEST(LogUsage, BriefAndPageAggregatesOnlyCoverRequestedData) {
    LogDb t;
    t.add(1, "a", "2026-10-19T10:00:00");
    const int last = t.add(1, "b", "2026-10-19T10:05:00");
    t.add(2, "c", "2026-10-19T09:00:00");
    log_usage::BriefMap brief;
    ASSERT_TRUE(log_usage::briefAll(t.raw(), 0, brief));
    ASSERT_EQ(brief[1].count, 2);
    ASSERT_EQ(brief[1].lastId, static_cast<std::int64_t>(last));
    ASSERT_EQ(brief[2].count, 1);

    log_usage::BriefCache cache;
    log_usage::BriefMap cached;
    ASSERT_TRUE(cache.snapshot(t.raw(), cached));
    const int newest = t.add(2, "d", "2026-10-19T11:00:00");
    ASSERT_TRUE(cache.snapshot(t.raw(), cached));
    ASSERT_EQ(cached[2].count, 2);
    ASSERT_EQ(cached[2].lastId, static_cast<std::int64_t>(newest));

    log_usage::UsageMap page;
    ASSERT_TRUE(log_usage::aggregateFor(t.raw(), {2}, page));
    ASSERT_EQ(page.size(), size_t(1));
    ASSERT_EQ(page[2].count, 2);
    ASSERT_EQ(page[2].lastAt, std::string("2026-10-19T11:00:00"));
    log_usage::UsageMap none;
    ASSERT_TRUE(log_usage::aggregateFor(t.raw(), {}, none));
    ASSERT_TRUE(none.empty());
}

