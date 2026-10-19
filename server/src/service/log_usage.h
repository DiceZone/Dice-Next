#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dice::log_usage {

struct Usage {
    int count = 0;
    std::string lastAt;
    std::uint64_t databaseBytes = 0;
    std::uint64_t imageBytes = 0;
};

using UsageMap = std::unordered_map<int, Usage>;

/// 在数据库内按日志汇总消息条数、最后时间与字段字节数，不把消息内容读进进程内存。
/// 只统计 id 大于 afterId 的消息，结果累加进 out。
inline bool aggregate(sqlite3* db, std::int64_t afterId, UsageMap& out) {
    static constexpr const char* kSql =
        "SELECT log_id, COUNT(*), MAX(created_at), "
        "TOTAL(LENGTH(CAST(message_id AS BLOB))) + TOTAL(LENGTH(CAST(sender AS BLOB))) "
        "+ TOTAL(LENGTH(CAST(user_id AS BLOB))) + TOTAL(LENGTH(CAST(content AS BLOB))) "
        "+ TOTAL(LENGTH(CAST(created_at AS BLOB))) + TOTAL(LENGTH(CAST(images AS BLOB))) "
        "FROM game_log_messages WHERE id > ? GROUP BY log_id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, afterId);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        auto& item = out[sqlite3_column_int(stmt, 0)];
        const int count = sqlite3_column_int(stmt, 1);
        item.count += count;
        if (const auto* text = sqlite3_column_text(stmt, 2)) {
            std::string lastAt(reinterpret_cast<const char*>(text));
            if (lastAt > item.lastAt) item.lastAt = std::move(lastAt);
        }
        // 与逐行统计时一致：每条消息另计 id 与 log_id 两个整数字段。
        item.databaseBytes += static_cast<std::uint64_t>(sqlite3_column_double(stmt, 3))
            + static_cast<std::uint64_t>(count) * (sizeof(int) * 2);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

inline bool scalarPair(sqlite3* db, const char* sql, std::int64_t bind, std::int64_t& a, std::int64_t& b) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    if (bind >= 0) sqlite3_bind_int64(stmt, 1, bind);
    const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) { a = sqlite3_column_int64(stmt, 0); b = sqlite3_column_int64(stmt, 1); }
    sqlite3_finalize(stmt);
    return ok;
}

/// 日志列表反复打开时复用上次的汇总：消息未变化直接返回；只新增了消息则只汇总新增部分；
/// 有删除时重新汇总。
class Cache {
public:
    bool snapshot(sqlite3* db, UsageMap& out) {
        std::lock_guard lock(mutex_);
        std::int64_t total = 0, maxId = 0;
        if (!scalarPair(db, "SELECT COUNT(*), COALESCE(MAX(id), 0) FROM game_log_messages", -1, total, maxId))
            return false;
        if (!valid_ || total != total_ || maxId != maxId_) {
            std::int64_t added = 0, unused = 0;
            const bool appendOnly = valid_ && maxId > maxId_
                && scalarPair(db, "SELECT COUNT(*), 0 FROM game_log_messages WHERE id > ?", maxId_, added, unused)
                && total_ + added == total;
            UsageMap next = appendOnly ? usage_ : UsageMap{};
            if (!aggregate(db, appendOnly ? maxId_ : 0, next)) return false;
            usage_ = std::move(next);
            total_ = total; maxId_ = maxId; valid_ = true;
        }
        out = usage_;
        return true;
    }

private:
    std::mutex mutex_;
    UsageMap usage_;
    std::int64_t total_ = 0, maxId_ = 0;
    bool valid_ = false;
};

} // namespace dice::log_usage
