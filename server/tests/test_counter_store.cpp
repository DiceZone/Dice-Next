// ─── C#29: CounterStore Unit Tests ────────────────────────────
// Tests the persistent counter storage (requires an in-memory SQLite DB).
// Covers: get, set, add, reset, listAll, listByRule, deleteByRule,
//         key parsing, scope isolation.

#include "test_framework.h"
#include "../src/core/causal/counter_store.h"
#include "../src/storage/database.h"

using namespace dice;

// Helper: create a fresh in-memory database for each test
static std::unique_ptr<Database> makeDb() {
    auto db = std::make_unique<Database>();
    db->open(":memory:");  // sqlite_orm supports in-memory mode
    return db;
}

TEST(Counter, GetReturnsZeroForMissingKey) {
    auto db = makeDb();
    CounterStore store(*db);
    ASSERT_EQ(store.get("5:sign_days:user:12345"), 0);
}

TEST(Counter, SetAndGet) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:sign_days:user:12345", 42);
    ASSERT_EQ(store.get("5:sign_days:user:12345"), 42);
}

TEST(Counter, SetOverwritesExisting) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:sign_days:user:12345", 10);
    store.set("5:sign_days:user:12345", 99);
    ASSERT_EQ(store.get("5:sign_days:user:12345"), 99);
}

TEST(Counter, AddReturnsNewValue) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:12345", 10);
    int newVal = store.add("5:count:user:12345", 5);
    ASSERT_EQ(newVal, 15);
    ASSERT_EQ(store.get("5:count:user:12345"), 15);
}

TEST(Counter, AddNegativeDelta) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:12345", 10);
    int newVal = store.add("5:count:user:12345", -3);
    ASSERT_EQ(newVal, 7);
}

TEST(Counter, AddToNonexistentStartsFromZero) {
    auto db = makeDb();
    CounterStore store(*db);
    int newVal = store.add("5:count:user:12345", 5);
    ASSERT_EQ(newVal, 5);
}

TEST(Counter, ResetDeletesEntry) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:12345", 42);
    ASSERT_EQ(store.get("5:count:user:12345"), 42);
    store.reset("5:count:user:12345");
    ASSERT_EQ(store.get("5:count:user:12345"), 0);
}

TEST(Counter, ResetNonexistentIsNoop) {
    auto db = makeDb();
    CounterStore store(*db);
    store.reset("5:nonexistent:user:12345");
    // Should not throw
    ASSERT_EQ(store.get("5:nonexistent:user:12345"), 0);
}

TEST(Counter, ListAllReturnsAllEntries) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:111", 1);
    store.set("5:count:user:222", 2);
    store.set("6:total:global:", 99);
    auto entries = store.listAll();
    ASSERT_EQ((int)entries.size(), 3);
}

TEST(Counter, ListAllEmptyReturnsEmpty) {
    auto db = makeDb();
    CounterStore store(*db);
    auto entries = store.listAll();
    ASSERT_EQ((int)entries.size(), 0);
}

TEST(Counter, ListByRuleReturnsMatchingOnly) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:111", 1);
    store.set("5:count:user:222", 2);
    store.set("6:total:global:", 99);
    auto entries = store.listByRule(5);
    ASSERT_EQ((int)entries.size(), 2);
}

TEST(Counter, ListByRuleNoMatchReturnsEmpty) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:111", 1);
    auto entries = store.listByRule(99);
    ASSERT_EQ((int)entries.size(), 0);
}

TEST(Counter, DeleteByRuleRemovesAllMatching) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:111", 1);
    store.set("5:count:user:222", 2);
    store.set("6:total:global:", 99);
    store.deleteByRule(5);
    ASSERT_EQ(store.get("5:count:user:111"), 0);
    ASSERT_EQ(store.get("5:count:user:222"), 0);
    // Rule 6's counter should be untouched
    ASSERT_EQ(store.get("6:total:global:"), 99);
}

TEST(Counter, DeleteByRuleNoMatchIsNoop) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:111", 1);
    store.deleteByRule(99);
    ASSERT_EQ(store.get("5:count:user:111"), 1);
}

TEST(Counter, ScopeIsolationPerUser) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:user:111", 10);
    store.set("5:count:user:222", 20);
    ASSERT_EQ(store.get("5:count:user:111"), 10);
    ASSERT_EQ(store.get("5:count:user:222"), 20);
}

TEST(Counter, ScopeIsolationPerGroup) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:count:per-group:111", 10);
    store.set("5:count:per-group:222", 20);
    ASSERT_EQ(store.get("5:count:per-group:111"), 10);
    ASSERT_EQ(store.get("5:count:per-group:222"), 20);
}

TEST(Counter, ScopeIsolationGlobal) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("5:total:global:", 100);
    ASSERT_EQ(store.get("5:total:global:"), 100);
    // Different key for per-user should be separate
    ASSERT_EQ(store.get("5:total:user:111"), 0);
}

TEST(Counter, EntryKeyParsing) {
    auto db = makeDb();
    CounterStore store(*db);
    store.set("7:sign_count:user:12345", 5);
    auto entries = store.listByRule(7);
    ASSERT_EQ((int)entries.size(), 1);
    EXPECT_EQ(entries[0].ruleId, 7);
    EXPECT_EQ(entries[0].counterName, "sign_count");
    EXPECT_EQ(entries[0].scope, "user");
    EXPECT_EQ(entries[0].scopeId, "12345");
    EXPECT_EQ(entries[0].value, 5);
}
