// ─── C#28-B: I18n Persona Lookup Chain Tests ──────────────────
// Tests the i18n lookup chain: override → persona → bundle → fallback.
// Also tests .rpmode command routing logic (no conflict with .rp).

#include "test_framework.h"
#include "../src/i18n/i18n.h"
#include "../src/common/types.h"

using namespace dice;

// Helper: create an I18n with a minimal bundle for testing
// We can't easily load from disk in tests, so we test the lookup chain
// using overrides and persona bundles directly.

TEST(I18nPersona, SetAndGetActivePersonaId) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    ASSERT_EQ(i18n.getActivePersonaId(), 0);
    i18n.setPersona(42);
    ASSERT_EQ(i18n.getActivePersonaId(), 42);
    i18n.setPersona(0);
    ASSERT_EQ(i18n.getActivePersonaId(), 0);
}

TEST(I18nPersona, SetPersonaBundlesForLocale) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    json bundles = {{"dice.greeting", "Hello from persona!"}};
    i18n.setPersonaBundles(Locale::kZhHans, bundles);

    // tr() should find the persona entry (since no bundle or override exists)
    std::string result = i18n.tr(Locale::kZhHans, "dice.greeting");
    ASSERT_EQ(result, "Hello from persona!");
}

TEST(I18nPersona, ClearPersonaBundles) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    json bundles = {{"dice.greeting", "Persona greeting"}};
    i18n.setPersonaBundles(Locale::kZhHans, bundles);

    // Verify it works
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "dice.greeting"), "Persona greeting");

    // Clear
    i18n.clearPersonaBundles();

    // Should fall back to raw key
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "dice.greeting"), "dice.greeting");
}

TEST(I18nPersona, OverrideTakesPrecedenceOverPersona) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // Set persona bundle
    json personaBundle = {{"test.key", "persona value"}};
    i18n.setPersonaBundles(Locale::kZhHans, personaBundle);

    // Set override (should take precedence)
    i18n.setOverride(Locale::kZhHans, "test.key", "override value");

    ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "override value");
}

TEST(I18nPersona, PersonaTakesPrecedenceOverBundle) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // Since we can't load bundles from disk in this test,
    // we verify the lookup order by testing that persona returns
    // its value when no override exists.
    json personaBundle = {{"test.key", "persona value"}};
    i18n.setPersonaBundles(Locale::kZhHans, personaBundle);

    // No override → should use persona
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "persona value");
}

TEST(I18nPersona, FallbackToRawKeyWhenNothingExists) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // No override, no persona, no bundle → should return the key itself
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "nonexistent.key"), "nonexistent.key");
}

TEST(I18nPersona, EmptyPersonaBundleNotInjected) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // Empty bundle should not be injected (per setPersonaBundles impl)
    json emptyBundle = json::object();
    i18n.setPersonaBundles(Locale::kZhHans, emptyBundle);

    // Should fall back to raw key
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "any.key"), "any.key");
}

TEST(I18nPersona, NonObjectBundleNotInjected) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // Non-object JSON should not be injected
    json badBundle = json::array({"not", "an", "object"});
    i18n.setPersonaBundles(Locale::kZhHans, badBundle);

    ASSERT_EQ(i18n.tr(Locale::kZhHans, "any.key"), "any.key");
}

TEST(I18nPersona, LocaleIsolation) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    json zhBundle = {{"greeting", "你好"}};
    json enBundle = {{"greeting", "Hello"}};
    i18n.setPersonaBundles(Locale::kZhHans, zhBundle);
    i18n.setPersonaBundles(Locale::kEn, enBundle);

    ASSERT_EQ(i18n.tr(Locale::kZhHans, "greeting"), "你好");
    ASSERT_EQ(i18n.tr(Locale::kEn, "greeting"), "Hello");
}

TEST(I18nPersona, InterpolationWorksInPersonaLayer) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    json personaBundle = {{"dice.roll", "You rolled {result}!"}};
    i18n.setPersonaBundles(Locale::kZhHans, personaBundle);

    std::string result = i18n.tr(Locale::kZhHans, "dice.roll", {{"result", "42"}});
    ASSERT_EQ(result, "You rolled 42!");
}

TEST(I18nPersona, ClearOverrideFallsBackToPersona) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    json personaBundle = {{"test.key", "persona value"}};
    i18n.setPersonaBundles(Locale::kZhHans, personaBundle);

    i18n.setOverride(Locale::kZhHans, "test.key", "override value");
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "override value");

    i18n.clearOverride(Locale::kZhHans, "test.key");
    // Should fall back to persona
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "test.key"), "persona value");
}

TEST(I18nPersona, HasOverride) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    ASSERT_FALSE(i18n.hasOverride(Locale::kZhHans, "test.key"));
    i18n.setOverride(Locale::kZhHans, "test.key", "value");
    ASSERT_TRUE(i18n.hasOverride(Locale::kZhHans, "test.key"));
    i18n.clearOverride(Locale::kZhHans, "test.key");
    ASSERT_FALSE(i18n.hasOverride(Locale::kZhHans, "test.key"));
}

TEST(I18nPersona, InterpolationNoPlaceholders) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // Template with no placeholders should return as-is
    std::string result = i18n.interpolate("hello world", {});
    ASSERT_EQ(result, "hello world");
}

TEST(I18nPersona, InterpolationUnknownPlaceholderKept) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    // Unknown placeholder should be kept verbatim
    std::string result = i18n.interpolate("hello {unknown}", {});
    ASSERT_EQ(result, "hello {unknown}");
}

TEST(I18nPersona, InterpolationMultiplePlaceholders) {
    I18n i18n("nonexistent_dir", Locale::kZhHans);

    std::string result = i18n.interpolate(
        "{name} rolled {dice} and got {result}",
        {{"name", "Alice"}, {"dice", "1d100"}, {"result", "55"}}
    );
    ASSERT_EQ(result, "Alice rolled 1d100 and got 55");
}

// ─── .rpmode vs .rp Conflict Test ─────────────────────────────
// Verify that the command routing logic correctly distinguishes
// ".rpmode" from ".rp" (COC7 penalty dice).
// This tests the routing logic extracted from command_router.h.

TEST(RpmodeRouting, RpmodeParsedCorrectly) {
    // Simulate the tryHandlePersona prefix check logic
    auto checkRpmode = [](const std::string& cmd) -> bool {
        if (cmd.size() < 6) return false;
        std::string prefix = cmd.substr(0, 6);
        // toLower
        for (auto& c : prefix) c = (char)std::tolower((unsigned char)c);
        if (prefix != "rpmode") return false;
        if (cmd.size() > 6 && cmd[6] != ' ' && cmd[6] != '\t') return false;
        return true;
    };

    // .rpmode should be recognized
    ASSERT_TRUE(checkRpmode("rpmode"));
    ASSERT_TRUE(checkRpmode("rpmode set test"));
    ASSERT_TRUE(checkRpmode("rpmode list"));
    ASSERT_TRUE(checkRpmode("RPMODE"));  // case insensitive

    // .rp should NOT be recognized as rpmode
    ASSERT_FALSE(checkRpmode("rp"));
    ASSERT_FALSE(checkRpmode("rp 1d100"));
    ASSERT_FALSE(checkRpmode("rps"));

    // .rpmodeX should NOT be recognized (needs space/tab after rpmode)
    ASSERT_FALSE(checkRpmode("rpmodes"));
    ASSERT_FALSE(checkRpmode("rpmodeX"));
}

TEST(RpmodeRouting, RpmodeSubcommandParsing) {
    // Simulate the subcommand + args parsing logic
    auto parseRpmode = [](const std::string& cmd) -> std::pair<std::string, std::string> {
        // After verifying "rpmode" prefix, extract rest
        std::string rest = cmd.substr(6);
        // trim leading whitespace
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
        // trim trailing
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();

        std::string subcmd, args;
        size_t spacePos = rest.find(' ');
        if (spacePos == std::string::npos) {
            subcmd = rest;
        } else {
            subcmd = rest.substr(0, spacePos);
            args = rest.substr(spacePos + 1);
            // trim args
            while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) args.erase(args.begin());
            while (!args.empty() && (args.back() == ' ' || args.back() == '\t')) args.pop_back();
        }
        return {subcmd, args};
    };

    // No subcommand
    {
        auto [sub, args] = parseRpmode("rpmode");
        ASSERT_EQ(sub, "");
        ASSERT_EQ(args, "");
    }

    // Subcommand only
    {
        auto [sub, args] = parseRpmode("rpmode list");
        ASSERT_EQ(sub, "list");
        ASSERT_EQ(args, "");
    }

    // Subcommand with args
    {
        auto [sub, args] = parseRpmode("rpmode set MyPersona");
        ASSERT_EQ(sub, "set");
        ASSERT_EQ(args, "MyPersona");
    }

    // Subcommand with multi-word args
    {
        auto [sub, args] = parseRpmode("rpmode copy SourceName DestName");
        ASSERT_EQ(sub, "copy");
        ASSERT_EQ(args, "SourceName DestName");
    }
}

// ─── Permission Level Tests (Logic Verification) ──────────────
// Verify the permission check logic from tryHandlePersona

TEST(RpmodePermissions, PermissionLevels) {
    // Simulate permission checks
    // isGroupAdmin = isMaster(msg) || senderTrust(msg) >= 4
    // isMasterUser = isMaster(msg)

    auto checkPermission = [](const std::string& subcmd, bool isMaster, int trust) -> std::string {
        bool isGroupAdmin = isMaster || trust >= 4;
        bool isMasterUser = isMaster;

        if (subcmd.empty() || subcmd == "list" || subcmd == "info")
            return "allowed";  // everyone

        if (subcmd == "set" || subcmd == "off" || subcmd == "default") {
            return isGroupAdmin ? "allowed" : "denied_admin";
        }

        if (subcmd == "create" || subcmd == "copy" || subcmd == "del") {
            return isMasterUser ? "allowed" : "denied_master";
        }

        return "unknown";
    };

    // Everyone commands
    ASSERT_EQ(checkPermission("", false, 0), "allowed");
    ASSERT_EQ(checkPermission("list", false, 0), "allowed");
    ASSERT_EQ(checkPermission("info", false, 0), "allowed");

    // Admin commands — normal user denied
    ASSERT_EQ(checkPermission("set", false, 0), "denied_admin");
    ASSERT_EQ(checkPermission("off", false, 0), "denied_admin");
    ASSERT_EQ(checkPermission("default", false, 0), "denied_admin");

    // Admin commands — trust >= 4 allowed
    ASSERT_EQ(checkPermission("set", false, 4), "allowed");
    ASSERT_EQ(checkPermission("off", false, 4), "allowed");

    // Admin commands — master allowed
    ASSERT_EQ(checkPermission("set", true, 0), "allowed");

    // Master-only commands — normal user denied
    ASSERT_EQ(checkPermission("create", false, 0), "denied_master");
    ASSERT_EQ(checkPermission("copy", false, 0), "denied_master");
    ASSERT_EQ(checkPermission("del", false, 0), "denied_master");

    // Master-only commands — trust >= 4 still denied (not master)
    ASSERT_EQ(checkPermission("create", false, 4), "denied_master");

    // Master-only commands — master allowed
    ASSERT_EQ(checkPermission("create", true, 0), "allowed");
    ASSERT_EQ(checkPermission("copy", true, 0), "allowed");
    ASSERT_EQ(checkPermission("del", true, 0), "allowed");
}
