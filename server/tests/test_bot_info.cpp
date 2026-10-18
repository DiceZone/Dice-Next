#include "test_framework.h"
#include "legacy_import_test_support.h"
#include "common/bot_message.h"
#include "common/version.h"
#include "core/command_router.h"
#include "storage/legacy_import_v2.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
using namespace dice;

struct BotInfoDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("dicenext-bot-info-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    BotInfoDirectory() {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("cannot create bot info fixture");
    }
    ~BotInfoDirectory() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

struct BotInfoFixture {
    BotInfoDirectory dir; // Only synthetic state; no adapter or listening server.
    Database db;
    ConfigManager cfg{u8str(dir.root / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck decks;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, decks, adapters};
    Message message;
    BotInfoFixture() {
        if (!db.open(u8str(dir.root / "app.db"))) throw std::runtime_error("cannot open bot info database");
        cfg.resetDefault();
        message.platform = "qq_official"; message.adapterId = "bot-info-account";
        message.senderId = "player"; message.senderName = "测试玩家";
        message.targetId = "group"; message.selfId = "bot"; message.type = MessageType::kGroup;
        message.extra = {{"role", "admin"}};
        message.content = ".bot";
    }
    std::string run(Locale forced = Locale::kZhHans) { return router.handleMessage(message, forced); }
    void legacy(const json& text) {
        std::filesystem::create_directories(dir.root / "old-conf");
        std::ofstream out(dir.root / "old-conf" / "CustomMsg.json", std::ios::binary); out << text.dump();
    }
    int importMessages(int& orphans, json& details, const legacyv2::ImportOptions& opts = {}) {
        return legacyv2::importCustomMsg(db, i18n, dir.root / "old-conf", orphans, &details, opts, &cfg);
    }
};

std::vector<std::string> botInfoLines(const std::string& text) {
    std::istringstream stream(text); std::vector<std::string> lines; std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}
} // namespace

TEST(BotInfo, AuthorUsesPersistedGlobalReplyLanguageNotMessageOrBrowserLocale) {
    BotInfoFixture f;
    for (const auto& locale : {"zh-Hans", "zh-Hant", "zh-CN", "zh-TW"}) {
        f.cfg.set<std::string>("i18n/default_locale", locale);
        ASSERT_TRUE(f.run(Locale::kEn).rfind("Dice!Next By DiceZone/希亚 Ver ", 0) == 0);
    }
    for (const auto& locale : {"en", "ja"}) {
        f.cfg.set<std::string>("i18n/default_locale", locale);
        ASSERT_TRUE(f.run().rfind("Dice!Next By DiceZone/ShiaNyaa Ver ", 0) == 0);
    }
}

TEST(BotInfo, CompatibilityTracksActualGlobalModesAndCustomEngineOrder) {
    BotInfoFixture f;
    ASSERT_EQ(botInfoLines(f.run()).at(2), "OneDice V1 & DiceScript Compatible");
    f.cfg.set<std::string>("dice/expression_mode", "compatible");
    ASSERT_EQ(botInfoLines(f.run()).at(2), "OneDice V1 Compatible");
    f.cfg.set<std::string>("dice/expression_mode", "original");
    ASSERT_EQ(botInfoLines(f.run()).at(2), "Dice!Next Native");
    f.cfg.set<std::string>("dice/expression_mode", "custom");
    f.cfg.set<json>("dice/expression_order", {"dicenext", "dicescript"});
    ASSERT_EQ(botInfoLines(f.run()).at(2), "DiceScript Compatible");
    f.cfg.set<json>("dice/expression_order", {"dicescript", "dicenext", "onedice"});
    ASSERT_EQ(botInfoLines(f.run()).at(2), "DiceScript & OneDice V1 Compatible");
    f.cfg.set<json>("dice/expression_order", {"onedice"});
    ASSERT_EQ(botInfoLines(f.run()).at(2), "OneDice V1 Compatible");
}

TEST(BotInfo, CompilerVersionTimestampAndAdapterAreFromRunningProgram) {
    BotInfoFixture f;
    const auto lines = botInfoLines(f.run());
    ASSERT_EQ(lines.size(), size_t(6));
    ASSERT_TRUE(lines[0].find(" Ver " + versionString() + "(") != std::string::npos);
    ASSERT_EQ(lines[1], "[" + compilerString() + " " + buildTime() + " For Adapter/qq_official]");
    f.message.platform = "onebot_v11";
    ASSERT_EQ(botInfoLines(f.run())[1], "[" + compilerString() + " " + buildTime() + " For Adapter/OnebotV11]");
}

TEST(BotInfo, OwnerTextAndLegacyHeaderCannotReplaceOrPrecedeFixedThreeLines) {
    BotInfoFixture f;
    const auto banner = botBanner(f.message.platform);
    f.cfg.set<std::string>("dice/bot_header", "旧页眉\n保留联系方式");
    f.cfg.set<std::string>("dice/bot_msg", "{self} 的自定义介绍\n第二行");
    ASSERT_EQ(f.run(), banner + "\n旧页眉\n保留联系方式\n{self} 的自定义介绍\n第二行");
}

TEST(BotInfo, DefaultAppendixIsAvailableToNewAndExistingConfigsAndCanBeCleared) {
    BotInfoFixture f;
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_msg"), kDefaultBotMessage);
    ASSERT_EQ(f.run(), botBanner(f.message.platform) + "\n" + kDefaultBotMessage);
    f.cfg.erase("dice/bot_msg"); // An older split dice.json may omit this key.
    ASSERT_EQ(f.run(), botBanner(f.message.platform) + "\n" + kDefaultBotMessage);
    f.cfg.set<std::string>("dice/bot_msg", "");
    ASSERT_EQ(f.run(), botBanner(f.message.platform));
}

TEST(BotInfo, EngineModeAndAppendixResolveAccountThenPlatformThenGlobal) {
    BotInfoFixture f;
    f.cfg.set<std::string>("dice/bot_msg", "全局说明");
    scoped_settings::setSection(f.cfg, "adapter", "qq_official", "dice",
        {{"expression_mode", "compatible"}, {"bot_msg", "平台说明"}});
    scoped_settings::setSection(f.cfg, "account", f.message.adapterId, "dice",
        {{"expression_mode", "original"}, {"bot_msg", "账号说明"}});
    ASSERT_EQ(botInfoLines(f.run()).at(2), "Dice!Next Native");
    ASSERT_EQ(botInfoLines(f.run()).back(), "账号说明");
    scoped_settings::setSection(f.cfg, "account", f.message.adapterId, "dice", {{"bot_msg", ""}});
    ASSERT_EQ(botInfoLines(f.run()).size(), size_t(3));
    scoped_settings::eraseTarget(f.cfg, "account", f.message.adapterId);
    ASSERT_EQ(botInfoLines(f.run()).at(2), "OneDice V1 Compatible");
    ASSERT_EQ(botInfoLines(f.run()).back(), "平台说明");
    f.message.adapterId = "another-account"; f.message.platform = "milky";
    ASSERT_EQ(botInfoLines(f.run()).at(2), "OneDice V1 & DiceScript Compatible");
    ASSERT_EQ(botInfoLines(f.run()).back(), "全局说明");
}

TEST(BotInfo, ReplyLanguageAndCustomAppendicesSurviveConfigurationReload) {
    BotInfoFixture f;
    f.cfg.set<std::string>("i18n/default_locale", "en");
    f.cfg.set<std::string>("dice/bot_msg", "Owner notice\n{self}");
    f.cfg.set<std::string>("dice/bot_header", "Imported introduction");
    ASSERT_TRUE(f.cfg.save());
    ConfigManager restored(u8str(f.dir.root / "config"));
    ASSERT_TRUE(restored.load());
    ASSERT_EQ(restored.get<std::string>("i18n/default_locale"), "en");
    ASSERT_EQ(restored.get<std::string>("dice/bot_msg"), "Owner notice\n{self}");
    ASSERT_EQ(restored.get<std::string>("dice/bot_header"), "Imported introduction");
    ASSERT_TRUE(f.cfg.load());
    ASSERT_TRUE(f.run().rfind("Dice!Next By DiceZone/ShiaNyaa Ver ", 0) == 0);
    ASSERT_TRUE(f.run().ends_with("Imported introduction\nOwner notice\n{self}"));
}

TEST(BotInfo, ImportDefaultIsNormalizedDeduplicatedAndAlwaysAtTheBottom) {
    const std::string defaults = kDefaultBotMessage;
    ASSERT_EQ(appendDefaultBotMessage(""), defaults);
    ASSERT_EQ(appendDefaultBotMessage("\r\n原说明\r\n"), "原说明\n" + defaults);
    ASSERT_EQ(appendDefaultBotMessage(defaults + "\n原说明\n" + defaults), "原说明\n" + defaults);
    ASSERT_EQ(appendDefaultBotMessage(appendDefaultBotMessage("原说明")), "原说明\n" + defaults);
    std::string crlf;
    for (const char c : defaults) crlf += c == '\n' ? "\r\n" : std::string(1, c);
    ASSERT_EQ(appendDefaultBotMessage("原说明\r\n" + crlf), "原说明\n" + defaults);
    // A default-looking substring embedded inside an owner's line is not deleted.
    ASSERT_EQ(appendDefaultBotMessage("前缀" + defaults + "后缀"), "前缀" + defaults + "后缀\n" + defaults);
}

TEST(BotInfo, LegacyBotMsgAndHeaderBecomeActiveSettingsAndReimportDoesNotDuplicate) {
    BotInfoFixture f;
    f.legacy({{"strBotHeader", "原版页眉"}, {"strBotMsg", "\n旧版介绍：{strSelfName}\r\n旧版群号：123456"}});
    int orphans = 0; json details = json::array();
    ASSERT_EQ(f.importMessages(orphans, details), 2); ASSERT_EQ(orphans, 0);
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_header"), "原版页眉");
    const std::string expected = "旧版介绍：{self}\n旧版群号：123456\n" + std::string(kDefaultBotMessage);
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_msg"), expected);
    ASSERT_EQ(f.run(), botBanner(f.message.platform) + "\n原版页眉\n" + expected);
    ASSERT_EQ(details.at(0).at("status"), "active"); ASSERT_EQ(details.at(1).at("status"), "active");
    ASSERT_TRUE(f.db.getStorage()->get_all<I18nOverrideRow>().empty());
    ASSERT_EQ(f.importMessages(orphans, details), 2); ASSERT_EQ(orphans, 0);
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_msg"), expected);
    ConfigManager restored(u8str(f.dir.root / "config")); ASSERT_TRUE(restored.load());
    ASSERT_EQ(restored.get<std::string>("dice/bot_msg"), expected);
}

TEST(BotInfo, LegacyImportPreservesOwnerChangesUnlessOverwriteIsExplicit) {
    BotInfoFixture f;
    f.cfg.set<std::string>("dice/bot_msg", "Next 现有说明");
    f.cfg.set<std::string>("dice/bot_header", "Next 现有附言");
    f.legacy({{"strBotMsg", "旧版说明"}, {"strBotHeader", "旧版页眉"}});
    int orphans = 0; json details = json::array();
    ASSERT_EQ(f.importMessages(orphans, details), 0); ASSERT_EQ(orphans, 2);
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_msg"), "Next 现有说明");
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_header"), "Next 现有附言");
    ASSERT_EQ(details.at(0).at("status"), "conflict"); ASSERT_EQ(details.at(1).at("status"), "conflict");
    ASSERT_EQ(f.db.getStorage()->get_all<I18nOverrideRow>().size(), size_t(2));
    legacyv2::ImportOptions overwrite; overwrite.overwriteData = true;
    ASSERT_EQ(f.importMessages(orphans, details, overwrite), 2);
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_msg"), "旧版说明\n" + std::string(kDefaultBotMessage));
    ASSERT_EQ(f.cfg.get<std::string>("dice/bot_header"), "旧版页眉");
}
