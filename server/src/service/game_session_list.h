#pragma once

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace dice::game_sessions {

using Json = nlohmann::json;
using Names = std::map<std::string, std::string>;
using NameLookup = std::function<Names(const std::set<std::string>&)>;

struct Options {
    bool paginated = true;
    int page = 1, size = 20;
    std::string query, sort = "status", direction = "desc";
};

inline int integerParameter(const std::string& value, int fallback) {
    int out = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() ? out : fallback;
}

namespace detail {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
inline Statement prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (!db || sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(db ? sqlite3_errmsg(db) : "log database unavailable");
    return Statement(stmt, sqlite3_finalize);
}
inline void bind(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("cannot bind game session query");
}
inline std::string text(sqlite3_stmt* stmt, int column) {
    const auto* value = sqlite3_column_text(stmt, column);
    return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(stmt, column)) : std::string();
}
inline std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
inline Json members(const Json& values, const char* key) {
    const auto found = values.find(key);
    if (found == values.end() || !found->is_string()) return Json::array();
    const auto parsed = Json::parse(found->get_ref<const std::string&>(), nullptr, false);
    auto out = Json::array();
    if (parsed.is_array()) for (const auto& item : parsed) {
        if (item.is_string()) out.push_back(item);
        else if (item.is_number_integer()) out.push_back(item.dump());
    }
    return out;
}
inline std::string activeJson(const Json& snapshot) {
    auto out = Json::array();
    if (snapshot.is_object()) for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (!it.value().is_object()) continue;
        const auto name = it.value().find("__name");
        if (name == it.value().end() || !name->is_string() || name->get_ref<const std::string&>().empty()) continue;
        out.push_back(Json{{"code", it.key()}, {"name", *name}, {"groups", members(it.value(), "__areas")},
                           {"gms", members(it.value(), "__gms")}, {"players", members(it.value(), "__pls")}});
    }
    return out.dump();
}

// 只读日志元数据，不读取消息、图片或整条日志对象。活动团务来自单次配置快照。
// 两个来源在 SQL 中合并、去重、筛选、排序；LIMIT/OFFSET 之后才组装响应。
inline const char* catalogSql() {
    return R"sql(
        WITH active AS MATERIALIZED (
            SELECT json_extract(value,'$.code') AS code, json_extract(value,'$.name') AS name,
                json_extract(value,'$.groups') AS groups, json_extract(value,'$.gms') AS gms,
                json_extract(value,'$.players') AS players FROM json_each(?1)
        ), metadata AS (
            SELECT id, group_id, gm_id, status, created_at,
                CASE WHEN json_valid(custom_rules) THEN custom_rules ELSE '{}' END AS meta FROM game_logs
        ), log_info AS MATERIALIZED (
            SELECT id, group_id, gm_id, status, created_at,
                json_extract(meta,'$.gameCode') AS code,
                CASE WHEN json_type(meta,'$.gameName')='text' THEN json_extract(meta,'$.gameName')
                     ELSE '未命名团务' END AS name
            FROM metadata WHERE json_type(meta,'$.gameCode')='text' AND json_extract(meta,'$.gameCode')<>''
        ), logged AS MATERIALIZED (
            SELECT code, MIN(id) AS firstId, json_group_array(DISTINCT group_id) AS groups,
                json_group_array(DISTINCT gm_id) FILTER (WHERE gm_id<>'') AS gms,
                MIN(created_at) AS createdAt, COUNT(*) AS logCount,
                SUM(status=0) AS activeLogs, SUM(status=1) AS pausedLogs,
                SUM(status<>0 AND status<>1) AS endedLogs FROM log_info GROUP BY code
        ), sessions AS (
            SELECT a.code, a.name, a.groups, a.gms, a.players, 1 AS active,
                COALESCE(l.createdAt,'') AS createdAt, COALESCE(l.logCount,0) AS logCount,
                COALESCE(l.activeLogs,0) AS activeLogs, COALESCE(l.pausedLogs,0) AS pausedLogs,
                COALESCE(l.endedLogs,0) AS endedLogs
            FROM active AS a LEFT JOIN logged AS l ON l.code=a.code
            UNION ALL
            SELECT l.code, i.name, l.groups, l.gms, '[]', 0, l.createdAt, l.logCount,
                l.activeLogs, l.pausedLogs, l.endedLogs
            FROM logged AS l JOIN log_info AS i ON i.id=l.firstId
            LEFT JOIN active AS a ON a.code=l.code WHERE a.code IS NULL
        )
    )sql";
}
} // namespace detail

inline Json list(sqlite3* logs, const Json& snapshot, Options options, const NameLookup& lookup) {
    options.page = (std::max)(1, options.page);
    options.size = std::clamp(options.size, 1, 100);
    const auto active = detail::activeJson(snapshot);
    Names names;
    auto matchedGroups = Json::array();
    if (!options.query.empty()) {
        // 按群名搜索也覆盖所有团务，但仅取群号；无需加载全部团务卡片或消息。
        auto stmt = detail::prepare(logs, std::string(detail::catalogSql()) +
            " SELECT DISTINCT CAST(g.value AS TEXT) FROM sessions, json_each(sessions.groups) AS g");
        detail::bind(stmt.get(), 1, active);
        std::set<std::string> ids;
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) ids.insert(detail::text(stmt.get(), 0));
        if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(logs));
        names = lookup(ids);
        const auto query = detail::lower(options.query);
        for (const auto& [id, name] : names)
            if (detail::lower(name).find(query) != std::string::npos) matchedGroups.push_back(id);
    }
    static const std::map<std::string, std::string> sorts{
        {"name", "name COLLATE NOCASE"}, {"code", "code COLLATE NOCASE"}, {"status", "active"},
        {"createdAt", "createdAt"}, {"logCount", "logCount"}
    };
    const auto field = sorts.find(options.sort);
    const auto order = (field == sorts.end() ? "active" : field->second) +
        std::string(options.direction == "asc" ? " ASC" : " DESC") + ", code COLLATE BINARY ASC";
    const std::string sql = std::string(detail::catalogSql()) + R"sql(
        , filtered AS (
            SELECT * FROM sessions WHERE ?2='' OR instr(lower(name),lower(?2))>0
                OR instr(lower(code),lower(?2))>0 OR EXISTS (
                    SELECT 1 FROM json_each(groups) AS member WHERE instr(lower(CAST(member.value AS TEXT)),lower(?2))>0
                        OR member.value IN (SELECT value FROM json_each(?3))
                ) OR EXISTS (SELECT 1 FROM json_each(gms) WHERE instr(lower(CAST(value AS TEXT)),lower(?2))>0)
                  OR EXISTS (SELECT 1 FROM json_each(players) WHERE instr(lower(CAST(value AS TEXT)),lower(?2))>0)
        ), paging AS (
            SELECT COUNT(*) AS total, MIN(?4,MAX(1,(COUNT(*)+?5-1)/?5)) AS page FROM filtered
        )
        SELECT total, page, COALESCE((SELECT json_group_array(json_object(
            'code', code, 'name', name, 'groups', json(groups), 'gms', json(gms), 'players', json(players),
            'active', json(CASE WHEN active=1 THEN 'true' ELSE 'false' END), 'createdAt', createdAt,
            'logCount', logCount, 'activeLogs', activeLogs, 'pausedLogs', pausedLogs, 'endedLogs', endedLogs
        )) FROM (SELECT * FROM filtered ORDER BY
    )sql" + order + R"sql(
            LIMIT ?6 OFFSET (SELECT (page-1)*?5 FROM paging))), '[]') FROM paging
    )sql";
    auto stmt = detail::prepare(logs, sql);
    detail::bind(stmt.get(), 1, active);
    detail::bind(stmt.get(), 2, options.query);
    detail::bind(stmt.get(), 3, matchedGroups.dump());
    sqlite3_bind_int(stmt.get(), 4, options.paginated ? options.page : 1);
    sqlite3_bind_int(stmt.get(), 5, options.size);
    sqlite3_bind_int(stmt.get(), 6, options.paginated ? options.size : -1);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(logs));
    const auto total = sqlite3_column_int64(stmt.get(), 0);
    const int page = sqlite3_column_int(stmt.get(), 1);
    auto items = Json::parse(detail::text(stmt.get(), 2));
    std::set<std::string> ids;
    for (const auto& session : items) for (const auto& id : session["groups"])
        if (id.is_string()) ids.insert(id.get<std::string>());
    if (options.query.empty()) names = lookup(ids);
    for (auto& session : items) {
        auto groupNames = Json::object();
        for (const auto& id : session["groups"]) if (id.is_string()) {
            const auto found = names.find(id.get<std::string>());
            if (found != names.end()) groupNames[found->first] = found->second;
        }
        session["groupNames"] = std::move(groupNames);
    }
    if (!options.paginated) return items; // 兼容无 page 参数的旧客户端。
    return Json{{"items", items}, {"total", total}, {"page", page}, {"size", options.size}};
}

} // namespace dice::game_sessions
