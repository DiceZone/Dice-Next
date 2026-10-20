#include "test_framework.h"
#include "../src/service/game_session_list.h"
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>

using namespace dice::game_sessions;
namespace {
struct Catalog {
    sqlite3* db = nullptr;
    Json active = Json::object();
    std::set<std::string> requestedGroups;
    Names names;
    Catalog() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) throw std::runtime_error("test database open");
        exec("CREATE TABLE game_logs(id INTEGER PRIMARY KEY, group_id TEXT, gm_id TEXT, status INTEGER, created_at TEXT, custom_rules TEXT)");
        exec("CREATE TABLE game_log_messages(content TEXT)");
    }
    ~Catalog() { sqlite3_close(db); }
    void exec(const std::string& sql) {
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    void session(const std::string& code, const std::string& name, Json groups = Json::array(),
                 Json gms = Json::array(), Json players = Json::array()) {
        active[code] = Json{{"__name", name}, {"__areas", groups.dump()}, {"__gms", gms.dump()}, {"__pls", players.dump()}};
    }
    void log(const std::string& code, const std::string& name, const std::string& group, const std::string& gm,
             int status = 2, const std::string& at = "2026-10-20", const std::string& raw = {}) {
        auto stmt = detail::prepare(db, "INSERT INTO game_logs(group_id,gm_id,status,created_at,custom_rules) VALUES(?1,?2,?3,?4,?5)");
        detail::bind(stmt.get(), 1, group); detail::bind(stmt.get(), 2, gm);
        sqlite3_bind_int(stmt.get(), 3, status); detail::bind(stmt.get(), 4, at);
        detail::bind(stmt.get(), 5, raw.empty() ? Json{{"gameCode",code},{"gameName",name}}.dump() : raw);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    }
    Json page(Options options = {}) {
        return list(db, active, options, [&](const auto& ids) {
            requestedGroups = ids;
            Names found;
            for (const auto& id : ids) if (names.count(id)) found[id] = names[id];
            return found;
        });
    }
};
std::string code(int value) {
    std::ostringstream out; out << 'S' << std::setfill('0') << std::setw(5) << value; return out.str();
}
} // namespace

TEST(GameSessionList, MoreThanTwoThousandSessionsArePagedBeforeResponseAssembly) {
    Catalog f;
    f.exec("BEGIN");
    for (int i = 0; i < 2507; ++i) {
        if (i < 2100) f.session(code(i), "团务" + std::to_string(i), Json::array({"group-" + std::to_string(i)}));
        f.log(code(i), "历史团名", "group-" + std::to_string(i), "gm");
    }
    f.exec("COMMIT");
    Options q; q.sort = "code"; q.direction = "asc";
    const auto started = std::chrono::steady_clock::now();
    auto first = f.page(q);
    ASSERT_EQ(first["total"], 2507);
    ASSERT_EQ(first["items"].size(), std::size_t(20));
    ASSERT_EQ(first["items"][0]["code"], code(0));
    ASSERT_EQ(f.requestedGroups.size(), std::size_t(20));
    q.page = 64; auto middle = f.page(q);
    ASSERT_EQ(middle["items"][0]["code"], code(1260));
    q.page = 126; auto last = f.page(q);
    ASSERT_EQ(last["items"].size(), std::size_t(7));
    ASSERT_EQ(last["items"][6]["code"], code(2506));
    ASSERT_EQ(f.requestedGroups.size(), std::size_t(7));
    std::cout << "(2,507 sessions: first/middle/last queries "
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()
              << " ms) ";
}

TEST(GameSessionList, ActiveAndHistoricalSourcesMergeWithoutDuplicatesAndRetainCounts) {
    Catalog f; f.session("live", "当前团名", Json::array({"current-group"}), Json::array({"current-gm"}), Json::array({"player"}));
    f.log("live", "旧团名", "old-group", "old-gm", 0, "2026-10-19");
    f.log("live", "旧团名", "old-group", "old-gm", 1, "2026-10-18");
    f.log("live", "旧团名", "old-group", "old-gm", 2);
    f.log("ended", "已结束", "ended-group", "", 2);
    f.session("no-log", "尚无日志");
    auto result = f.page();
    ASSERT_EQ(result["total"], 3);
    ASSERT_EQ(result["items"][0]["code"], "live");
    const auto& live = result["items"][0];
    ASSERT_EQ(live["name"], "当前团名"); ASSERT_EQ(live["groups"], Json::array({"current-group"}));
    ASSERT_EQ(live["players"], Json::array({"player"})); ASSERT_EQ(live["createdAt"], "2026-10-18");
    ASSERT_EQ(live["logCount"], 3); ASSERT_EQ(live["activeLogs"], 1);
    ASSERT_EQ(live["pausedLogs"], 1); ASSERT_EQ(live["endedLogs"], 1);
    ASSERT_TRUE(live["active"].is_boolean()); ASSERT_TRUE(live["active"].get<bool>());
    ASSERT_FALSE(result["items"][2]["active"].get<bool>());
    ASSERT_EQ(result["items"][2]["gms"], Json::array());
}

TEST(GameSessionList, SearchCoversAllRowsAndMemberFieldsWithLiteralMetacharacters) {
    Catalog f;
    for (int i=0; i<55; ++i) f.session(code(i), "一般团务");
    f.session("LAST-AbC", "深夜调查_%'\\", Json::array({"987654"}), Json::array({"gm-special"}), Json::array({"player-special"}));
    f.names["987654"] = "秘密基地";
    Options q;
    for (const std::string needle : {"调查", "last-abc", "987654", "GM-special", "PLAYER-special", "秘密基地", "_%'\\"}) {
        q.query = needle; auto result = f.page(q);
        ASSERT_EQ(result["total"], 1); ASSERT_EQ(result["items"][0]["code"], "LAST-AbC");
        ASSERT_EQ(result["items"][0]["groupNames"]["987654"], "秘密基地");
    }
    q.query = "% OR 1=1 --"; ASSERT_EQ(f.page(q)["total"], 0);
    q.query = "__name"; ASSERT_EQ(f.page(q)["total"], 0);
}

TEST(GameSessionList, SortIsGlobalWhitelistedAndStableAcrossPageBoundaries) {
    Catalog f;
    for (int i=0; i<45; ++i) f.session(code(i), "same");
    f.session("zzz", "Alpha"); f.session("aaa", "Zulu");
    Options q; q.sort="name"; q.direction="asc"; q.size=10;
    ASSERT_EQ(f.page(q)["items"][0]["code"], "zzz");
    q.direction="desc"; ASSERT_EQ(f.page(q)["items"][0]["code"], "aaa");
    q.direction="asc";
    std::set<std::string> seen;
    for (int p=1; p<=5; ++p) {
        q.page=p;
        const auto result = f.page(q);
        for (const auto& item : result["items"]) seen.insert(item["code"].get<std::string>());
    }
    ASSERT_EQ(seen.size(), std::size_t(47));
    q.sort="name; DROP TABLE game_logs; --"; q.page=1;
    ASSERT_EQ(f.page(q)["total"], 47);
    q.sort="logCount"; f.log("zzz", "old", "group", "gm");
    q.direction="desc"; ASSERT_EQ(f.page(q)["items"][0]["code"], "zzz");
    q.sort="createdAt"; ASSERT_EQ(f.page(q)["items"][0]["code"], "zzz");
}

TEST(GameSessionList, InvalidMetadataIsIgnoredWithoutBreakingTheWholePage) {
    Catalog f;
    f.log("", "broken", "g", "", 2, "", "not json");
    f.log("", "array", "g", "", 2, "", "[]");
    f.log("", "number", "g", "", 2, "", R"({"gameCode":12})");
    f.log("ended", "ended", "g", "", 2);
    f.active["empty"] = Json{{"__name", ""}};
    f.active["bad"] = "wrong type";
    f.session("live", "有效团", Json::array({123, "456", Json::object(), true}), Json::array({789}));
    f.active["live"]["__pls"] = "broken array";
    auto result = f.page();
    ASSERT_EQ(result["total"], 2);
    ASSERT_EQ(result["items"][0]["groups"], Json::array({"123", "456"}));
    ASSERT_EQ(result["items"][0]["gms"], Json::array({"789"}));
    ASSERT_EQ(result["items"][0]["players"], Json::array());
}

TEST(GameSessionList, PagesClampAfterDeletionAndBoundIntegerOverflow) {
    Catalog f;
    for (int i=0; i<21; ++i) f.session(code(i), "团务");
    Options q; q.page=2; ASSERT_EQ(f.page(q)["items"].size(), std::size_t(1));
    f.active.erase(code(20)); auto after = f.page(q);
    ASSERT_EQ(after["page"], 1); ASSERT_EQ(after["items"].size(), std::size_t(20));
    q.page=std::numeric_limits<int>::max(); ASSERT_EQ(f.page(q)["page"], 1);
    q.page=-5; q.size=0; ASSERT_EQ(f.page(q)["size"], 1);
    q.size=10000; ASSERT_EQ(f.page(q)["size"], 100);
    f.active.clear(); auto empty=f.page(q);
    ASSERT_EQ(empty["page"], 1); ASSERT_EQ(empty["total"], 0); ASSERT_TRUE(empty["items"].empty());
    ASSERT_EQ(integerParameter("12oops", 1), 1); ASSERT_EQ(integerParameter("999999999999999", 1), 1);
    ASSERT_EQ(integerParameter("", 20), 20); ASSERT_EQ(integerParameter("12", 1), 12);
}

TEST(GameSessionList, OldArrayContractRemainsAndListNeverReadsMessagesOrWritesStorage) {
    Catalog f;
    for (int i=0; i<24; ++i) f.log(code(i), "团务", "g", "gm", 2);
    sqlite3_set_authorizer(f.db, [](void*, int action, const char* table, const char*, const char*, const char*) {
        if (action==SQLITE_READ && table && std::string(table)=="game_log_messages") return SQLITE_DENY;
        if (action==SQLITE_INSERT || action==SQLITE_UPDATE || action==SQLITE_DELETE) return SQLITE_DENY;
        return SQLITE_OK;
    }, nullptr);
    ASSERT_EQ(f.page()["items"].size(), std::size_t(20));
    Options q; q.paginated=false; auto legacy=f.page(q);
    ASSERT_TRUE(legacy.is_array()); ASSERT_EQ(legacy.size(), std::size_t(24));
    ASSERT_TRUE(f.active.empty());
    ASSERT_EQ(sqlite3_total_changes(f.db), 24);
}
