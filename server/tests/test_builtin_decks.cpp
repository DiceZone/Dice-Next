#include "test_framework.h"
#include "../src/core/deck/card_deck.h"
#include "../src/core/deck/deck_file_utils.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path bundledDeckDir() {
    return fs::path(__FILE__).parent_path().parent_path() /
           "resources" / "default-data" / "decks";
}

}  // namespace

TEST(BundledDecks, LegacyCollectionIsCompleteAndLoadable) {
    const fs::path deckDir = bundledDeckDir();
    const fs::path collection = deckDir / "legacy-builtins.json";
    ASSERT_TRUE(fs::is_regular_file(collection));

    std::ifstream input(collection, std::ios::binary);
    ASSERT_TRUE(input.good());
    json document;
    input >> document;
    ASSERT_TRUE(document.is_object());
    ASSERT_EQ(document.size(), static_cast<size_t>(69));

    const std::vector<std::string> required{
        "数字", "大写字母", "小写字母", "大写英文", "小写英文",
        "天干", "地支", "硬币", "扑克牌", "麻将牌", "性别",
        "塔罗牌", "塔罗牌占卜", "调查员信息"
    };
    for (const auto& name : required) {
        ASSERT_TRUE(document.contains(name));
        ASSERT_TRUE(document.at(name).is_array());
        ASSERT_FALSE(document.at(name).empty());
        for (const auto& card : document.at(name)) ASSERT_TRUE(card.is_string());
    }

    dice::CardDeck decks;
    ASSERT_TRUE(decks.loadDir(deckDir.string()) >= 69);
    for (const auto& name : required) ASSERT_TRUE(decks.has(name));
    ASSERT_EQ(decks.getSourceFile("硬币"), std::string("legacy-builtins.json"));
    auto source = decks.getSourceInfo("硬币");
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(source->bundled);

    const std::set<std::string> coinFaces{"正", "反"};
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto result = decks.drawFromDeck("硬币");
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(coinFaces.contains(*result));
    }

    auto investigator = decks.drawFromDeck("调查员信息");
    ASSERT_TRUE(investigator.has_value());
    ASSERT_FALSE(investigator->empty());
    ASSERT_EQ(investigator->find('{'), std::string::npos);
}

TEST(BundledDecks, DeckFilenamesStayInsideFlatJsonDirectory) {
    using dice::deck_files::isSafeJsonFilename;
    ASSERT_TRUE(isSafeJsonFilename("my-deck.json"));
    ASSERT_TRUE(isSafeJsonFilename("中文牌堆.json"));
    ASSERT_FALSE(isSafeJsonFilename("../config/server.json"));
    ASSERT_FALSE(isSafeJsonFilename("..\\config\\server.json"));
    ASSERT_FALSE(isSafeJsonFilename("folder/deck.json"));
    ASSERT_FALSE(isSafeJsonFilename("deck.JSON"));
    ASSERT_FALSE(isSafeJsonFilename("deck.json.bak"));
    ASSERT_FALSE(isSafeJsonFilename("CON.json"));
    ASSERT_FALSE(isSafeJsonFilename(".json"));
    ASSERT_FALSE(isSafeJsonFilename("bad:name.json"));
}

TEST(BundledDecks, ReloadAppliesUserOverridesAndDropsRemovedEntries) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("dicenext-deck-reload-" + std::to_string(nonce));
    const fs::path bundled = root / "release" / "decks";
    const fs::path users = root / "data" / "decks";
    fs::create_directories(bundled);
    fs::create_directories(users);

    {
        std::ofstream out(bundled / "base.json", std::ios::binary);
        out << R"({"共享":["内置"],"仅内置":["保留"]})";
    }
    {
        std::ofstream out(users / "override.json", std::ios::binary);
        out << R"({"共享":["用户"],"仅用户":["临时"]})";
    }

    dice::CardDeck decks;
    decks.reload({bundled.string(), users.string()});
    auto sharedSource = decks.getSourceInfo("共享");
    ASSERT_TRUE(sharedSource.has_value());
    ASSERT_FALSE(sharedSource->bundled);
    ASSERT_EQ(decks.drawFromDeck("共享").value_or(""), std::string("用户"));
    ASSERT_TRUE(decks.has("仅用户"));

    ASSERT_TRUE(fs::remove(users / "override.json"));
    decks.reload({bundled.string(), users.string()});
    sharedSource = decks.getSourceInfo("共享");
    ASSERT_TRUE(sharedSource.has_value());
    ASSERT_TRUE(sharedSource->bundled);
    ASSERT_EQ(decks.drawFromDeck("共享").value_or(""), std::string("内置"));
    ASSERT_FALSE(decks.has("仅用户"));

    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
}
