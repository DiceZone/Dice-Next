#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dice::log_usage {

struct Usage {
    int count = 0;
    std::string lastAt;
    std::uint64_t databaseBytes = 0;
    std::uint64_t imageBytes = 0;
};

using UsageMap = std::unordered_map<int, Usage>;

/// 只靠 (log_id, id) 索引就能得到的统计：条数与最后一条消息的编号（编号随时间递增）。
struct Brief {
    int count = 0;
    std::int64_t lastId = 0;
};

using BriefMap = std::unordered_map<int, Brief>;

namespace detail {

inline const char* usageColumns() {
    return "SELECT log_id, COUNT(*), MAX(created_at), "
           "TOTAL(LENGTH(CAST(message_id AS BLOB))) + TOTAL(LENGTH(CAST(sender AS BLOB))) "
           "+ TOTAL(LENGTH(CAST(user_id AS BLOB))) + TOTAL(LENGTH(CAST(content AS BLOB))) "
           "+ TOTAL(LENGTH(CAST(created_at AS BLOB))) + TOTAL(LENGTH(CAST(images AS BLOB))) "
           "FROM game_log_messages ";
}

inline bool readUsage(sqlite3_stmt* stmt, UsageMap& out) {
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

inline std::string placeholders(std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) out += i ? ",?" : "?";
    return out;
}

} // namespace detail

/// 在数据库内按日志汇总消息条数、最后时间与字段字节数，不把消息内容读进进程内存。
/// 只统计 id 大于 afterId 的消息，结果累加进 out。
inline bool aggregate(sqlite3* db, std::int64_t afterId, UsageMap& out) {
    const std::string sql = std::string(detail::usageColumns()) + "WHERE id > ? GROUP BY log_id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, afterId);
    return detail::readUsage(stmt, out);
}

/// 只汇总指定日志（一页的用量），经 log_id 索引定位，不扫描其他日志。
inline bool aggregateFor(sqlite3* db, const std::vector<int>& logIds, UsageMap& out) {
    if (logIds.empty()) return true;
    const std::string sql = std::string(detail::usageColumns()) + "WHERE log_id IN ("
        + detail::placeholders(logIds.size()) + ") GROUP BY log_id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    for (std::size_t i = 0; i < logIds.size(); ++i) sqlite3_bind_int(stmt, static_cast<int>(i + 1), logIds[i]);
    return detail::readUsage(stmt, out);
}

/// 各日志的条数与最后消息编号；只读索引，不读消息内容。id 大于 afterId 的部分累加进 out。
inline bool briefAll(sqlite3* db, std::int64_t afterId, BriefMap& out) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT log_id, COUNT(*), MAX(id) FROM game_log_messages "
            "WHERE id > ? GROUP BY log_id", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, afterId);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        auto& item = out[sqlite3_column_int(stmt, 0)];
        item.count += sqlite3_column_int(stmt, 1);
        const auto lastId = sqlite3_column_int64(stmt, 2);
        if (lastId > item.lastId) item.lastId = lastId;
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
/// 有删除时重新汇总。Aggregate 把 id 大于给定值的消息累加进结果。
template <typename Map, bool (*Aggregate)(sqlite3*, std::int64_t, Map&)>
class IncrementalCache {
public:
    bool snapshot(sqlite3* db, Map& out) {
        std::lock_guard lock(mutex_);
        std::int64_t total = 0, maxId = 0;
        if (!scalarPair(db, "SELECT COUNT(*), COALESCE(MAX(id), 0) FROM game_log_messages", -1, total, maxId))
            return false;
        if (!valid_ || total != total_ || maxId != maxId_) {
            std::int64_t added = 0, unused = 0;
            const bool appendOnly = valid_ && maxId > maxId_
                && scalarPair(db, "SELECT COUNT(*), 0 FROM game_log_messages WHERE id > ?", maxId_, added, unused)
                && total_ + added == total;
            Map next = appendOnly ? data_ : Map{};
            if (!Aggregate(db, appendOnly ? maxId_ : 0, next)) return false;
            data_ = std::move(next);
            total_ = total; maxId_ = maxId; valid_ = true;
        }
        out = data_;
        return true;
    }

private:
    std::mutex mutex_;
    Map data_;
    std::int64_t total_ = 0, maxId_ = 0;
    bool valid_ = false;
};

using Cache = IncrementalCache<UsageMap, aggregate>;
using BriefCache = IncrementalCache<BriefMap, briefAll>;

} // namespace dice::log_usage
