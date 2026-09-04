// ─── C#28-B: PersonaManager Unit Tests ────────────────────────
// Tests PersonaManager CRUD, per-group switching, I18n injection,
// import/export, built-in protection.

#include "test_framework.h"
#include "../src/core/persona/persona_manager.h"
#include "../src/i18n/i18n.h"
#include "../src/storage/database.h"
#include "../src/config/config_manager.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <utility>

using namespace dice;
namespace fs = std::filesystem;

namespace {
class PersonaTestConfig final : public ConfigManager {
public:
    PersonaTestConfig() : PersonaTestConfig(makeRoot()) {}

    ~PersonaTestConfig() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

private:
    explicit PersonaTestConfig(fs::path root)
        : ConfigManager((root / "config").string()), root_(std::move(root)) {}

    static fs::path makeRoot() {
        static uint64_t sequence = 0;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path root = fs::temp_directory_path() /
            ("dice_next_persona_" + std::to_string(nonce) + "_" +
             std::to_string(++sequence));
        std::error_code ec;
        fs::remove_all(root, ec);
        return root;
    }

    fs::path root_;
};
}

static std::unique_ptr<Database> makeDb() {
    auto db = std::make_unique<Database>();
    db->open(":memory:");
    return db;
}

static std::unique_ptr<PersonaTestConfig> makeCfg() {
    auto cfg = std::make_unique<PersonaTestConfig>();
    cfg->load();
    return cfg;
}

// ─── Template CRUD Tests ──────────────────────────────────────

TEST(PersonaCRUD, CreateTemplate) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int id = mgr.createTemplate("TestPersona", "A test persona");
    ASSERT_TRUE(id > 0);

    auto tmpl = mgr.getTemplateById(id);
    ASSERT_EQ(tmpl.name, "TestPersona");
    ASSERT_EQ(tmpl.description, "A test persona");
    ASSERT_FALSE(tmpl.isBuiltin);
}

TEST(PersonaCRUD, CreateDuplicateNameFails) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int id1 = mgr.createTemplate("Dup", "first");
    ASSERT_TRUE(id1 > 0);

    int id2 = mgr.createTemplate("Dup", "second");
    ASSERT_EQ(id2, -1);
}

TEST(PersonaCRUD, CreateEmptyNameFails) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int id = mgr.createTemplate("", "empty name");
    ASSERT_EQ(id, -1);
}

TEST(PersonaCRUD, GetTemplateByName) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    mgr.createTemplate("ByName", "test");

    auto tmpl = mgr.getTemplateByName("ByName");
    ASSERT_TRUE(tmpl.id > 0);
    ASSERT_EQ(tmpl.name, "ByName");
}

TEST(PersonaCRUD, GetTemplateByNameNotFound) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    auto tmpl = mgr.getTemplateByName("NonExistent");
    ASSERT_EQ(tmpl.id, 0);
    ASSERT_TRUE(tmpl.name.empty());
}

TEST(PersonaCRUD, ListTemplates) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    mgr.createTemplate("Persona1", "desc1");
    mgr.createTemplate("Persona2", "desc2");
    mgr.createTemplate("Persona3", "desc3");

    auto list = mgr.listTemplates();
    ASSERT_EQ((int)list.size(), 3);
}

TEST(PersonaCRUD, UpdateTemplateMeta) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int id = mgr.createTemplate("Original", "original desc");

    bool success = mgr.updateTemplateMeta(id, "Updated", "updated desc");
    ASSERT_TRUE(success);

    auto tmpl = mgr.getTemplateById(id);
    ASSERT_EQ(tmpl.name, "Updated");
    ASSERT_EQ(tmpl.description, "updated desc");
}

TEST(PersonaCRUD, UpdateTemplateMetaDuplicateNameFails) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    mgr.createTemplate("Name1", "");
    int id2 = mgr.createTemplate("Name2", "");

    bool success = mgr.updateTemplateMeta(id2, "Name1", "");
    ASSERT_FALSE(success);
}

TEST(PersonaCRUD, DeleteTemplate) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int id = mgr.createTemplate("ToDelete", "will be deleted");

    bool success = mgr.deleteTemplate(id);
    ASSERT_TRUE(success);

    auto tmpl = mgr.getTemplateById(id);
    ASSERT_EQ(tmpl.id, 0);
}

// ─── Entry CRUD Tests ─────────────────────────────────────────

TEST(PersonaEntry, SetAndGetEntry) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("TestEntries", "");

    bool success = mgr.setEntry(pid, "zh-Hans", "dice.roll", "Roll: {result}");
    ASSERT_TRUE(success);

    auto entries = mgr.listEntries(pid);
    ASSERT_EQ((int)entries.size(), 1);
    ASSERT_EQ(entries[0].locale, "zh-Hans");
    ASSERT_EQ(entries[0].key, "dice.roll");
    ASSERT_EQ(entries[0].value, "Roll: {result}");
}

TEST(PersonaEntry, SetEntryUpsert) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("UpsertTest", "");

    mgr.setEntry(pid, "zh-Hans", "key1", "value1");
    mgr.setEntry(pid, "zh-Hans", "key1", "value2");  // upsert

    auto entries = mgr.listEntries(pid);
    ASSERT_EQ((int)entries.size(), 1);
    ASSERT_EQ(entries[0].value, "value2");
}

TEST(PersonaEntry, DeleteEntry) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("DeleteEntry", "");
    mgr.setEntry(pid, "zh-Hans", "key1", "value1");

    bool success = mgr.deleteEntry(pid, "zh-Hans", "key1");
    ASSERT_TRUE(success);

    auto entries = mgr.listEntries(pid);
    ASSERT_EQ((int)entries.size(), 0);
}

TEST(PersonaEntry, GetEntryCount) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("CountTest", "");
    mgr.setEntry(pid, "zh-Hans", "key1", "v1");
    mgr.setEntry(pid, "zh-Hans", "key2", "v2");
    mgr.setEntry(pid, "en", "key1", "v1_en");

    ASSERT_EQ(mgr.getEntryCount(pid), 3);
}

TEST(PersonaEntry, GetEntryKeys) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("KeysTest", "");
    mgr.setEntry(pid, "zh-Hans", "keyB", "vB");
    mgr.setEntry(pid, "zh-Hans", "keyA", "vA");
    mgr.setEntry(pid, "en", "keyC", "vC");

    auto keys = mgr.getEntryKeys(pid);
    ASSERT_EQ((int)keys.size(), 3);
    // listEntries orders by key, so getEntryKeys should be sorted
    ASSERT_EQ(keys[0], "keyA");
    ASSERT_EQ(keys[1], "keyB");
    ASSERT_EQ(keys[2], "keyC");
}

// ─── Copy Template Tests ──────────────────────────────────────

TEST(PersonaCopy, CopyTemplateWithEntries) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int srcId = mgr.createTemplate("Source", "source persona");
    mgr.setEntry(srcId, "zh-Hans", "key1", "value1");
    mgr.setEntry(srcId, "zh-Hans", "key2", "value2");
    mgr.setEntry(srcId, "en", "key3", "value3_en");

    int newId = mgr.copyTemplate(srcId, "Copied");
    ASSERT_TRUE(newId > 0);

    // Verify template
    auto tmpl = mgr.getTemplateById(newId);
    ASSERT_EQ(tmpl.name, "Copied");
    ASSERT_EQ(tmpl.description, "source persona");

    // Verify entries were copied
    ASSERT_EQ(mgr.getEntryCount(newId), 3);

    // Original should be unchanged
    ASSERT_EQ(mgr.getEntryCount(srcId), 3);
}

TEST(PersonaCopy, CopyNonexistentSourceFails) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int newId = mgr.copyTemplate(9999, "Copy");
    ASSERT_EQ(newId, -1);
}

TEST(PersonaCopy, CopyDuplicateNameFails) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int srcId = mgr.createTemplate("Original", "");
    mgr.createTemplate("AlreadyExists", "");

    int newId = mgr.copyTemplate(srcId, "AlreadyExists");
    ASSERT_EQ(newId, -1);
}

// ─── Active Persona Tests ─────────────────────────────────────

TEST(PersonaActive, GetActivePersonaDefaultZero) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    // No persona configured → should return 0
    ASSERT_EQ(mgr.getActivePersona(""), 0);
    ASSERT_EQ(mgr.getActivePersona("group1"), 0);
}

TEST(PersonaActive, SetAndGetGlobalPersona) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("Global", "");
    mgr.setActivePersona(pid, "");  // global

    ASSERT_EQ(mgr.getActivePersona(""), pid);
    // Per-group with no group-specific setting should fall back to global
    ASSERT_EQ(mgr.getActivePersona("group1"), pid);
}

TEST(PersonaActive, SetAndGetGroupPersona) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("GroupPersona", "");
    mgr.setActivePersona(pid, "group1");

    // Group1 should return the group-specific persona
    ASSERT_EQ(mgr.getActivePersona("group1"), pid);
    // Different group should return 0 (no global set)
    ASSERT_EQ(mgr.getActivePersona("group2"), 0);
    // Empty group should return 0
    ASSERT_EQ(mgr.getActivePersona(""), 0);
}

TEST(PersonaActive, GroupPersonaOverridesGlobal) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int globalPid = mgr.createTemplate("Global", "");
    int groupPid = mgr.createTemplate("GroupSpecific", "");

    mgr.setActivePersona(globalPid, "");  // set global
    mgr.setActivePersona(groupPid, "group1");  // set group1

    // Group1 should return group-specific (not global)
    ASSERT_EQ(mgr.getActivePersona("group1"), groupPid);
    // Other group should fall back to global
    ASSERT_EQ(mgr.getActivePersona("group2"), globalPid);
    // Empty group should return global
    ASSERT_EQ(mgr.getActivePersona(""), globalPid);
}

TEST(PersonaActive, SameGroupIdIsIsolatedByPlatform) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int qqPid = mgr.createTemplate("QQ", "");
    int discordPid = mgr.createTemplate("Discord", "");
    mgr.setActivePersona(qqPid, "same-id", "onebot_v11");
    mgr.setActivePersona(discordPid, "same-id", "discord");

    ASSERT_EQ(mgr.getActivePersona("same-id", "onebot_v11"), qqPid);
    ASSERT_EQ(mgr.getActivePersona("same-id", "discord"), discordPid);
}

TEST(PersonaActive, LegacyOneBotPlatformRowsRemainReadable) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("Legacy", "");
    mgr.setActivePersona(pid, "legacy-group", "onebot_v11");

    ASSERT_EQ(mgr.getActivePersona("legacy-group", "milky"), pid);
}

TEST(PersonaActive, SetGlobalPersonaToZero) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("Test", "");
    mgr.setActivePersona(pid, "");
    ASSERT_EQ(mgr.getActivePersona(""), pid);

    mgr.setActivePersona(0, "");  // turn off
    ASSERT_EQ(mgr.getActivePersona(""), 0);
}

// ─── Import/Export Tests ──────────────────────────────────────

TEST(PersonaImportExport, ExportTemplate) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("Exportable", "export desc");
    mgr.setEntry(pid, "zh-Hans", "key1", "value1");
    mgr.setEntry(pid, "en", "key2", "value2_en");

    json j = mgr.exportTemplate(pid);
    ASSERT_EQ(j["name"], "Exportable");
    ASSERT_EQ(j["description"], "export desc");
    ASSERT_EQ(j["entries"].size(), 2);
}

TEST(PersonaImportExport, ImportTemplate) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    json importData = {
        {"name", "Imported"},
        {"description", "imported persona"},
        {"entries", json::array({
            {{"locale", "zh-Hans"}, {"key", "key1"}, {"value", "val1"}},
            {{"locale", "en"}, {"key", "key2"}, {"value", "val2"}}
        })}
    };

    int newId = mgr.importTemplate(importData);
    ASSERT_TRUE(newId > 0);

    auto tmpl = mgr.getTemplateById(newId);
    ASSERT_EQ(tmpl.name, "Imported");
    ASSERT_EQ(tmpl.description, "imported persona");

    ASSERT_EQ(mgr.getEntryCount(newId), 2);
}

TEST(PersonaImportExport, ImportInvalidJsonFails) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    // Not an object
    json bad = json::array({1, 2, 3});
    ASSERT_EQ(mgr.importTemplate(bad), -1);

    // Empty name
    json bad2 = {{"name", ""}};
    ASSERT_EQ(mgr.importTemplate(bad2), -1);
}

TEST(PersonaImportExport, ExportImportRoundTrip) {
    auto db1 = makeDb();
    auto cfg1 = makeCfg();
    I18n i18n1("i18n", Locale::kZhHans);
    PersonaManager mgr1(*db1, i18n1, *cfg1);

    int pid = mgr1.createTemplate("RoundTrip", "round trip test");
    mgr1.setEntry(pid, "zh-Hans", "greeting", "Hello!");
    mgr1.setEntry(pid, "en", "greeting", "Hi!");

    json exported = mgr1.exportTemplate(pid);

    // Import into a fresh manager
    auto db2 = makeDb();
    auto cfg2 = makeCfg();
    I18n i18n2("i18n", Locale::kZhHans);
    PersonaManager mgr2(*db2, i18n2, *cfg2);

    int newId = mgr2.importTemplate(exported);
    ASSERT_TRUE(newId > 0);

    auto tmpl = mgr2.getTemplateById(newId);
    ASSERT_EQ(tmpl.name, "RoundTrip");
    ASSERT_EQ(mgr2.getEntryCount(newId), 2);

    // Verify entry values
    auto entries = mgr2.listEntries(newId);
    bool foundZh = false, foundEn = false;
    for (auto& e : entries) {
        if (e.locale == "zh-Hans" && e.key == "greeting" && e.value == "Hello!") foundZh = true;
        if (e.locale == "en" && e.key == "greeting" && e.value == "Hi!") foundEn = true;
    }
    ASSERT_TRUE(foundZh);
    ASSERT_TRUE(foundEn);
}

// ─── Delete Template with Entries ─────────────────────────────

TEST(PersonaDelete, DeleteRemovesEntries) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("WithEntries", "");
    mgr.setEntry(pid, "zh-Hans", "key1", "v1");
    mgr.setEntry(pid, "zh-Hans", "key2", "v2");

    ASSERT_EQ(mgr.getEntryCount(pid), 2);

    mgr.deleteTemplate(pid);

    // Entries should be gone
    ASSERT_EQ(mgr.getEntryCount(pid), 0);
}

// ─── LoadIntoI18n Tests ───────────────────────────────────────

TEST(PersonaI18n, LoadIntoI18nClearsAndInjects) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("I18nTest", "");
    mgr.setEntry(pid, "zh-Hans", "dice.greeting", "Persona greeting!");
    mgr.setEntry(pid, "en", "dice.greeting", "EN persona greeting!");

    mgr.loadIntoI18n(pid);

    // Loading a persona must not mutate the global selection.
    ASSERT_EQ(i18n.getActivePersonaId(), 0);  // setPersona not called yet
    auto scope = i18n.scopedPersona(pid);
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "dice.greeting"), "Persona greeting!");
    ASSERT_EQ(i18n.tr(Locale::kEn, "dice.greeting"), "EN persona greeting!");
}

TEST(PersonaI18n, ReloadEmptyPersonaClearsOnlyItsCache) {
    auto db = makeDb();
    auto cfg = makeCfg();
    I18n i18n("i18n", Locale::kZhHans);
    PersonaManager mgr(*db, i18n, *cfg);

    int pid = mgr.createTemplate("Clear", "");
    int otherPid = mgr.createTemplate("Other", "");
    mgr.setEntry(pid, "zh-Hans", "test.key", "test value");
    mgr.setEntry(otherPid, "zh-Hans", "test.key", "other value");

    mgr.loadIntoI18n(pid);
    mgr.loadIntoI18n(otherPid);
    {
        auto scope = i18n.scopedPersona(pid);
        ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "test value");
    }

    ASSERT_TRUE(mgr.deleteEntry(pid, "zh-Hans", "test.key"));
    mgr.loadIntoI18n(pid);

    {
        auto scope = i18n.scopedPersona(pid);
        ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "test.key");
    }
    {
        auto scope = i18n.scopedPersona(otherPid);
        ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "other value");
    }
}
