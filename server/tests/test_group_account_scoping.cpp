// ─── Issue #7: Per-Adapter-Account Group Settings Regression Tests ─────
// Two OneBot accounts on the same client used to overwrite each other's group
// list: refreshing the second account marked the first account's groups as
// "left" and shared group switches showed only the last writer's value.
//
// The fix stores switches/runtime state in group_account_settings keyed by
// adapter id; these tests lock in that behavior and the legacy fallback.

#include "test_framework.h"
#include "../src/storage/database.h"
#include "../src/storage/group_account_settings.h"
#include "../src/core/identity/identity_binding.h"
#include "../src/i18n/locale_resolver.h"
#include "../src/config/config_manager.h"

#include <memory>
#include <string>
#include <vector>

using namespace dice;

namespace {

std::unique_ptr<Database> makeDb() {
    auto db = std::make_unique<Database>();
    db->open(":memory:");
    return db;
}

std::unique_ptr<ConfigManager> makeCfg() {
    auto cfg = std::make_unique<ConfigManager>("config");
    cfg->load();
    return cfg;
}

// Seed the same shape of rows that group auto-discovery writes for one account:
// enabled + presence markers, so syncAccountGroupPresence() sees a realistic row.
void seedDiscoveredGroup(Database& db, const std::string& adapterId,
                         const std::string& groupId, const std::string& endpointId) {
    auto* st = db.getStorage();
    setAccountGroupSetting(*st, adapterId, "onebot_v11", groupId, endpointId, "__removed", "0");
    setAccountGroupSetting(*st, adapterId, "onebot_v11", groupId, endpointId, "left", "0");
    setAccountGroupSetting(*st, adapterId, "onebot_v11", groupId, endpointId, "enabled", "1");
}

std::string leftState(Database& db, const std::string& adapterId, const std::string& groupId) {
    return accountGroupSetting(*db.getStorage(), adapterId, "onebot_v11", groupId, "left");
}

}  // namespace

TEST(GroupAccountScoping, TableIsRegisteredInStorage) {
    auto db = makeDb();
    const auto names = db->getStorage()->table_names();
    ASSERT_TRUE(std::find(names.begin(), names.end(), std::string("group_account_settings")) != names.end());
}

TEST(GroupAccountScoping, TwoAccountsKeepIndependentSettings) {
    auto db = makeDb();
    auto* st = db->getStorage();

    setAccountGroupSetting(*st, "adapterA", "onebot_v11", "1001", "1001", "enabled", "1");
    setAccountGroupSetting(*st, "adapterB", "onebot_v11", "1001", "1001", "enabled", "0");
    setAccountGroupSetting(*st, "adapterA", "onebot_v11", "1001", "1001", "card", "A 的群名片");
    setAccountGroupSetting(*st, "adapterB", "onebot_v11", "1001", "1001", "card", "B 的群名片");

    ASSERT_EQ(accountGroupSetting(*st, "adapterA", "onebot_v11", "1001", "enabled"), std::string("1"));
    ASSERT_EQ(accountGroupSetting(*st, "adapterB", "onebot_v11", "1001", "enabled"), std::string("0"));
    ASSERT_EQ(accountGroupSetting(*st, "adapterA", "onebot_v11", "1001", "card"), std::string("A 的群名片"));
    ASSERT_EQ(accountGroupSetting(*st, "adapterB", "onebot_v11", "1001", "card"), std::string("B 的群名片"));

    // Updating A must never overwrite B (the original issue: last writer wins).
    setAccountGroupSetting(*st, "adapterA", "onebot_v11", "1001", "1001", "enabled", "0");
    ASSERT_EQ(accountGroupSetting(*st, "adapterA", "onebot_v11", "1001", "enabled"), std::string("0"));
    ASSERT_EQ(accountGroupSetting(*st, "adapterB", "onebot_v11", "1001", "enabled"), std::string("0"));
    ASSERT_EQ(accountGroupSetting(*st, "adapterB", "onebot_v11", "1001", "card"), std::string("B 的群名片"));
}

TEST(GroupAccountScoping, LegacySharedRowsRemainTheFallback) {
    auto db = makeDb();
    auto* st = db->getStorage();

    // Pre-issue#7 data: only the shared platform+group row exists.
    GroupSettingRow shared; shared.platform = "onebot_v11"; shared.groupId = "2002";
    shared.key = "enabled"; shared.value = "1"; st->insert(shared);

    // Before the account is observed, reads fall back to the shared row.
    ASSERT_EQ(accountGroupSetting(*st, "adapterA", "onebot_v11", "2002", "enabled"), std::string("1"));

    // First account write creates an account row and becomes authoritative;
    // the other account still sees the legacy shared value.
    setAccountGroupSetting(*st, "adapterA", "onebot_v11", "2002", "2002", "enabled", "0");
    ASSERT_EQ(accountGroupSetting(*st, "adapterA", "onebot_v11", "2002", "enabled"), std::string("0"));
    ASSERT_EQ(accountGroupSetting(*st, "adapterB", "onebot_v11", "2002", "enabled"), std::string("1"));
}

TEST(GroupAccountScoping, SyncOneAccountNeverMarksTheOtherAccountsGroupsLeft) {
    auto db = makeDb();

    // Account A is in groups 3001 and 3002; account B is in 3002 and 3003.
    seedDiscoveredGroup(*db, "adapterA", "3001", "3001");
    seedDiscoveredGroup(*db, "adapterA", "3002", "3002");
    seedDiscoveredGroup(*db, "adapterB", "3002", "3002");
    seedDiscoveredGroup(*db, "adapterB", "3003", "3003");

    // Refreshing B sees its current list (3002, 3003): B's own rows stay live.
    syncAccountGroupPresence(*db->getStorage(), "adapterB", "onebot_v11",
                             {{"3002", "3002"}, {"3003", "3003"}});
    ASSERT_EQ(leftState(*db, "adapterB", "3002"), std::string("0"));
    ASSERT_EQ(leftState(*db, "adapterB", "3003"), std::string("0"));
    // A's groups are untouched by B's refresh (this is the issue #7 regression).
    ASSERT_EQ(leftState(*db, "adapterA", "3001"), std::string("0"));
    ASSERT_EQ(leftState(*db, "adapterA", "3002"), std::string("0"));

    // Now A refreshes and only reports 3001: A's 3002 is archived...
    syncAccountGroupPresence(*db->getStorage(), "adapterA", "onebot_v11", {{"3001", "3001"}});
    ASSERT_EQ(leftState(*db, "adapterA", "3001"), std::string("0"));
    ASSERT_EQ(leftState(*db, "adapterA", "3002"), std::string("1"));
    // ...but B's 3002 must NOT be moved to left along with it.
    ASSERT_EQ(leftState(*db, "adapterB", "3002"), std::string("0"));
    ASSERT_EQ(leftState(*db, "adapterB", "3003"), std::string("0"));
}

TEST(GroupAccountScoping, EmptyLiveListIsTreatedAsNotArrivedYet) {
    auto db = makeDb();
    seedDiscoveredGroup(*db, "adapterA", "4001", "4001");
    seedDiscoveredGroup(*db, "adapterA", "4002", "4002");

    // A cold start where the async group list has not arrived must not archive
    // every known group.
    syncAccountGroupPresence(*db->getStorage(), "adapterA", "onebot_v11", {});
    ASSERT_EQ(leftState(*db, "adapterA", "4001"), std::string("0"));
    ASSERT_EQ(leftState(*db, "adapterA", "4002"), std::string("0"));
}

TEST(GroupAccountScoping, LegacySharedGroupsAreNotArchivedByAccountSync) {
    auto db = makeDb();
    auto* st = db->getStorage();
    seedDiscoveredGroup(*db, "adapterA", "5001", "5001");

    // Legacy shared-only row: no adapter has observed it yet.
    GroupSettingRow shared; shared.platform = "onebot_v11"; shared.groupId = "5009";
    shared.key = "enabled"; shared.value = "1"; st->insert(shared);

    syncAccountGroupPresence(*st, "adapterA", "onebot_v11", {{"5001", "5001"}});
    // The legacy group keeps working (no account-scoped "left" row is created).
    ASSERT_EQ(leftState(*db, "adapterA", "5009"), std::string());
    ASSERT_EQ(accountGroupSetting(*st, "adapterA", "onebot_v11", "5009", "enabled"), std::string("1"));
}

TEST(GroupAccountScoping, LocaleResolverPrefersAccountLocale) {
    auto db = makeDb();
    auto cfg = makeCfg();
    LocaleResolver resolver(*db, *cfg);
    setAccountGroupSetting(*db->getStorage(), "adapterA", "onebot_v11", "6001", "6001", "locale", "ja");

    Message m;
    m.platform = "onebot_v11"; m.targetId = "6001"; m.adapterId = "adapterA";
    m.type = MessageType::kGroup;
    ASSERT_TRUE(resolver.resolve(m) == Locale::kJa);

    // Another account without its own override keeps the configured default.
    Message m2 = m; m2.adapterId = "adapterB";
    ASSERT_TRUE(resolver.resolve(m2) != Locale::kJa);
}

TEST(GroupAccountScoping, OfficialEndpointPhantomRowsAreCleanedUp) {
    auto db = makeDb();
    auto* st = db->getStorage();

    // 官方群端点：原始 OpenID → 公共群号 20001。
    IdentityRow ent; ent.kind = "group"; ent.publicId = "20001"; ent.isVirtual = false; st->insert(ent);
    ent.id = st->get_all<IdentityRow>().front().id;   // 回读自增主键
    IdentityEndpointRow ep; ep.identityId = ent.id; ep.kind = "group";
    ep.adapterType = "qq_official"; ep.adapterAccount = "botA"; ep.endpointId = "OPENID_GROUP_1"; st->insert(ep);

    // 旧事件路径留下的幽灵行：groupId 是原始 OpenID（left=1，显示已退群）。
    GroupAccountSettingRow ph; ph.adapterId = "adapterA"; ph.platform = "qq_official";
    ph.groupId = "OPENID_GROUP_1"; ph.endpointId = "OPENID_GROUP_1"; ph.key = "left"; ph.value = "1"; st->insert(ph);
    GroupSettingRow ph2; ph2.platform = "qq_official"; ph2.groupId = "OPENID_GROUP_1";
    ph2.key = "left"; ph2.value = "1"; st->insert(ph2);

    // 消息路径留下的真实行：groupId 是公共群号。
    GroupAccountSettingRow real; real.adapterId = "adapterA"; real.platform = "qq_official";
    real.groupId = "20001"; real.endpointId = "OPENID_GROUP_1"; real.key = "left"; real.value = "0"; st->insert(real);

    identity::BindingStore::cleanupOfficialEndpointGroupSettings(*db);

    const auto accountRows = st->get_all<GroupAccountSettingRow>();
    ASSERT_EQ((int)accountRows.size(), 1);
    ASSERT_EQ(accountRows.front().groupId, std::string("20001"));
    ASSERT_EQ((int)st->get_all<GroupSettingRow>().size(), 0);
}
