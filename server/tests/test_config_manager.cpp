#include "test_framework.h"
#include "../src/config/config_manager.h"
#include "../src/config/scoped_settings.h"
#include "../src/service/web_auth.h"

#include <filesystem>
#include <fstream>
#include <chrono>

using namespace dice;
namespace fs = std::filesystem;

namespace {
fs::path temporaryConfigRoot(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (std::string("dice_next_") + name + "_" + std::to_string(nonce));
}
}

TEST(ConfigManager, SplitConfigRoundTripAndSnapshotRecovery) {
    const fs::path root = temporaryConfigRoot("config");
    const fs::path configDir = root / "config";
    std::error_code ec;
    fs::remove_all(root, ec);

    ConfigManager cfg(configDir.string());
    ASSERT_TRUE(cfg.load());
    ASSERT_TRUE(cfg.createdOnLoad());
    ASSERT_TRUE(fs::exists(configDir / "server.json"));
    ASSERT_TRUE(fs::exists(configDir / "adapters.json"));

    cfg.set<int>("server/port", 19001);
    cfg.set<std::string>("webui/password", "test-password");
    ASSERT_TRUE(cfg.save());
    const json good = cfg.getAll();

    ConfigManager reloaded(configDir.string());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.get<int>("server/port"), 19001);
    ASSERT_EQ(reloaded.get<std::string>("webui/password"), std::string("test-password"));

    { std::ofstream out(root / "config" / "server.json", std::ios::trunc); out << R"({"port":"invalid","db_path":"./custom-data/dice.db"})"; }
    ASSERT_FALSE(reloaded.reload());
    ASSERT_EQ(reloaded.recoveryDatabasePath("./data/dice.db"), std::string("./custom-data/dice.db"));
    ASSERT_TRUE(reloaded.restoreSnapshot(good));
    ASSERT_TRUE(reloaded.save());

    ConfigManager recovered(configDir.string());
    ASSERT_TRUE(recovered.load());
    ASSERT_EQ(recovered.get<int>("server/port"), 19001);
    fs::remove_all(root, ec);
}

TEST(ConfigManager, ObsoleteDefaultConfigIsDiscardedWithoutCreatingDefaults) {
    const fs::path root = temporaryConfigRoot("obsolete_default");
    const fs::path configDir = root / "config";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(configDir, ec);
    { std::ofstream out(configDir / "default_config.json", std::ios::trunc); out << R"({"server":{"port":8080}})"; }

    ConfigManager cfg(configDir.string());
    ASSERT_FALSE(cfg.load());
    ASSERT_TRUE(cfg.discardedObsoleteDefaultConfig());
    ASSERT_FALSE(fs::exists(configDir / "default_config.json"));
    ASSERT_FALSE(fs::exists(configDir / "server.json"));
    fs::remove_all(root, ec);
}

TEST(WebAuth, NewPasswordPolicyRequiresAllCharacterClasses) {
    ASSERT_TRUE(WebAuth::isValidNewPassword("Dice@2026!"));
    ASSERT_TRUE(WebAuth::isValidNewPassword("", true));
    ASSERT_FALSE(WebAuth::isValidNewPassword(""));
    ASSERT_FALSE(WebAuth::isValidNewPassword("Aa1!aaa"));
    ASSERT_FALSE(WebAuth::isValidNewPassword("dice@2026"));
    ASSERT_FALSE(WebAuth::isValidNewPassword("DICE@2026"));
    ASSERT_FALSE(WebAuth::isValidNewPassword("Dice@Test"));
    ASSERT_FALSE(WebAuth::isValidNewPassword("Dice2026"));
    ASSERT_FALSE(WebAuth::isValidNewPassword("Dice 2026!"));
    ASSERT_FALSE(WebAuth::isValidNewPassword("密码Dice@2026"));
    ASSERT_FALSE(WebAuth::isValidNewPassword(std::string(65, 'A') + "a1!"));
}

TEST(WebAuth, TrustedDeviceSessionSurvivesReconfigure) {
    const fs::path root = temporaryConfigRoot("web_auth");
    const fs::path sessions = root / "config" / "webui_sessions.json";
    std::error_code ec;
    fs::remove_all(root, ec);

    auto& auth = WebAuth::instance();
    auth.configure("test-password", sessions);
    const std::string token = auth.issueToken(true);
    ASSERT_TRUE(fs::exists(sessions));
    ASSERT_TRUE(auth.validToken(token));

    // configure() represents a server restart: all memory-only sessions are
    // gone, while the trusted token must be recovered from disk.
    auth.configure("test-password", sessions);
    ASSERT_TRUE(auth.validToken(token));
    auth.revoke(token);
    ASSERT_FALSE(auth.validToken(token));
    auth.configure("", {});
    fs::remove_all(root, ec);
}

TEST(WebAuth, HashedPasswordSurvivesRestart) {
    const fs::path root = temporaryConfigRoot("web_auth_password");
    const fs::path sessions = root / "config" / "webui_sessions.json";
    std::error_code ec;
    fs::remove_all(root, ec);

    const std::string password = "restart-safe-password";
    const std::string stored = WebAuth::hashPassword(password);
    ASSERT_TRUE(stored.rfind("pbkdf2$", 0) == 0);
    ASSERT_TRUE(WebAuth::verifyPassword(stored, password));
    ASSERT_FALSE(WebAuth::verifyPassword(stored, "wrong-password"));

    // configure() is the startup path: it receives the hash read back from
    // config/webui.json and must validate the user's original plain password.
    auto& auth = WebAuth::instance();
    auth.configure(stored, sessions);
    ASSERT_TRUE(auth.checkPassword(password));
    ASSERT_FALSE(auth.checkPassword("wrong-password"));
    ASSERT_FALSE(WebAuth::verifyPassword("pbkdf2$bad$format", password));
    ASSERT_FALSE(WebAuth::verifyPassword(stored + "$extra", password));

    auth.configure("", {});
    fs::remove_all(root, ec);
}
TEST(ConfigManager, ScopedSettingsResolveAccountThenAdapterThenGlobal) {
    json all = {
        {"events", {
            {"friend_policy", "manual"},
            {"poke_enabled", true},
            {"poke_command", ".jrrp"}
        }},
        {"dice", {
            {"scoped_overrides", {
                {"adapter", {
                    {"onebot_v11", {
                        {"events", {
                            {"friend_policy", "all"},
                            {"poke_enabled", false}
                        }}
                    }}
                }},
                {"account", {
                    {"12", {
                        {"events", {
                            {"friend_policy", "keyword"}
                        }}
                    }}
                }}
            }}
        }}
    };

    const json account = scoped_settings::resolveSection(all, "events", "onebot_v11", "12");
    ASSERT_EQ(account.value("friend_policy", std::string()), std::string("keyword"));
    ASSERT_FALSE(account.value("poke_enabled", true));
    ASSERT_EQ(account.value("poke_command", std::string()), std::string(".jrrp"));
    ASSERT_EQ(scoped_settings::sourceFor(all, "events", "friend_policy", "onebot_v11", "12"), std::string("account"));
    ASSERT_EQ(scoped_settings::sourceFor(all, "events", "poke_enabled", "onebot_v11", "12"), std::string("adapter"));
    ASSERT_EQ(scoped_settings::sourceFor(all, "events", "poke_command", "onebot_v11", "12"), std::string("global"));

    const json otherAccount = scoped_settings::resolveSection(all, "events", "onebot_v11", "13");
    ASSERT_EQ(otherAccount.value("friend_policy", std::string()), std::string("all"));

    const json otherAdapter = scoped_settings::resolveSection(all, "events", "discord", "99");
    ASSERT_EQ(otherAdapter.value("friend_policy", std::string()), std::string("manual"));
    ASSERT_TRUE(otherAdapter.value("poke_enabled", false));
}
TEST(ConfigManager, ScopedSettingsPersistAndCanRestoreInheritance) {
    const fs::path root = temporaryConfigRoot("scoped_settings");
    const fs::path configDir = root / "config";
    std::error_code ec;
    fs::remove_all(root, ec);

    ConfigManager cfg(configDir.string());
    ASSERT_TRUE(cfg.load());
    cfg.set<std::string>("events/friend_policy", "manual");
    ASSERT_TRUE(scoped_settings::setSection(cfg, "adapter", "onebot_v11", "events",
        json{{"friend_policy", "all"}}));
    ASSERT_TRUE(scoped_settings::setSection(cfg, "account", "12", "events",
        json{{"friend_policy", "keyword"}}));
    ASSERT_TRUE(cfg.save());

    ConfigManager reloaded(configDir.string());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(scoped_settings::resolveSection(reloaded.getAll(), "events", "onebot_v11", "12")
        .value("friend_policy", std::string()), std::string("keyword"));

    ASSERT_TRUE(scoped_settings::setSection(reloaded, "account", "12", "events",
        json::object(), json::array({"friend_policy"})));
    ASSERT_EQ(scoped_settings::resolveSection(reloaded.getAll(), "events", "onebot_v11", "12")
        .value("friend_policy", std::string()), std::string("all"));

    ASSERT_TRUE(scoped_settings::setSection(reloaded, "adapter", "onebot_v11", "events",
        json::object(), json::array({"friend_policy"})));
    ASSERT_EQ(scoped_settings::resolveSection(reloaded.getAll(), "events", "onebot_v11", "12")
        .value("friend_policy", std::string()), std::string("manual"));

    fs::remove_all(root, ec);
}
