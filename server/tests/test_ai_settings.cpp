#include "test_framework.h"
#include "../src/service/ai_vision_config.h"
#include "../src/config/config_manager.h"

#include <chrono>
#include <filesystem>

using namespace dice;

TEST(AiVisionSettings, MissingValuesUseCompatibleDefaults) {
    const auto value = aivision::normalizeSettings(json::object());
    ASSERT_TRUE(value.at("pass_url").get<bool>());
    ASSERT_FALSE(value.at("enabled").get<bool>());
    ASSERT_EQ(value.at("max_images").get<int>(), 2);
    ASSERT_EQ(value.at("model_id").get<std::string>(), "");
}

TEST(AiVisionSettings, BothSwitchStatesSurviveRepeatedApiRoundTrips) {
    for (const bool direct : {false, true}) {
        json value = {{"enabled", true}, {"model_id", "vision-model"},
                      {"prompt", "describe"}, {"max_images", 3}, {"pass_url", direct}};
        const auto expected = value;
        for (int i = 0; i < 5; ++i) value = aivision::normalizeSettings(value);
        ASSERT_TRUE(value == expected);
    }
}

TEST(AiVisionSettings, OlderClientCannotReenableExplicitlyDisabledDirectUrls) {
    const json previous = {{"pass_url", false}};
    const auto value = aivision::normalizeSettings({{"enabled", true}}, previous);
    ASSERT_FALSE(value.at("pass_url").get<bool>());
    ASSERT_TRUE(aivision::normalizeSettings({{"pass_url", true}}, previous).at("pass_url").get<bool>());
}

TEST(AiVisionSettings, ImageLimitsRemainBoundedAndWrongBooleanTypesAreRejected) {
    ASSERT_EQ(aivision::normalizeSettings({{"max_images", -2}}).at("max_images").get<int>(), 1);
    ASSERT_EQ(aivision::normalizeSettings({{"max_images", 50}}).at("max_images").get<int>(), 4);
    for (const json wrong : {json("false"), json(0), json(nullptr)}) {
        bool rejected = false;
        try { (void)aivision::normalizeSettings({{"pass_url", wrong}}); }
        catch (const json::type_error&) { rejected = true; }
        ASSERT_TRUE(rejected);
    }
}

TEST(AiVisionSettings, DisabledDirectUrlsSurviveSaveAndRestart) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("dnx-ai-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    ConfigManager cfg(root.string());
    ASSERT_TRUE(cfg.load());
    cfg.set<json>("dice/ai/vision", aivision::normalizeSettings({{"pass_url", false}, {"enabled", true}}));
    ASSERT_TRUE(cfg.save());
    ConfigManager restarted(root.string());
    ASSERT_TRUE(restarted.load());
    const auto loaded = aivision::normalizeSettings(restarted.get<json>("dice/ai/vision"));
    ASSERT_FALSE(loaded.at("pass_url").get<bool>());
    restarted.set<json>("dice/ai/vision", aivision::normalizeSettings({{"enabled", false}}, loaded));
    ASSERT_TRUE(restarted.save());
    ASSERT_TRUE(cfg.reload());
    ASSERT_FALSE(cfg.get<bool>("dice/ai/vision/pass_url", true));
}
