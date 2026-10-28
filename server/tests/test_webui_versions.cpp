#include "test_framework.h"
#include "service/webui_versions.h"
#include "service/sso_auth.h"
#include "service/webui_entry.h"
#include <chrono>
#include <fstream>

namespace {
struct UiFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("dice_ui_versions_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    dice::ConfigManager config{(root / "config").string()};
    UiFixture() { if (!config.load()) throw std::runtime_error("config fixture failed"); }
    ~UiFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    void entry(const char* path) {
        const auto target = root / "web" / path;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream(target) << "<html>fixture</html>";
    }
};
}

TEST(WebuiVersions, DefaultV1AndLegacySingleBundle) {
    UiFixture f;
    f.entry("index.html");
    const auto state = dice::webui::versions(f.config, f.root / "web");
    ASSERT_EQ(state["selected"], "v1"); ASSERT_EQ(state["effective"], "v1");
    ASSERT_TRUE(state["legacy"].get<bool>()); ASSERT_FALSE(state["available"]["v2"].get<bool>());
}
TEST(WebuiVersions, SelectionSurvivesRestartAndPreservesCredentials) {
    UiFixture f; f.entry("v1/index.html"); f.entry("v2/index.html");
    ASSERT_TRUE(f.config.setAndSave("webui/password", "test-hash"));
    ASSERT_TRUE(f.config.setAndSave("webui/sso", {{"enabled", true}}));
    std::string error;
    ASSERT_TRUE(dice::webui::selectVersion(f.config, f.root / "web", {{"version", "v2"}}, error));
    dice::ConfigManager restarted{f.config.configPath()}; ASSERT_TRUE(restarted.load());
    ASSERT_EQ(dice::webui::versions(restarted, f.root / "web")["effective"], "v2");
    ASSERT_EQ(restarted.get<std::string>("webui/password"), "test-hash");
    ASSERT_TRUE(restarted.get<bool>("webui/sso/enabled"));
    ASSERT_TRUE(std::filesystem::is_regular_file(f.root / "config/webui.json"));
    ASSERT_TRUE(dice::webui::selectVersion(f.config, f.root / "web", {{"version", "v1"}}, error));
}
TEST(WebuiVersions, RejectsInvalidUnavailableAndUnrelatedWrites) {
    UiFixture f; f.entry("v1/index.html"); std::string error;
    for (const auto& body : {dice::json{{"version", "v2"}}, dice::json{{"version", "../../config"}},
         dice::json{{"version", 2}}, dice::json{{"version", "v1"}, {"password", "changed"}}, dice::json::array()}) {
        ASSERT_FALSE(dice::webui::selectVersion(f.config, f.root / "web", body, error));
        ASSERT_EQ(f.config.get<std::string>("webui/ui_version"), "v1");
    }
    ASSERT_FALSE(f.config.setAndSave("webui/ui_version", "invalid"));
}
TEST(WebuiVersions, MissingPreviewFallsBackWithoutOverwritingPreference) {
    UiFixture f; f.entry("v1/index.html");
    ASSERT_TRUE(f.config.setAndSave("webui/ui_version", "v2"));
    const auto state = dice::webui::versions(f.config, f.root / "web");
    ASSERT_EQ(state["selected"], "v2"); ASSERT_EQ(state["effective"], "v1");
    ASSERT_EQ(f.config.get<std::string>("webui/ui_version"), "v2");
}
TEST(WebuiVersions, RestrictedAdminCanReadButNotSetInstancePreference) {
    ASSERT_FALSE(dice::sso::blockedForRestrictedAdmin("GET", "/api/system/webui-version"));
    ASSERT_TRUE(dice::sso::blockedForRestrictedAdmin("PUT", "/api/system/webui-version"));
}
TEST(WebuiVersions, ActualEntryResponseUsesSavedPreferenceWithoutCaching) {
    UiFixture f; f.entry("v1/index.html"); f.entry("v2/index.html");
    auto response = dice::webui::entryResponse(f.config, f.root / "web", "sso=bound");
    ASSERT_EQ(response->statusCode(), drogon::k302Found);
    ASSERT_EQ(response->getHeader("Location"), "./v1/?sso=bound");
    ASSERT_EQ(response->getHeader("Cache-Control"), "no-store");
    ASSERT_TRUE(f.config.setAndSave("webui/ui_version", "v2"));
    response = dice::webui::entryResponse(f.config, f.root / "web");
    ASSERT_EQ(response->getHeader("Location"), "./v2/");
}
