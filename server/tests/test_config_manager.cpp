#include "test_framework.h"
#include "../src/config/config_manager.h"
#include "../src/config/scoped_settings.h"
#include "../src/service/web_auth.h"
#include "../src/common/hot_reload.h"
#include "test_watchdog.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>
#include <thread>

using namespace dice;
namespace fs = std::filesystem;

namespace {
fs::path temporaryConfigRoot(const char* name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (std::string("dice_next_") + name + "_" + std::to_string(nonce));
}

struct ReloadFixture {
    fs::path root = temporaryConfigRoot("reload");
    ConfigManager cfg{(root / "config").string()};
    ReloadFixture() {
        if (!cfg.load()) throw std::runtime_error("cannot create configuration");
        cfg.set<std::string>("server/api_key", "fixed-test-api-key");
        if (!cfg.save()) throw std::runtime_error("cannot persist configuration");
    }
    ~ReloadFixture() { std::error_code ec; fs::remove_all(root, ec); }
};
}

TEST(ConfigManager, IgnoresTemporarySessionAndNestedFiles) {
    for (const auto& file : {"server.json", "dice.json", "custom_rule.json", "rules-1.json"})
        ASSERT_TRUE(ConfigManager::isConfigurationFile(file));
    for (const auto& file : {"webui_sessions.json", "default_config.json", "dice.json.tmp", ".dice.json",
                            "runtime.log", "data/dice.json", "nested\\server.json", "dice.json~", "server"})
        ASSERT_FALSE(ConfigManager::isConfigurationFile(file));
}

TEST(ConfigManager, RepeatedIdenticalReloadsDoNotEmitCallbacksOrSnapshots) {
    ReloadFixture f;
    ConfigManager observer(f.cfg.configPath());
    ASSERT_TRUE(observer.load());
    int callbacks = 0, snapshots = 0;
    observer.onConfigChanged([&] { ++callbacks; });
    observer.setSnapshotWriter([&](const json&) { ++snapshots; });
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(observer.reload());
    ASSERT_EQ(callbacks, 0);
    ASSERT_EQ(snapshots, 0);
    // Whitespace / key-order-only edits are not functional changes either.
    const auto original = observer.getAll();
    { std::ofstream out(f.root / "config" / "dice.json"); out << original["dice"].dump(4); }
    ASSERT_TRUE(observer.reload());
    ASSERT_EQ(callbacks, 0);
    const auto timestamp = fs::last_write_time(f.root / "config" / "dice.json");
    ASSERT_TRUE(observer.save());
    ASSERT_TRUE(fs::last_write_time(f.root / "config" / "dice.json") == timestamp);
}

TEST(ConfigManager, ChangedValidContentReloadsOnceAndInvalidEditPreservesOldState) {
    ReloadFixture f;
    ConfigManager observer(f.cfg.configPath());
    ASSERT_TRUE(observer.load());
    int callbacks = 0;
    observer.onConfigChanged([&] { ++callbacks; });
    auto changed = observer.getAll()["dice"];
    changed["rules"]["default_dice_sides"] = 20;
    { std::ofstream out(f.root / "config" / "dice.json"); out << changed; }
    ASSERT_TRUE(observer.reload());
    ASSERT_EQ(callbacks, 1);
    ASSERT_EQ(observer.get<int>("dice/rules/default_dice_sides"), 20);
    ASSERT_TRUE(observer.reload());
    ASSERT_EQ(callbacks, 1);
    { std::ofstream out(f.root / "config" / "dice.json"); out << "{invalid"; }
    ASSERT_FALSE(observer.reload());
    ASSERT_EQ(callbacks, 1);
    ASSERT_EQ(observer.get<int>("dice/rules/default_dice_sides"), 20);
}

TEST(ConfigManager, ExternalEditImmediatelyAfterOwnSaveIsNotLost) {
    ReloadFixture f;
    int callbacks = 0;
    f.cfg.onConfigChanged([&] { ++callbacks; });
    ASSERT_TRUE(f.cfg.save());
    auto changed = f.cfg.getAll()["dice"];
    changed["rules"]["default_dice_sides"] = 8;
    { std::ofstream out(f.root / "config" / "dice.json"); out << changed; }
    ASSERT_TRUE(f.cfg.reload());
    ASSERT_EQ(f.cfg.get<int>("dice/rules/default_dice_sides"), 8);
    ASSERT_EQ(callbacks, 1);
}

TEST(ConfigManager, MissingApiKeyDoesNotChangeOnEveryRead) {
    ReloadFixture f;
    auto server = f.cfg.getAll()["server"];
    server["api_key"] = "";
    { std::ofstream out(f.root / "config" / "server.json"); out << server; }
    ConfigManager observer(f.cfg.configPath());
    ASSERT_TRUE(observer.load());
    const auto key = observer.get<std::string>("server/api_key");
    ASSERT_FALSE(key.empty());
    int callbacks = 0;
    observer.onConfigChanged([&] { ++callbacks; });
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(observer.reload());
    ASSERT_EQ(callbacks, 0);
    ASSERT_EQ(observer.get<std::string>("server/api_key"), key);
}

TEST(ConfigManager, FreshInstallWritesTheApiKeyBeforeAnyoneReadsIt) {
    // main.cpp 启动时只读一次密钥。以前首次启动写出去的是空值，要等热重载才补上
    const auto root = temporaryConfigRoot("fresh_key");
    {
        ConfigManager cfg((root / "config").string());
        ASSERT_TRUE(cfg.load());
        const auto key = cfg.get<std::string>("server/api_key", std::string());
        ASSERT_FALSE(key.empty());
        json server; { std::ifstream in(root / "config" / "server.json"); in >> server; }
        ASSERT_EQ(server.value("api_key", std::string()), key);
        ConfigManager again((root / "config").string());
        ASSERT_TRUE(again.load());
        ASSERT_EQ(again.get<std::string>("server/api_key", std::string()), key);
    }
    std::error_code ec; fs::remove_all(root, ec);
}

TEST(ConfigManager, DefaultsSavedAfterAnIncompleteDirectoryCarryTheApiKey) {
    // BDC 托管实例首启：守护进程先放了别的配置文件，读取因缺 server.json 失败，随后写出默认值
    const auto root = temporaryConfigRoot("partial_key");
    {
        fs::create_directories(root / "config");
        { std::ofstream out(root / "config" / "dice.json"); out << "{}"; }
        ConfigManager cfg((root / "config").string());
        ASSERT_FALSE(cfg.load());
        ASSERT_TRUE(cfg.save());
        const auto key = cfg.get<std::string>("server/api_key", std::string());
        ASSERT_FALSE(key.empty());
        ConfigManager again((root / "config").string());
        ASSERT_TRUE(again.load());
        ASSERT_EQ(again.get<std::string>("server/api_key", std::string()), key);
    }
    std::error_code ec; fs::remove_all(root, ec);
}

TEST(ConfigManager, NativeWatcherCoalescesConfigChangesAndIgnoresRuntimeWrites) {
#if defined(_WIN32) || defined(__linux__)
    TestWatchdog watchdog;
    ReloadFixture f;
    ConfigManager observer(f.cfg.configPath());
    ASSERT_TRUE(observer.load());
    std::atomic<int> reloads{0}, notifications{0};
    std::atomic<bool> failed{false};
    observer.onConfigChanged([&] { ++reloads; });
    HotReloadMonitor watcher;
    ASSERT_TRUE(watcher.start(observer.configPath(), 100, [&](const std::string&) {
        ++notifications;
        if (!observer.reload()) failed = true;
    }, ConfigManager::isConfigurationFile));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int i = 0; i < 100; ++i) {
        std::ofstream(f.root / "config" / "runtime.log") << i;
        std::ofstream(f.root / "config" / "webui_sessions.json") << "{}";
        std::ofstream(f.root / "config" / "dice.json.tmp") << "{}";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ASSERT_EQ(reloads.load(), 0);
    // An overflow may prompt a content check, but must not apply any config.
    const auto beforeEdit = notifications.load();
    auto changed = observer.getAll()["dice"];
    changed["rules"]["default_dice_sides"] = 12;
    { std::ofstream out(f.root / "config" / "dice.json"); out << changed; }
    // More changes in the same quiet period must not hide the real JSON edit.
    std::ofstream(f.root / "config" / "webui_sessions.json") << "{}";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!reloads.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    watcher.stop();
    ASSERT_FALSE(failed.load());
    ASSERT_EQ(reloads.load(), 1);
    ASSERT_EQ(notifications.load(), beforeEdit + 1);
    ASSERT_EQ(observer.get<int>("dice/rules/default_dice_sides"), 12);
#endif
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

TEST(ConfigManager, TrayTextPersistsAndRejectsInvalidSnapshots) {
    const fs::path root = temporaryConfigRoot("tray_text");
    ConfigManager cfg((root / "config").string());
    ASSERT_TRUE(cfg.load());
    ASSERT_EQ(cfg.get<std::string>("dice/tray_text", ""), "");
    cfg.set<std::string>("dice/tray_text", "希亚骰🎲");
    ASSERT_TRUE(cfg.save());
    ConfigManager reloaded((root / "config").string());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.get<std::string>("dice/tray_text", ""), "希亚骰🎲");
    const auto good = cfg.getAll();
    for (const auto& invalid : {json(42), json("12345678901"), json("骰\n骰")}) {
        auto snapshot = good;
        snapshot["dice"]["tray_text"] = invalid;
        ASSERT_FALSE(reloaded.restoreSnapshot(snapshot));
        ASSERT_EQ(reloaded.get<std::string>("dice/tray_text", ""), "希亚骰🎲");
    }
    cfg.set<std::string>("dice/tray_text", "");
    ASSERT_TRUE(cfg.save());
    ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.get<std::string>("dice/tray_text", ""), "");
    std::error_code ec;
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
    const std::string cookieName = auth.sessionCookieName();
    const std::string token = auth.issueToken(true);
    const std::string browserSessionToken = auth.issueToken(false);
    ASSERT_TRUE(fs::exists(sessions));
    ASSERT_TRUE(auth.validToken(token));
    ASSERT_TRUE(auth.persistentSecondsRemaining(token) > 0);
    ASSERT_TRUE(auth.validToken(browserSessionToken));
    ASSERT_EQ(auth.persistentSecondsRemaining(browserSessionToken), 0);

    // configure() represents a server restart: all memory-only sessions are
    // gone, while the trusted token must be recovered from disk.
    auth.configure("test-password", sessions);
    ASSERT_EQ(auth.sessionCookieName(), cookieName);
    ASSERT_TRUE(auth.validToken(token));
    ASSERT_FALSE(auth.validToken(browserSessionToken));
    auth.revoke(token);
    ASSERT_FALSE(auth.validToken(token));
    auth.configure("", {});
    fs::remove_all(root, ec);
}

TEST(WebAuth, SessionCookieNamesAreStableAndSeparateInstallations) {
    const fs::path root = temporaryConfigRoot("web_auth_cookie");
    const fs::path sessionsA = root / "instance-a" / "config" / "webui_sessions.json";
    const fs::path sessionsB = root / "instance-b" / "config" / "webui_sessions.json";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(sessionsA.parent_path(), ec);
    ASSERT_FALSE(static_cast<bool>(ec));
    fs::create_directories(sessionsB.parent_path(), ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    const std::string cookieA = WebAuth::cookieNameForSessionsPath(sessionsA);
    const std::string cookieANormalized = WebAuth::cookieNameForSessionsPath(
        sessionsA.parent_path() / ".." / "config" / "webui_sessions.json");
    const std::string cookieB = WebAuth::cookieNameForSessionsPath(sessionsB);
    ASSERT_TRUE(cookieA.rfind("dice_session_", 0) == 0);
    ASSERT_EQ(cookieA, cookieANormalized);
    ASSERT_NE(cookieA, cookieB);

    auto& auth = WebAuth::instance();
    auth.configure("test-password", sessionsA);
    ASSERT_EQ(auth.sessionCookieName(), cookieA);
    auth.configure("test-password", sessionsB);
    ASSERT_EQ(auth.sessionCookieName(), cookieB);

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
