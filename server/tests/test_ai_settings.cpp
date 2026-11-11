#include "../src/service/ai_settings_patch.h"
#include "../src/service/ai_vision.h"
#include "test_framework.h"
#include "../src/service/ai_vision_config.h"
#include "../src/service/ai_usage.h"
#include "../src/config/config_manager.h"

#include <chrono>
#include <filesystem>
#include <map>

using namespace dice;

namespace {
json legacyAiConfig() {
    return json{
        {"enabled", true},
        {"models", json::array({
            {{"id", "a"}, {"name", "A"}, {"base_url", "https://api.one.example/v1"}, {"api_key", "k1"}, {"model", "m-a"}, {"enabled", true}},
            {{"id", "b"}, {"name", "B"}, {"base_url", "https://api.one.example/v1"}, {"api_key", "k1"}, {"model", "m-b"}, {"enabled", true}},
            {{"id", "c"}, {"name", "C"}, {"base_url", "http://localhost:11434/v1"}, {"api_key", ""}, {"model", "m-c"}, {"enabled", true}}})},
        {"chat", {{"model_id", "b"}}},
        {"polish", {{"model_id", "c"}}},
        {"vision", {{"enabled", true}, {"model_id", "c"}}},
        {"memory", {{"long", {{"embed_model_id", "a"}, {"embed_model", "text-embedding-3-small"}}}}}};
}
struct TempConfig {
    std::filesystem::path root;
    ConfigManager cfg;
    TempConfig() : root(std::filesystem::temp_directory_path() / ("dnx-ai-usage-" + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count()))),
                   cfg(root.string()) { cfg.load(); }
    ~TempConfig() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
}  // namespace

TEST(AiUsage, LegacyModelsMergeIntoProvidersAndUsages) {
    json c = legacyAiConfig();
    ASSERT_TRUE(ai::migrateConfig(c));
    ASSERT_EQ(c["providers"].size(), (size_t)2);              // 相同 URL+Key 合并
    for (auto& m : c["models"]) ASSERT_FALSE(m.contains("api_key"));
    ASSERT_EQ(c["models"][0]["provider_id"], c["models"][1]["provider_id"]);
    ASSERT_EQ(c["usages"]["reply"]["models"][0]["id"].get<std::string>(), "b");
    ASSERT_EQ(c["usages"]["polish"]["models"][0]["id"].get<std::string>(), "c");
    ASSERT_TRUE(c["models"][2]["vision"].get<bool>());
    // 嵌入连接 + 模型名 → 一个嵌入模型，沿用 a 的提供商。
    json emb = c["models"][3];
    ASSERT_EQ(emb["kind"].get<std::string>(), "embedding");
    ASSERT_EQ(emb["model"].get<std::string>(), "text-embedding-3-small");
    ASSERT_EQ(emb["provider_id"], c["models"][0]["provider_id"]);
    ASSERT_EQ(c["usages"]["embed"]["models"][0]["id"].get<std::string>(), emb["id"].get<std::string>());
    ASSERT_FALSE(ai::migrateConfig(c));                        // 已迁移不重复处理
    // 解析后模型带回提供商的连接信息。
    auto ms = ai::modelsOf(c);
    ASSERT_EQ(ms[0].baseUrl, std::string("https://api.one.example/v1"));
    ASSERT_EQ(ms[0].apiKey, std::string("k1"));
}

TEST(AiUsage, NormalizeDropsUnknownModelsWrongKindsAndBadStrategies) {
    json models = json::array({{{"id", "x"}, {"kind", "chat"}}, {{"id", "e1"}, {"kind", "embedding"}},
                               {{"id", "e2"}, {"kind", "embedding"}}});
    json in = {{"reply", {{"models", json::array({{{"id", "x"}, {"weight", 500}}, {{"id", "e1"}}, {{"id", "ghost"}}, {{"id", "x"}}})},
                          {"strategy", "nonsense"}}},
               {"embed", {{"models", json::array({"e1", "e2"})}, {"strategy", "random"}}}};
    json out = ai::normalizeUsages(in, models);
    ASSERT_EQ(out["reply"]["models"].size(), (size_t)1);
    ASSERT_EQ(out["reply"]["models"][0]["weight"].get<int>(), 100);
    ASSERT_EQ(out["reply"]["strategy"].get<std::string>(), "balance");
    ASSERT_EQ(out["embed"]["models"].size(), (size_t)1);       // 嵌入只保留一个
    ASSERT_EQ(out["embed"]["strategy"].get<std::string>(), "priority");
    for (auto& d : ai::usageDefs()) ASSERT_TRUE(out.contains(d.key));
}

TEST(AiUsage, StrategiesOrderCandidates) {
    auto make = [](const std::string& id, int w) { ai::Candidate c; c.model.id = id; c.weight = w; return c; };
    std::vector<ai::Candidate> cs = {make("p1", 1), make("p2", 1), make("p3", 1)};
    auto pri = ai::orderByStrategy(cs, "priority", "t-pri");
    ASSERT_EQ(pri[0].id, std::string("p1"));
    ASSERT_EQ(pri[2].id, std::string("p3"));
    // 负载均衡：处理中的请求最少的排第一；空闲时轮流。
    {
        ai::InflightGuard busy1("p1"), busy2("p2");
        ASSERT_EQ(ai::orderByStrategy(cs, "balance", "t-bal")[0].id, std::string("p3"));
    }
    std::map<std::string, int> firsts;
    for (int i = 0; i < 6; ++i) ++firsts[ai::orderByStrategy(cs, "balance", "t-rr")[0].id];
    ASSERT_EQ(firsts.size(), (size_t)3);
    // 分配权重：权重 99:1 时绝大多数排第一的是重的那个。
    std::vector<ai::Candidate> ws = {make("light", 1), make("heavy", 99)};
    int heavy = 0;
    for (int i = 0; i < 400; ++i) if (ai::orderByStrategy(ws, "weight", "t-w")[0].id == "heavy") ++heavy;
    ASSERT_TRUE(heavy > 340);
    // 每种策略都保留全部候选作为失败时的备选。
    for (auto s : {"priority", "random", "weight", "balance"}) ASSERT_EQ(ai::orderByStrategy(cs, s, "t-all").size(), (size_t)3);
}

TEST(AiUsage, UnassignedUsagesFallBackAlongTheChain) {
    TempConfig t;
    json c = legacyAiConfig();
    ai::migrateConfig(c);
    c["usages"]["reply"] = {{"models", json::array({{{"id", "a"}}})}, {"strategy", "priority"}};
    c["usages"]["mood"] = json::object();
    c["usages"]["npc"] = json::object();
    c["usages"]["speech"] = json::object();
    t.cfg.set<json>("dice/ai", c);
    ASSERT_EQ(ai::pick(t.cfg, "mood")[0].id, std::string("a"));        // mood → npc → reply
    ASSERT_EQ(ai::pick(t.cfg, "mood", "c")[0].id, std::string("c"));   // NPC 专属模型排最前
    ASSERT_TRUE(ai::pick(t.cfg, "speech").empty());                     // 语音识别不回退
    for (auto& m : ai::pick(t.cfg, "vision")) ASSERT_TRUE(m.vision);   // 只选支持图片的模型
    ASSERT_EQ(ai::pick(t.cfg, "embed")[0].kind, std::string("embedding"));
    for (auto& m : ai::pick(t.cfg, "reply")) ASSERT_EQ(m.kind, std::string("chat"));
    // 停用提供商 → 其下的模型都不可用。
    c["providers"][0]["enabled"] = false;
    t.cfg.set<json>("dice/ai", c);
    for (auto& m : ai::pick(t.cfg, "reply")) ASSERT_NE(m.id, std::string("a"));
}

TEST(AiUsage, ReasoningIsSeparatedFromTheReply) {
    std::string content = "<think>先算一下</think>答案是 42";
    ASSERT_EQ(ai::takeReasoning(json::object(), content), std::string("先算一下"));
    ASSERT_EQ(content, std::string("答案是 42"));
    std::string plain = "好的";
    ASSERT_EQ(ai::takeReasoning(json{{"reasoning_content", "思考"}}, plain), std::string("思考"));
    ASSERT_EQ(plain, std::string("好的"));
}

TEST(AiVisionSettings, MissingValuesUseCompatibleDefaults) {
    const auto value = aivision::normalizeSettings(json::object());
    ASSERT_FALSE(value.at("pass_url").get<bool>());
    ASSERT_FALSE(value.at("enabled").get<bool>());
    ASSERT_EQ(value.at("max_images").get<int>(), 2);
    ASSERT_FALSE(value.contains("model_id"));   // 模型改由用途分配决定
}

TEST(AiVisionSettings, BothSwitchStatesSurviveRepeatedApiRoundTrips) {
    for (const bool direct : {false, true}) {
        json value = {{"enabled", true},
                      {"prompt", "describe"}, {"max_images", 3}, {"pass_url", direct}};
        const auto expected = aivision::normalizeSettings(value);
        for (int i = 0; i < 5; ++i) value = aivision::normalizeSettings(value);
        ASSERT_TRUE(value == expected);
    }
}

TEST(AiVisionSettings, OlderClientCannotReenableExplicitlyDisabledDirectUrls) {
    const json previous = {{"pass_url", false}};
    const auto value = aivision::normalizeSettings({{"enabled", true}}, previous);
    ASSERT_FALSE(value.at("pass_url").get<bool>());
    ASSERT_FALSE(aivision::normalizeSettings({{"pass_url", true}}, previous).at("pass_url").get<bool>());
}

TEST(AiVisionSettings, ImageLimitsRemainBoundedAndWrongBooleanTypesAreRejected) {
    ASSERT_EQ(aivision::normalizeSettings({{"max_images", -2}}).at("max_images").get<int>(), 1);
    ASSERT_EQ(aivision::normalizeSettings({{"max_images", 50}}).at("max_images").get<int>(), 4);
    for (const json wrong : {json("false"), json(0), json(nullptr)}) {
        bool rejected = false;
        try { (void)aivision::normalizeSettings({{"enabled", wrong}}); }
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

TEST(AiSettings, PartialSwitchPreservesOtherFieldsAndExplicitFalse) {
    json old = {{"enabled", true}, {"providers", json::array({{{"id", "p1"}, {"api_key", "private"}}})},
        {"chat", {{"enabled", true}, {"prompt", "keep me"}}}, {"memory", {{"short", {{"enabled", true}, {"rounds", 30}}}}}};
    auto next = ai::mergeSettings(old, {{"chat", {{"enabled", false}}}});
    ASSERT_FALSE(next["chat"]["enabled"].get<bool>());
    ASSERT_EQ(next["chat"]["prompt"].get<std::string>(), "keep me");
    ASSERT_TRUE(next["providers"] == old["providers"]);
    next = ai::mergeSettings(next, {{"memory", {{"short", {{"enabled", false}}}}}});
    ASSERT_FALSE(next["memory"]["short"]["enabled"].get<bool>());
    ASSERT_EQ(next["memory"]["short"]["rounds"].get<int>(), 30);
}
TEST(AiVisionSettings, LimitsAndImageIdentity) {
    ASSERT_EQ(aivision::normalizeSettings({})["max_image_kb"].get<int>(), 1024);
    ASSERT_EQ(aivision::normalizeSettings({{"max_image_kb", -1}})["max_image_kb"].get<int>(), 1);
    ASSERT_EQ(aivision::normalizeSettings({{"max_image_kb", 99999}})["max_image_kb"].get<int>(), 8192);
    ASSERT_NE(aivision::visionCacheKey({"https://image.example/download?file=1"}), aivision::visionCacheKey({"https://image.example/download?file=2"}));
    ASSERT_NE(aivision::visionCacheKey({"a|b", "c"}), aivision::visionCacheKey({"a", "b|c"}));
    ASSERT_EQ(aivision::visionCacheKey({"same"}), aivision::visionCacheKey({"same"}));
}
TEST(AiVisionSettings, DetectsMimeFromBytesAndSkipsOversizedFiles) {
    TempConfig temp;
    const auto path = temp.root / "picture.tmp";
    { std::ofstream out(path, std::ios::binary); out << "GIF89a" << std::string(1024, 'x'); }
    std::string error;
    ASSERT_TRUE(aivision::fileToDataUrl(path.string(), 2048, &error).rfind("data:image/gif;base64,", 0) == 0);
    ASSERT_TRUE(aivision::fileToDataUrl(path.string(), 1024, &error).empty());
    ASSERT_FALSE(error.empty());
    { std::ofstream out(path, std::ios::binary); out << "<html>expired image</html>"; }
    ASSERT_TRUE(aivision::fileToDataUrl(path.string(), 2048, &error).empty());
}
TEST(AiSettings, ClaudeDoesNotReceiveConflictingSamplingOptions) {
    json body = {{"temperature", 0.7}, {"top_p", 1}, {"model", "claude"}};
    ai::applyProviderRequestCompatibility("https://api.anthropic.com/v1", body);
    ASSERT_FALSE(body.contains("temperature")); ASSERT_FALSE(body.contains("top_p"));
    ASSERT_EQ(body["model"].get<std::string>(), "claude");
}

TEST(AiSettings, ImagePayloadNeverAppearsInDebugMessages) {
    json messages = json::array({{{"role", "user"}, {"content", json::array({
        {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,PRIVATE_IMAGE_BYTES"}}}},
        {{"type", "text"}, {"text", "What is in this image?"}}
    })}}});
    auto printed = ai::formatMessages(messages);
    ASSERT_TRUE(printed.find("PRIVATE_IMAGE_BYTES") == std::string::npos);
    ASSERT_TRUE(printed.find("What is in this image?") != std::string::npos);
}
