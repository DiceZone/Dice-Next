#include "test_framework.h"
#include "../src/core/deck/card_deck.h"

#include <filesystem>
#include <fstream>
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
