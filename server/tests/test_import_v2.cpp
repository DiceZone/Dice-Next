// ─── C#30: Legacy Import V2 Unit Tests ────────────────────────
// Tests ImportResult structure, ImportOptions (skip/overwrite),
// deck JSON validation, and import logic.

#include "test_framework.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <map>
#include <set>
#include <tuple>
#include <vector>
#include <cstdint>
#include <sqlite_orm/sqlite_orm.h>
#include "../src/core/deck/card_deck.h"

// Provide a complete LuaPluginManager definition so that legacy_import_v2.h's
// runImport() body compiles. CardDeck uses the real project implementation,
// which is already linked into the test target.
namespace dice {
class LuaPluginManager {
public:
    void reload() {}
    void confSet(const std::string& scope, const std::string& key, const std::string& value) { conf_[scope][key] = value; }
    bool confSetBatch(const std::vector<std::tuple<std::string, std::string, std::string>>& values) {
        for (const auto& [scope, key, value] : values) {
            if (value.empty()) conf_[scope].erase(key);
            else conf_[scope][key] = value;
        }
        return true;
    }
    std::string confGet(const std::string& scope, const std::string& key) const {
        auto it = conf_.find(scope); if (it == conf_.end()) return {};
        auto kv = it->second.find(key); return kv == it->second.end() ? std::string() : kv->second;
    }
private:
    std::map<std::string, std::map<std::string, std::string>> conf_;
};
}

// Include database.h which defines the row structs
#include "../src/storage/database.h"
#include "../src/service/backup_service.h"

// Now include the full import header — CardDeck/LuaPluginManager are complete
#include "../src/storage/legacy_import_v2.h"

using namespace dice;
using namespace dice::legacyv2;
namespace fs = std::filesystem;

// ─── ImportResult Structure Tests ─────────────────────────────

TEST(ImportResult, DefaultValues) {
    ImportResult result;
    ASSERT_EQ(result.success, 0);
    ASSERT_EQ(result.skipped, 0);
    ASSERT_EQ(result.failed, 0);
    ASSERT_TRUE(result.details.empty());
}

TEST(ImportResult, ToJSON) {
    ImportResult result;
    result.success = 3;
    result.skipped = 2;
    result.failed = 1;
    result.details.push_back({"deck1.json", "success", ""});
    result.details.push_back({"deck2.json", "skipped", "already exists"});
    result.details.push_back({"deck3.json", "failed", "invalid JSON"});

    json j = result.toJSON();
    ASSERT_EQ(j["success"], 3);
    ASSERT_EQ(j["skipped"], 2);
    ASSERT_EQ(j["failed"], 1);
    ASSERT_EQ(j["details"].size(), 3);
    ASSERT_EQ(j["details"][0]["name"], "deck1.json");
    ASSERT_EQ(j["details"][0]["status"], "success");
    ASSERT_EQ(j["details"][1]["name"], "deck2.json");
    ASSERT_EQ(j["details"][1]["status"], "skipped");
    ASSERT_EQ(j["details"][1]["reason"], "already exists");
    ASSERT_EQ(j["details"][2]["name"], "deck3.json");
    ASSERT_EQ(j["details"][2]["status"], "failed");
    ASSERT_EQ(j["details"][2]["reason"], "invalid JSON");
}

TEST(ImportResult, EmptyToJSON) {
    ImportResult result;
    json j = result.toJSON();
    ASSERT_EQ(j["success"], 0);
    ASSERT_EQ(j["skipped"], 0);
    ASSERT_EQ(j["failed"], 0);
    ASSERT_EQ(j["details"].size(), 0);
}

// ─── ImportOptions Tests ──────────────────────────────────────

TEST(ImportOptions, DefaultOverwriteFalse) {
    ImportOptions opts;
    ASSERT_FALSE(opts.overwrite);
}

TEST(ImportOptions, OverwriteTrue) {
    ImportOptions opts;
    opts.overwrite = true;
    ASSERT_TRUE(opts.overwrite);
}

// ─── validateDeckJson Tests ───────────────────────────────────

TEST(ValidateDeck, ValidDeckObject) {
    std::string content = R"({"deck1": ["card1", "card2"], "deck2": ["a", "b", "c"]})";
    std::string error = validateDeckJson(content);
    ASSERT_TRUE(error.empty());
}

TEST(ValidateDeck, ValidEmptyObject) {
    std::string content = R"({})";
    std::string error = validateDeckJson(content);
    ASSERT_TRUE(error.empty());
}

TEST(ValidateDeck, InvalidNotObject) {
    std::string content = R"(["array", "not", "object"])";
    std::string error = validateDeckJson(content);
    ASSERT_FALSE(error.empty());
    ASSERT_EQ(error, "not a JSON object");
}

TEST(ValidateDeck, InvalidValueNotArray) {
    std::string content = R"({"deck1": "not an array"})";
    std::string error = validateDeckJson(content);
    ASSERT_FALSE(error.empty());
    // Should mention the key name
    EXPECT_TRUE(error.find("deck1") != std::string::npos);
}

TEST(ValidateDeck, InvalidJSON) {
    std::string content = "not json at all {{{";
    std::string error = validateDeckJson(content);
    ASSERT_FALSE(error.empty());
}

TEST(ValidateDeck, EmptyContent) {
    std::string content = "";
    std::string error = validateDeckJson(content);
    ASSERT_FALSE(error.empty());
}

TEST(ValidateDeck, MultipleKeysAllValid) {
    std::string content = R"({
        "deck1": ["a", "b"],
        "deck2": ["c"],
        "deck3": [],
        "deck4": ["x", "y", "z"]
    })";
    std::string error = validateDeckJson(content);
    ASSERT_TRUE(error.empty());
}

TEST(ValidateDeck, MixedValidInvalidValues) {
    std::string content = R"({"deck1": ["a"], "deck2": 42})";
    std::string error = validateDeckJson(content);
    ASSERT_FALSE(error.empty());
    EXPECT_TRUE(error.find("deck2") != std::string::npos);
}

// ─── utf8Truncate Tests ───────────────────────────────────────

TEST(Utf8Truncate, NoTruncationNeeded) {
    std::string s = "hello world";
    std::string result = utf8Truncate(s, 100);
    ASSERT_EQ(result, "hello world");
}

TEST(Utf8Truncate, TruncateAscii) {
    std::string s = "hello world";
    std::string result = utf8Truncate(s, 5);
    ASSERT_EQ(result, "hello");
}

TEST(Utf8Truncate, TruncateExactLength) {
    std::string s = "hello";
    std::string result = utf8Truncate(s, 5);
    ASSERT_EQ(result, "hello");
}

TEST(Utf8Truncate, TruncateMultiByteUTF8) {
    // "你好" = 6 bytes (3 bytes per char)
    std::string s = "\xe4\xbd\xa0\xe5\xa5\xbd";  // 你好
    std::string result = utf8Truncate(s, 4);
    // Should not split a multi-byte char — should truncate to 3 bytes (1 char)
    ASSERT_EQ(result, "\xe4\xbd\xa0");  // 你
}

TEST(Utf8Truncate, TruncateAtCharBoundary) {
    // "你好世界" = 12 bytes
    std::string s = "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c";
    std::string result = utf8Truncate(s, 6);
    ASSERT_EQ(result, "\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好
}

// ─── sanitizeJsonControls Tests ───────────────────────────────

TEST(SanitizeJson, NoControlsInStrings) {
    std::string input = R"({"key": "value"})";
    std::string output = sanitizeJsonControls(input);
    ASSERT_EQ(output, input);
}

TEST(SanitizeJson, EscapesNewlinesInStrings) {
    std::string input = "{\"key\": \"line1\nline2\"}";
    std::string output = sanitizeJsonControls(input);
    ASSERT_EQ(output, "{\"key\": \"line1\\nline2\"}");
}

TEST(SanitizeJson, EscapesTabsInStrings) {
    std::string input = "{\"key\": \"col1\tcol2\"}";
    std::string output = sanitizeJsonControls(input);
    ASSERT_EQ(output, "{\"key\": \"col1\\tcol2\"}");
}

TEST(SanitizeJson, EscapesCarriageReturnsInStrings) {
    std::string input = "{\"key\": \"line1\rline2\"}";
    std::string output = sanitizeJsonControls(input);
    ASSERT_EQ(output, "{\"key\": \"line1\\rline2\"}");
}

TEST(SanitizeJson, DoesNotEscapeOutsideStrings) {
    // Raw newline outside strings should be preserved (JSON allows whitespace)
    std::string input = "{\n\"key\": \"value\"\n}";
    std::string output = sanitizeJsonControls(input);
    // The newlines outside strings should remain
    EXPECT_TRUE(output.find("\n") != std::string::npos);
}

TEST(SanitizeJson, HandlesEscapedQuotes) {
    std::string input = "{\"key\": \"has \\\"quotes\\\" inside\"}";
    std::string output = sanitizeJsonControls(input);
    // Should not break the escaping
    EXPECT_TRUE(output.find("\\\"quotes\\\"") != std::string::npos);
}

static fs::path makeTempDir(const std::string& name);
static void cleanupTempDir(const fs::path& p);

TEST(ImportCensorWords, ImportsLegacyLevelsAndEnablesFilter) {
    fs::path root = makeTempDir("censor_words");
    fs::create_directories(root / "conf");
    {
        std::ofstream out(root / "conf" / "CustomCensor.json", std::ios::binary);
        out << R"({"alpha":1,"测试词":4})";
    }
    dice::ConfigManager cfg((root / "config").string());
    ASSERT_TRUE(cfg.load());
    ASSERT_EQ(importCensorWords(cfg, root / "conf"), 2);
    auto current = cfg.get<json>("dice/censor", json::object());
    ASSERT_TRUE(current.value("enabled", false));
    ASSERT_EQ(current["words"]["alpha"].get<int>(), 1);
    ASSERT_EQ(current["words"]["测试词"].get<int>(), 4);

    // Re-importing is idempotent and preserves WebUI edits.
    current["words"]["alpha"] = 3;
    cfg.set("dice/censor", current);
    ASSERT_TRUE(cfg.save());
    ASSERT_EQ(importCensorWords(cfg, root / "conf"), 0);
    ASSERT_EQ(cfg.get<json>("dice/censor", json::object())["words"]["alpha"].get<int>(), 3);
    cleanupTempDir(root);
}

// ─── importDecks Integration Tests (Filesystem) ───────────────
// These tests create temporary directories and files.

static fs::path makeTempDir(const std::string& name) {
    fs::path tmp = fs::temp_directory_path() / ("dice_test_" + name);
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

static void cleanupTempDir(const fs::path& p) {
    // Change to a safe directory before removing
    fs::current_path(fs::temp_directory_path());
    fs::remove_all(p);
}

TEST(ImportReplies, LegacyDeckAnswerRetainsAndUsesNumericWeight) {
    const auto root = makeTempDir("reply_weights");
    {
        Database db;
        ASSERT_TRUE(db.open((root / "test.db").string()));
        ConfigManager cfg((root / "config").string());
        cfg.resetDefault();
        ReplyManager replies(db, cfg);
        std::ofstream(root / "CustomMsgReply.json") <<
            R"({"旧版回复":{"match":["hello"],"echo":"Deck","answer":["::5::你好"]}})";
        ASSERT_EQ(importReplies(replies, root), 1);
        const auto matched = replies.matchMessage("hello");
        ASSERT_EQ(matched.size(), size_t(1));
        ASSERT_EQ(matched.front().results.front(), "你好");
        ASSERT_EQ(matched.front().resultWeights.front(), 5);
        ASSERT_EQ(replies.pickResult(matched.front()), "你好");
    }
    cleanupTempDir(root);
}

TEST(ReplyReferenceMigration, ConvertsUniqueReferencesWithoutEvaluatingOrTouchingVariables) {
    using namespace legacy_reply_references;
    Catalog catalog{{"牌堆"}, {"词条"}, {"strHello"}, {}};
    Report report; bool compatibility = false;
    ASSERT_EQ(migrate("{牌堆}/{%牌堆}/{词条}/{strHello}/{nick}/{$1}/{draw:牌堆}", catalog, report, "r", "results[0]", compatibility),
        "{deck:牌堆}/{deck:%牌堆}/{help:词条}/{text:strHello}/{nick}/{$1}/{draw:牌堆}");
    ASSERT_EQ(report.converted, 4);
    ASSERT_FALSE(compatibility);
    ASSERT_EQ(report.details.front()["target"].get<std::string>(), "{deck:牌堆}");
    ASSERT_EQ(migrate("{%_1D3}/{1++}", catalog, report, "r", "results[1]", compatibility), "{%_1D3}/{1++}");
    ASSERT_EQ(report.ambiguous, 2); ASSERT_TRUE(compatibility);
}

TEST(ReplyReferenceMigration, PreservesAmbiguityEscapesAndMigratesEveryNestedBranch) {
    using namespace legacy_reply_references;
    Catalog catalog{{"同名", "user", "1d3", "牌堆"}, {"同名"}, {}, {}};
    Report report; bool compatibility = false;
    const std::string text = "{同名}/{user}/{1d3}/{未知}/\\{牌堆}/{sample:{牌堆}|{sample:{%牌堆}|字}}";
    ASSERT_EQ(migrate(text, catalog, report, "r", "results[1]", compatibility),
        "{同名}/{user}/{1d3}/{未知}/\\{牌堆}/{sample:{deck:牌堆}|{sample:{deck:%牌堆}|字}}");
    ASSERT_EQ(report.ambiguous, 3);
    ASSERT_EQ(report.unresolved, 1);
    ASSERT_EQ(report.converted, 2);
    ASSERT_TRUE(compatibility);
    Report escaped; bool flag = false;
    ASSERT_EQ(migrate("\\{sample:{牌堆}|{%牌堆}}/{a|b}/{}", catalog, escaped, "r", "f", flag),
        "\\{sample:{牌堆}|{%牌堆}}/{a|b}/{}");
    ASSERT_EQ(escaped.converted, 0);
}

TEST(ImportReplies, MigrationIsIdempotentAndUpgradesExactOldDefinitionsWithoutResettingOwnerSettings) {
    const auto root = makeTempDir("reply_references");
    {
        Database db; ASSERT_TRUE(db.open((root / "test.db").string()));
        ConfigManager cfg((root / "config").string()); cfg.resetDefault();
        ReplyManager replies(db, cfg);
        std::ofstream(root / "CustomMsgReply.json") << R"({"旧规则":{"match":["hello"],"answer":["::3::{牌堆}","{歧义}"],"limit":"cd:15&@echo={词条};today:2&@echo={牌堆}"}})";
        ASSERT_EQ(importReplies(replies, root), 1);
        auto old = replies.matchMessage("hello").front();
        const auto id = old.id;
        old.enabled = false; old.priority = 777; ASSERT_TRUE(replies.updateRule(id, old));
        legacy_reply_references::Catalog catalog{{"牌堆", "歧义"}, {"词条", "歧义"}, {}, {}};
        legacy_reply_references::Report report;
        ASSERT_EQ(importReplies(replies, root, catalog, &report), 0);
        auto snapshot = replies.listRules(); ASSERT_EQ(snapshot->size(), size_t(1));
        auto rule = snapshot->front();
        ASSERT_EQ(rule.id, id); ASSERT_FALSE(rule.enabled); ASSERT_EQ(rule.priority, 777);
        ASSERT_EQ(rule.results[0], "{deck:牌堆}"); ASSERT_EQ(rule.results[1], "{歧义}");
        ASSERT_EQ(rule.resultWeights[0], 3); ASSERT_TRUE(rule.legacyReferences);
        ASSERT_EQ(rule.cooldownNotice, "{help:词条}"); ASSERT_EQ(rule.dayLimitNotice, "{deck:牌堆}");
        ASSERT_EQ(report.converted, 3); ASSERT_EQ(report.ambiguous, 1);
        replies.loadRules(); ASSERT_TRUE(replies.listRules()->front().legacyReferences);
        ASSERT_EQ(importReplies(replies, root, catalog), 0);
        ASSERT_EQ(replies.listRules()->size(), size_t(1));
        catalog.help.erase("歧义");
        ASSERT_EQ(importReplies(replies, root, catalog), 0);
        ASSERT_EQ(replies.listRules()->size(), size_t(1));
        const auto resolved = replies.listRules()->front();
        ASSERT_EQ(resolved.id, id); ASSERT_FALSE(resolved.enabled); ASSERT_EQ(resolved.priority, 777);
        ASSERT_EQ(resolved.results[1], "{deck:歧义}"); ASSERT_FALSE(resolved.legacyReferences);
    }
    cleanupTempDir(root);
}

TEST(ReplyReferenceMigration, OrchestratorUsesActualDestinationAndImportsHelpAndTextsBeforeReplies) {
    const auto root = makeTempDir("reply_reference_orchestrator");
    const auto legacy = root / "DiceData";
    const auto runtime = root / "runtime";
    fs::create_directories(legacy / "conf"); fs::create_directories(legacy / "PublicDeck");
    fs::create_directories(runtime / "data/decks");
    std::ofstream(legacy / "PublicDeck/same.json") << R"({"源新增":["新"],"冲突":["新"]})";
    std::ofstream(runtime / "data/decks/same.json") << R"({"旧目标":["保留"],"冲突":["保留"]})";
    std::ofstream(legacy / "conf/CustomHelp.json") << R"({"帮助":"说明"})";
    std::ofstream(legacy / "conf/CustomMsg.json") << R"({"strDemo":"文案","strRollCriticalSuccess":"优秀"})";
    std::ofstream(legacy / "conf/CustomMsgReply.json") << R"({"规则":{"match":["hello"],"answer":"{旧目标}/{源新增}/{冲突}/{帮助}/{strDemo}/{strRollCriticalSuccess}"}})";
    {
        struct RestoreDirectory { fs::path path = fs::current_path(); ~RestoreDirectory() { fs::current_path(path); } } restore;
        fs::current_path(runtime);
        Database db; ASSERT_TRUE(db.open((runtime / "test.db").string()));
        ConfigManager cfg((runtime / "config").string()); cfg.resetDefault();
        I18n i18n((fs::path(__FILE__).parent_path().parent_path() / "i18n").string());
        ReplyManager replies(db, cfg);
        const auto report = runImport(db, cfg, i18n, replies, legacy.string());
        ASSERT_TRUE(report["ok"].get<bool>());
        ASSERT_EQ(report["decks"]["skipped"].get<int>(), 1);
        ASSERT_EQ(report["replyReferences"]["converted"].get<int>(), 5);
        ASSERT_EQ(report["replyReferences"]["unresolved"].get<int>(), 1);
        const auto rule = replies.matchMessage("hello").front();
        ASSERT_EQ(rule.results.front(), "{deck:旧目标}/{源新增}/{deck:冲突}/{help:帮助}/{text:strDemo}/{text:strRollCriticalSuccess}");
        ASSERT_TRUE(rule.legacyReferences);
        ASSERT_EQ(runImport(db, cfg, i18n, replies, legacy.string())["replies"].get<int>(), 0);
        ASSERT_EQ(replies.listRules()->size(), size_t(1));
    }
    cleanupTempDir(root);
}

// Small writers for authentic Dice! V2 binary containers.  Keeping these in
// the test suite makes the parser test independent from a developer's private
// DiceData directory.
template <typename T>
static void writeRaw(std::ofstream& out, T value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
static void writeLegacyStr(std::ofstream& out, const std::string& value) {
    writeRaw<int16_t>(out, static_cast<int16_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}
static void writeLegacyInt(std::ofstream& out, int value) {
    out.put(2); writeRaw<int32_t>(out, value);
}
static void writeLegacyUtf8(std::ofstream& out, const std::string& value) {
    out.put(20); writeLegacyStr(out, value);
}

TEST(ImportDecks, ValidDeckImported) {
    fs::path root = makeTempDir("decks_valid");

    // Create PublicDeck directory with a valid deck file
    fs::create_directories(root / "PublicDeck");
    std::string deckContent = R"({"test_deck": ["card1", "card2", "card3"]})";
    {
        std::ofstream f(root / "PublicDeck" / "test.json");
        f << deckContent;
    }

    // Change to temp dir for data/decks relative path
    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 1);
    ASSERT_EQ(result.skipped, 0);
    ASSERT_EQ(result.failed, 0);
    ASSERT_EQ((int)result.details.size(), 1);
    ASSERT_EQ(result.details[0].name, "test.json");
    ASSERT_EQ(result.details[0].status, "success");

    cleanupTempDir(root);
}

TEST(ImportDecks, NonJsonFileSkipped) {
    fs::path root = makeTempDir("decks_nonjson");

    fs::create_directories(root / "PublicDeck");
    {
        std::ofstream f(root / "PublicDeck" / "readme.txt");
        f << "This is not a deck file";
    }
    {
        std::ofstream f(root / "PublicDeck" / "valid.json");
        f << R"({"deck": ["a"]})";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 1);
    ASSERT_EQ(result.skipped, 1);
    ASSERT_EQ(result.failed, 0);

    // Find the txt file in details — should be skipped
    bool foundTxt = false;
    for (auto& d : result.details) {
        if (d.name == "readme.txt") {
            ASSERT_EQ(d.status, "skipped");
            foundTxt = true;
        }
    }
    ASSERT_TRUE(foundTxt);

    cleanupTempDir(root);
}

TEST(ImportDecks, InvalidJsonFails) {
    fs::path root = makeTempDir("decks_invalid");

    fs::create_directories(root / "PublicDeck");
    {
        std::ofstream f(root / "PublicDeck" / "broken.json");
        f << "this is not valid json {{{";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 0);
    ASSERT_EQ(result.failed, 1);
    ASSERT_EQ(result.details[0].status, "failed");
    EXPECT_FALSE(result.details[0].reason.empty());

    cleanupTempDir(root);
}

TEST(ImportDecks, NonObjectJsonFails) {
    fs::path root = makeTempDir("decks_nonobj");

    fs::create_directories(root / "PublicDeck");
    {
        std::ofstream f(root / "PublicDeck" / "array.json");
        f << R"(["not", "an", "object"])";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 0);
    ASSERT_EQ(result.failed, 1);

    cleanupTempDir(root);
}

TEST(ImportDecks, SkipExistingWhenOverwriteFalse) {
    fs::path root = makeTempDir("decks_skip");

    fs::create_directories(root / "PublicDeck");
    {
        std::ofstream f(root / "PublicDeck" / "existing.json");
        f << R"({"deck": ["a", "b"]})";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    // Create data/decks with existing file
    fs::create_directories("data/decks");
    {
        std::ofstream f("data/decks/existing.json");
        f << R"({"old": ["old"]})";
    }

    ImportOptions opts;  // overwrite = false (default)
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 0);
    ASSERT_EQ(result.skipped, 1);
    ASSERT_EQ(result.details[0].status, "skipped");
    EXPECT_TRUE(result.details[0].reason.find("overwrite") != std::string::npos);

    cleanupTempDir(root);
}

TEST(ImportDecks, OverwriteExistingWhenOverwriteTrue) {
    fs::path root = makeTempDir("decks_overwrite");

    fs::create_directories(root / "PublicDeck");
    {
        std::ofstream f(root / "PublicDeck" / "existing.json");
        f << R"({"new": ["new"]})";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    // Create data/decks with existing file
    fs::create_directories("data/decks");
    {
        std::ofstream f("data/decks/existing.json");
        f << R"({"old": ["old"]})";
    }

    ImportOptions opts;
    opts.overwrite = true;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 1);
    ASSERT_EQ(result.skipped, 0);

    // Verify the file was overwritten
    std::string content;
    {
        std::ifstream f(root / "data" / "decks" / "existing.json");
        content = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    EXPECT_TRUE(content.find("\"new\"") != std::string::npos);

    cleanupTempDir(root);
}

TEST(ImportDecks, NoPublicDeckDirReturnsEmpty) {
    fs::path root = makeTempDir("decks_nodir");
    // No PublicDeck directory

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 0);
    ASSERT_EQ(result.skipped, 0);
    ASSERT_EQ(result.failed, 0);
    ASSERT_TRUE(result.details.empty());

    cleanupTempDir(root);
}

TEST(ImportDecks, MultipleFilesMixedResults) {
    fs::path root = makeTempDir("decks_mixed");

    fs::create_directories(root / "PublicDeck");
    // Valid JSON
    {
        std::ofstream f(root / "PublicDeck" / "good1.json");
        f << R"({"deck1": ["a"]})";
    }
    // Non-JSON
    {
        std::ofstream f(root / "PublicDeck" / "notes.txt");
        f << "notes";
    }
    // Invalid JSON
    {
        std::ofstream f(root / "PublicDeck" / "bad.json");
        f << "{{{broken";
    }
    // Another valid JSON
    {
        std::ofstream f(root / "PublicDeck" / "good2.json");
        f << R"({"deck2": ["x", "y"]})";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importDecks(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 2);
    ASSERT_EQ(result.skipped, 1);  // notes.txt
    ASSERT_EQ(result.failed, 1);   // bad.json
    ASSERT_EQ((int)result.details.size(), 4);

    cleanupTempDir(root);
}

// ─── importMods Tests ─────────────────────────────────────────

TEST(ImportMods, ValidModImported) {
    fs::path root = makeTempDir("mods_valid");

    // Create mod directory with a file
    fs::create_directories(root / "mod" / "mymod");
    {
        std::ofstream f(root / "mod" / "mymod" / "descriptor.json");
        f << R"({"name": "mymod"})";
    }

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importMods(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 1);
    ASSERT_EQ(result.skipped, 0);
    ASSERT_EQ(result.failed, 0);

    cleanupTempDir(root);
}

TEST(ImportMods, NoModDirReturnsEmpty) {
    fs::path root = makeTempDir("mods_nodir");

    fs::path origCwd = fs::current_path();
    fs::current_path(root);

    ImportOptions opts;
    auto result = importMods(root, opts);

    fs::current_path(origCwd);

    ASSERT_EQ(result.success, 0);
    ASSERT_EQ(result.skipped, 0);
    ASSERT_EQ(result.failed, 0);

    cleanupTempDir(root);
}

TEST(ImportPlugins, ValidPluginImported) {
    fs::path root = makeTempDir("plugins_valid");
    fs::create_directories(root / "plugin");
    { std::ofstream f(root / "plugin" / "legacy.lua"); f << "msg_order = {}"; }
    fs::path origCwd = fs::current_path(); fs::current_path(root);
    auto result = importPlugins(root);
    fs::current_path(origCwd);
    ASSERT_EQ(result.success, 1);
    ASSERT_TRUE(fs::exists(root / "data" / "plugin" / "legacy.lua"));
    cleanupTempDir(root);
}

TEST(ImportBinaryCards, PreservesMetadataAndActiveCardBinding) {
    fs::path root = makeTempDir("binary_cards");
    fs::create_directories(root / "user");
    {
        std::ofstream out(root / "user" / "PlayerCards.RDconf", std::ios::binary);
        writeRaw<int32_t>(out, 1); writeRaw<int64_t>(out, 10001);
        writeRaw<int16_t>(out, 2); writeRaw<int16_t>(out, 1); writeRaw<uint16_t>(out, 0);
        writeLegacyStr(out, "Tag"); writeLegacyStr(out, "默认卡");
        writeLegacyStr(out, "Type"); writeLegacyStr(out, "coc7");
        writeLegacyStr(out, "Attr"); writeRaw<int16_t>(out, 3);
        writeLegacyStr(out, "力量"); writeLegacyInt(out, 70);
        writeLegacyStr(out, "备注"); writeLegacyUtf8(out, "测试文本");
        writeLegacyStr(out, "&伤害"); writeLegacyUtf8(out, "1d6+1");
        writeLegacyStr(out, "DiceExp"); writeRaw<int16_t>(out, 1); writeLegacyStr(out, "侦查"); writeLegacyStr(out, "1d100");
        writeLegacyStr(out, "Lock"); writeRaw<int16_t>(out, 1); writeLegacyStr(out, "力量");
        writeLegacyStr(out, "Info"); writeRaw<int16_t>(out, 1); writeLegacyStr(out, "来源"); writeLegacyStr(out, "旧版");
        writeLegacyStr(out, "Note"); writeLegacyStr(out, "卡片注记");
        writeLegacyStr(out, "END");
        writeRaw<int16_t>(out, 1); writeRaw<uint64_t>(out, 20001); writeRaw<uint16_t>(out, 0);
    }
    Database db; ASSERT_TRUE(db.open((root / "app.db").string()));
    int users = 0;
    ASSERT_EQ(importCards(db, root / "user", users), 1);
    ASSERT_EQ(users, 1);
    auto cards = db.getCardStorage()->get_all<CharacterCardRow>();
    ASSERT_EQ(cards.size(), (size_t)1);
    ASSERT_EQ(cards[0].userId, "10001");
    ASSERT_EQ(cards[0].name, "");
    auto attrs = json::parse(cards[0].attrs);
    ASSERT_EQ(attrs["力量"], 70);
    ASSERT_EQ(attrs["__meta"]["texts"]["备注"], "测试文本");
    ASSERT_EQ(attrs["__meta"]["texts"]["note"], "卡片注记");
    ASSERT_EQ(attrs["__meta"]["legacyExpressions"]["伤害"], "1d6+1");
    ASSERT_EQ(attrs["__meta"]["legacyType"], "coc7");
    auto binds = db.getStorage()->get_all<UserSettingRow>();
    ASSERT_EQ(binds.size(), (size_t)1);
    ASSERT_EQ(binds[0].userId, "10001"); ASSERT_EQ(binds[0].groupId, "20001");
    ASSERT_EQ(binds[0].key, "cardBind"); ASSERT_EQ(binds[0].value, "");
    db.close(); cleanupTempDir(root);
}

TEST(ImportBinaryUsers, KeepsScalarOnlyLuaConfiguration) {
    fs::path root = makeTempDir("binary_users");
    fs::create_directories(root / "user");
    {
        std::ofstream out(root / "user" / "UserConf.dat", std::ios::binary);
        writeRaw<int32_t>(out, 1); writeRaw<int64_t>(out, 10002);
        writeLegacyStr(out, "Cfg"); writeRaw<int16_t>(out, 1);
        writeLegacyStr(out, "pluginFlag"); writeLegacyInt(out, 7);
        writeLegacyStr(out, "END");
    }
    Database db; ASSERT_TRUE(db.open((root / "app.db").string()));
    LuaPluginManager lua;
    ASSERT_EQ(importUsers(db, root / "user", &lua), 1);
    ASSERT_EQ(lua.confGet("u:10002", "pluginFlag"), "7");
    ASSERT_EQ(db.getStorage()->count<PlayerProfileRow>(), 1);
    db.close(); cleanupTempDir(root);
}

TEST(ImportBinaryGroups, KeepsCoreAndLuaCompatibleSettings) {
    fs::path root = makeTempDir("binary_groups");
    fs::create_directories(root / "user");
    {
        std::ofstream out(root / "user" / "ChatConf.dat", std::ios::binary);
        writeRaw<int32_t>(out, 1); writeRaw<int64_t>(out, 20001); writeRaw<int64_t>(out, 20001);
        writeRaw<int16_t>(out, 4); writeLegacyStr(out, "旧群名");
        writeRaw<int16_t>(out, 11); writeRaw<int16_t>(out, 4);
        writeLegacyStr(out, "停用指令"); writeLegacyInt(out, 1);
        writeLegacyStr(out, "rc房规"); writeLegacyInt(out, 6);
        writeLegacyStr(out, "入群欢迎"); writeLegacyUtf8(out, "欢迎 {at}");
        writeLegacyStr(out, "pluginOption"); writeLegacyInt(out, 9);
        writeRaw<int16_t>(out, -1);
    }
    Database db; ASSERT_TRUE(db.open((root / "app.db").string()));
    LuaPluginManager lua;
    int groups = 0;
    ASSERT_EQ(importChatConf(db, root / "user", groups, &lua), 4);
    ASSERT_EQ(groups, 1);
    ASSERT_EQ(lua.confGet("g:20001", "pluginOption"), "9");
    auto rows = db.getStorage()->get_all<GroupSettingRow>();
    std::map<std::string, std::string> values;
    for (const auto& row : rows) values[row.key] = row.value;
    ASSERT_EQ(values["legacyGroupName"], "旧群名");
    ASSERT_EQ(values["enabled"], "0");
    ASSERT_EQ(values["cocRule"], "6");
    ASSERT_EQ(values["welcome"], "欢迎 {at}");
    db.close(); cleanupTempDir(root);
}

TEST(ImportLinks, ConvertsOnlySafeGroupLinks) {
    fs::path root = makeTempDir("links_valid");
    fs::create_directories(root / "conf");
    { std::ofstream out(root / "conf" / "LinkList.json");
      out << R"([{"origin":{"gid":20001},"target":{"gid":20002},"type":"to","linking":false},{"origin":{"uid":10001},"target":{"gid":20003},"type":"with","linking":true}])"; }
    ConfigManager cfg((root / "config").string()); ASSERT_TRUE(cfg.load());
    ASSERT_EQ(importLinks(cfg, root / "conf"), 1);
    auto links = cfg.get<json>("dice/links", json::array());
    ASSERT_EQ(links.size(), (size_t)1);
    ASSERT_EQ(links[0]["platform"], "onebot_v11");
    ASSERT_EQ(links[0]["home"], "20001"); ASSERT_EQ(links[0]["target"], "20002");
    ASSERT_EQ(links[0]["mode"], "to"); ASSERT_EQ(links[0]["active"], false);
    cleanupTempDir(root);
}

TEST(ImportNotices, ConvertsUserAndGroupWindows) {
    fs::path root = makeTempDir("notices_valid");
    fs::create_directories(root / "conf");
    { std::ofstream out(root / "conf" / "NoticeList.json");
      out << R"([{"uid":10001,"type":14},{"gid":20001,"type":3},{"gid":0,"type":15}])"; }
    ConfigManager cfg((root / "config").string()); ASSERT_TRUE(cfg.load());
    ASSERT_EQ(importNotices(cfg, root / "conf"), 2);
    auto windows = cfg.get<json>("dice/notice/windows", json::array());
    ASSERT_EQ(windows.size(), (size_t)2);
    ASSERT_EQ(windows[0]["chat_id"], "10001"); ASSERT_EQ(windows[0]["is_group"], false);
    ASSERT_EQ(windows[0]["level_mask"], 14);
    ASSERT_EQ(windows[1]["chat_id"], "20001"); ASSERT_EQ(windows[1]["is_group"], true);
    ASSERT_EQ(windows[1]["level_mask"], 3);
    cleanupTempDir(root);
}

TEST(BackupRestore, ArchivesStagesAndAppliesAtStartup) {
    fs::path root = makeTempDir("backup_restore");
    fs::path oldCwd = fs::current_path(); fs::current_path(root);
    fs::create_directories("config"); fs::create_directories("data/backups");
    { std::ofstream f("config/server.json"); f << R"({"server":{"port":18088}})"; }
    { std::ofstream f("data/custom.txt"); f << "before-restore"; }
    { std::ofstream f("data/backups/older-backup.zip"); f << "must-not-be-nested"; }
    fs::path archive; std::string error;
    {
        Database db;
        ASSERT_TRUE(db.open("data/dice.db"));
        ASSERT_TRUE(backup::createArchive(db, "config", archive, error));
        db.close();
    }
    ASSERT_TRUE(fs::is_regular_file(archive));
    ASSERT_TRUE(archive.filename().string().rfind("dicenext_bak_", 0) == 0);
    ASSERT_TRUE(archive.extension() == ".zip");
    const dice::proc::Result listed = backup::runArchive(backup::archiveListCommand(archive));
    ASSERT_TRUE(listed.output.find("data/backups/") == std::string::npos);
    ASSERT_EQ(listed.exitCode, 0);
    std::ifstream backupFile(archive, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(backupFile)), std::istreambuf_iterator<char>());
    backupFile.close();
    ASSERT_TRUE(backup::stageRestore(bytes, error));
    ASSERT_TRUE(backup::stageStoredRestore(archive.filename().string(), false, error));
    { std::ofstream f("data/custom.txt"); f << "changed"; }
    { std::ofstream f("config/server.json"); f << "{}"; }
    std::string notice; const bool restoredOk = backup::applyPendingRestore(notice);
    if (!restoredOk) std::cerr << "backup restore detail: " << notice << std::endl; ASSERT_TRUE(restoredOk);
    ASSERT_TRUE(notice.find("已应用") != std::string::npos);
    std::ifstream restored("data/custom.txt"); std::string content; std::getline(restored, content);
    ASSERT_EQ(content, "before-restore");
    ASSERT_TRUE(fs::exists("restore-rollbacks"));
    fs::current_path(oldCwd);
    // SQLite may still release a WAL sidecar asynchronously on Windows; cleanup is best-effort.
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
}

TEST(BackupRestore, PartialArchiveOverlaysOnlySelectedContent) {
    fs::path root = makeTempDir("backup_partial_restore");
    fs::path oldCwd = fs::current_path(); fs::current_path(root);
    fs::create_directories("config"); fs::create_directories("data/plugins/js");
    { std::ofstream f("config/server.json"); f << "{}"; }
    { std::ofstream f("data/plugins/js/example.js"); f << "original"; }
    { std::ofstream f("data/unrelated.txt"); f << "keep-me"; }
    fs::path archive; std::string error;
    {
        Database db;
        ASSERT_TRUE(db.open("data/dice.db"));
        backup::Selection selection{};
        selection.config = false;
        selection.coreDatabase = false;
        selection.characterCards = false;
        selection.chatHistory = false;
        selection.gameLogs = false;
        selection.runtimeLogs = false;
        selection.auditLogs = false;
        selection.decks = false;
        selection.rules = false;
        selection.help = false;
        selection.cardTemplates = false;
        selection.jsPlugins = true;
        selection.luaMods = false;
        selection.uploadedAssets = false;
        selection.resourceImages = false;
        selection.gameLogImages = false;
        selection.chatMedia = false;
        ASSERT_TRUE(backup::createArchive(db, "config", archive, error, selection));
        db.close();
    }
    std::ifstream backupFile(archive, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(backupFile)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(backup::stageRestore(bytes, error));
    { std::ofstream f("data/plugins/js/example.js"); f << "changed"; }
    { std::ofstream f("data/unrelated.txt"); f << "still-here"; }
    std::string notice; ASSERT_TRUE(backup::applyPendingRestore(notice));
    std::ifstream restoredPlugin("data/plugins/js/example.js"); std::string plugin; std::getline(restoredPlugin, plugin);
    std::ifstream untouched("data/unrelated.txt"); std::string unrelated; std::getline(untouched, unrelated);
    ASSERT_EQ(plugin, "original");
    ASSERT_EQ(unrelated, "still-here");
    fs::current_path(oldCwd); std::error_code cleanupError; fs::remove_all(root, cleanupError);
}

TEST(CustomMessageMigration, MapsOnlyAuditedSlots) {
    const auto& map = msgKeyMap();
    ASSERT_EQ(map.at("strRollDice"), "dice.roll.result");
    ASSERT_EQ(map.at("strGameJoined"), "game.joined");
    ASSERT_EQ(map.at("strObEnter"), "ob.joined");
    ASSERT_EQ(map.at("strLeaveUnused"), "event.leave_unused");
    ASSERT_EQ(map.at("strPropNotFound"), "card.attr_missing");
    // New log off/end callbacks no longer provide the old name/file variables.
    ASSERT_TRUE(map.find("strLogOff") == map.end());
    ASSERT_TRUE(map.find("strLogEnd") == map.end());
}

TEST(CustomMessageMigration, NormalizesAuditedLegacyPlaceholders) {
    ASSERT_EQ(normalizeLegacyTemplate("strRollDice", "{pc}掷骰: {res}"), "{nick}掷骰: {res}");
    ASSERT_EQ(normalizeLegacyTemplate("strDeckNotFound", "{self}找不到{deck_name}"), "{self}找不到{name}");
    ASSERT_EQ(normalizeLegacyTemplate("strLogNew", "{game.log_name}"), "{name}");
}

TEST(LegacyLog, ParsesOriginalHeader) {
    std::string sender, uid, stamp;
    ASSERT_TRUE(legacyLogHeader("测试玩家(123456) 2026-07-28 12:34:56", sender, uid, stamp));
    ASSERT_EQ(sender, "测试玩家");
    ASSERT_EQ(uid, "123456");
    ASSERT_EQ(stamp, "2026-07-28 12:34:56");
    ASSERT_FALSE(legacyLogHeader("not a header", sender, uid, stamp));
}

TEST(ImportSessions, ConvertsSessionAndLogContext) {
    fs::path root = makeTempDir("sessions_valid");
    fs::create_directories(root / "user" / "session");
    {
        std::ofstream f(root / "user" / "session" / "coc#1.json");
        f << R"({"master":[10001],"player":[10002],"data":{"rule":"COC7"},"roulette":{"100":{"copy":1,"pool":[1,2,3]}},"chats":[{"gid":20001}],"create_time":1700000000,"log":{"start":1700000000,"name":"测试日志","file":"coc#1_测试日志.txt","logging":false}})";
    }
    LuaPluginManager lua;
    std::map<std::string, LegacyLogContext> contexts;
    ASSERT_EQ(importSessions(root, &lua, contexts), 1);
    ASSERT_TRUE(!lua.confGet("game:index", "sessions").empty());
    ASSERT_TRUE(!lua.confGet("game:20001", "__session").empty());
    ASSERT_EQ(contexts.size(), (size_t)1);
    ASSERT_EQ(contexts.begin()->second.groupId, "20001");
    ASSERT_EQ(contexts.begin()->second.status, 1);
    cleanupTempDir(root);
}

TEST(ImportLogs, ConvertsTranscriptMessages) {
    fs::path root = makeTempDir("logs_valid");
    fs::create_directories(root / "user" / "log");
    { std::ofstream f(root / "user" / "log" / "20001_test.txt");
      f << "Alice(10001) 2026-07-28 12:00:00\n\n第一行\n第二行\n\nBot(99999) 2026-07-28 12:01:00\n\n回复"; }
    Database db; ASSERT_TRUE(db.open((root / "app.db").string()));
    std::map<std::string, LegacyLogContext> contexts;
    int messageCount = 0;
    ASSERT_EQ(importLogs(db, root, contexts, messageCount), 1);
    ASSERT_EQ(messageCount, 2);
    ASSERT_EQ(db.getLogStorage()->count<GameLogRow>(), 1);
    ASSERT_EQ(db.getLogStorage()->count<GameLogMessageRow>(), 2);
    db.close(); cleanupTempDir(root);
}

// ─── Backward Compatibility Tests ─────────────────────────────

TEST(BackCompat, ImportResultToJsonHasExpectedFields) {
    ImportResult result;
    result.success = 1;
    result.skipped = 1;
    result.failed = 0;
    result.details.push_back({"test.json", "success", ""});

    json j = result.toJSON();

    // Verify all expected fields exist
    ASSERT_TRUE(j.contains("success"));
    ASSERT_TRUE(j.contains("skipped"));
    ASSERT_TRUE(j.contains("failed"));
    ASSERT_TRUE(j.contains("details"));
    ASSERT_TRUE(j["details"].is_array());
    ASSERT_TRUE(j["details"][0].contains("name"));
    ASSERT_TRUE(j["details"][0].contains("status"));
    ASSERT_TRUE(j["details"][0].contains("reason"));
}

TEST(BackCompat, ValidateDeckAcceptsOriginalDiceFormat) {
    // Original Dice! deck format: object with array values
    std::string originalFormat = R"({
        "塔罗牌": ["愚者", "魔术师", "女祭司", "皇后", "皇帝"],
        "天气": ["晴天", "雨天", "雪天", "阴天"]
    })";
    std::string error = validateDeckJson(originalFormat);
    ASSERT_TRUE(error.empty());
}

TEST(LegacyConsole, ParsesYamlSettings) {
    const auto values = parseConsoleSettings(
        "master: 1\nconfig:\n  InactiveUserLine: 365\n  GroupClearLimit: 10\n  AllowStranger: 2\nclock:\n  - task: save\n", false);
    ASSERT_EQ(values.at("InactiveUserLine"), 365);
    ASSERT_EQ(values.at("GroupClearLimit"), 10);
    ASSERT_EQ(values.at("AllowStranger"), 2);
    ASSERT_TRUE(values.find("master") == values.end());
}

TEST(LegacyConsole, ParsesXmlSettings) {
    const auto values = parseConsoleSettings(
        "<root><master>1</master><conf><a>InactiveUserLine=360</a>"
        "<b>GroupInvalidSize=500</b><c>DisabledListenAt=1</c></conf></root>", true);
    ASSERT_EQ(values.at("InactiveUserLine"), 360);
    ASSERT_EQ(values.at("GroupInvalidSize"), 500);
    ASSERT_EQ(values.at("DisabledListenAt"), 1);
}

TEST(LegacyConsole, MigratesSettingsIntoActiveCapabilities) {
    fs::path root = makeTempDir("console_settings");
    fs::create_directories(root / "conf");
    {
        std::ofstream out(root / "conf" / "console.yaml");
        out << "master: 1707642\nconfig:\n"
               "  InactiveUserLine: 180\n"
               "  GroupClearLimit: 12\n"
               "  GroupInvalidSize: 600\n"
               "  DisabledListenAt: 1\n"
               "  LeaveBlackQQ: 1\n"
               "  AllowStranger: 2\n";
    }
    ConfigManager cfg((root / "config").string());
    ASSERT_TRUE(cfg.load());
    ASSERT_TRUE(importMasters(cfg, root / "conf") > 0);
    ASSERT_EQ(cfg.get<int>("dice/friend_clean_days"), 180);
    ASSERT_EQ(cfg.get<int>("dice/group_clear_limit"), 12);
    ASSERT_EQ(cfg.get<int>("dice/max_group_size"), 600);
    ASSERT_FALSE(cfg.get<bool>("dice/listen_at_when_off"));
    ASSERT_TRUE(cfg.get<bool>("dice/leave_black_qq"));
    ASSERT_EQ(cfg.get<std::string>("events/friend_policy"), "nonblacklist");
    auto masters = cfg.get<json>("dice/masters", json::array());
    ASSERT_EQ(masters.size(), static_cast<size_t>(1));
    ASSERT_EQ(masters[0].value("id", ""), "1707642");
    cleanupTempDir(root);
}
