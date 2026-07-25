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
#include <sqlite_orm/sqlite_orm.h>

// Provide complete definitions for forward-declared classes so that
// legacy_import_v2.h's runImport() body compiles. These are stubs —
// the import tests only test utility functions + importDecks/importMods.
namespace dice {
class CardDeck { public: void loadDir(const std::string&) {} };
class LuaPluginManager { public: void reload() {} };
}

// Include database.h which defines the row structs
#include "../src/storage/database.h"

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
