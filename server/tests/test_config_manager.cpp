#include "test_framework.h"
#include "../src/config/config_manager.h"

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
