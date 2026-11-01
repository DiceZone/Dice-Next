#include "test_framework.h"
#include "core/command_router.h"
#include "core/playground_reply.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct PlaygroundTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-playground-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    PlaygroundTestDir() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create test directory");
    }
    ~PlaygroundTestDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// The presentation style is process-wide; restore it for later tests.
struct StyleScope {
    PresentationStyle previous = IAdapter::presentationStyle();
    explicit StyleScope(PresentationStyle style) { IAdapter::setPresentationStyle(style); }
    ~StyleScope() { IAdapter::setPresentationStyle(previous); }
};

// No adapter is registered, exactly like the playground with no bot connected.
struct PlaygroundFixture {
    PlaygroundTestDir dir;
    Database db;
    ConfigManager cfg{"unused-playground-test-config"};
    DiceEngine engine{cfg};
    I18n i18n{(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n").string(), Locale::kZhHans};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;

    PlaygroundFixture() {
        if (!db.open(u8str(dir.path / "test.db"))) throw std::runtime_error("cannot open test database");
        if (!i18n.load()) throw std::runtime_error("cannot load i18n");
        cfg.resetDefault();
        msg.id = "playground";
        msg.senderId = "10001";
        msg.senderName = "希亚";
        msg.targetId = "100001";
        msg.type = MessageType::kGroup;
        msg.extra = json::object();
    }

    // Same order as /api/test/message: capture the outbound format, then render.
    json send(const std::string& platform, const std::string& text, PresentationStyle style) {
        msg.platform = platform;
        msg.content = msg.rawContent = msg.displayContent = text;
        const auto preferred = playground::defaultReplyFormat(platform, style);
        I18n::beginOutboundCapture(preferred, style);
        const auto reply = router.handleMessage(msg, Locale::kZhHans);
        const auto format = I18n::endOutboundCapture();
        if (reply.empty()) return json();
        return playground::render(reply, format, platform, msg.type, style,
            preferred == ContentFormat::kPlainText);
    }
};

bool hasMarkdownSyntax(const std::string& text) {
    return text.find("**") != std::string::npos || text.find('`') != std::string::npos
        || text.find('\\') != std::string::npos;
}

} // namespace

TEST(Playground, PlainPlatformsShowTheTextActuallySent) {
    const auto shown = playground::render("**<测试员\\>** 掷骰：`3D6+2=(1+3+1)+2=5+2=7`",
        ContentFormat::kMarkdown, "onebot_v11", MessageType::kGroup, PresentationStyle::kStandard, true);
    ASSERT_EQ(shown["text"].get<std::string>(), std::string("<测试员> 掷骰：3D6+2=(1+3+1)+2=5+2=7"));
    ASSERT_FALSE(shown["markdown"].get<bool>());
    ASSERT_EQ(playground::previewPlatform("milky", MessageType::kPrivate), std::string("plain"));
    ASSERT_EQ(playground::previewPlatform("qq_official", MessageType::kPrivate), std::string("qq_private"));
}

TEST(Playground, OneBotRollReplyIsPlainTextInStandardStyle) {
    StyleScope style(PresentationStyle::kStandard);
    PlaygroundFixture f;
    const auto shown = f.send("onebot_v11", ".r 3d6+2", PresentationStyle::kStandard);
    ASSERT_TRUE(shown.is_object());
    const auto text = shown["text"].get<std::string>();
    ASSERT_TRUE(text.find("希亚") != std::string::npos);
    ASSERT_TRUE(text.find("3D6+2=") != std::string::npos);
    ASSERT_FALSE(hasMarkdownSyntax(text));
    ASSERT_FALSE(shown["markdown"].get<bool>());
}

TEST(Playground, DiscordAndKookKeepTheirRichText) {
    StyleScope style(PresentationStyle::kStandard);
    PlaygroundFixture f;
    for (const std::string platform : {"discord", "kook"}) {
        const auto shown = f.send(platform, ".r 3d6+2", PresentationStyle::kStandard);
        ASSERT_TRUE(shown.is_object());
        ASSERT_TRUE(shown["markdown"].get<bool>());
        ASSERT_TRUE(shown["text"].get<std::string>().find("**") != std::string::npos);
        ASSERT_FALSE(hasMarkdownSyntax(shown["plain"].get<std::string>()));
    }
    // Traditional style sends plain text to every platform.
    const auto plain = f.send("discord", ".r 3d6+2", PresentationStyle::kTraditional);
    ASSERT_FALSE(plain["markdown"].get<bool>());
    ASSERT_FALSE(hasMarkdownSyntax(plain["text"].get<std::string>()));
}

TEST(Playground, QQOfficialMessageWithoutExtraDoesNotThrow) {
    StyleScope style(PresentationStyle::kStandard);
    PlaygroundFixture f;
    f.msg.extra = json();   // a synthetic message carries no adapter metadata
    json shown;
    bool threw = false;
    try { shown = f.send("qq_official", ".r 3d6+2", PresentationStyle::kStandard); }
    catch (const std::exception& e) { threw = true; std::cerr << "  threw: " << e.what() << "\n"; }
    ASSERT_FALSE(threw);
    ASSERT_TRUE(shown.is_object());
    ASSERT_FALSE(shown["markdown"].get<bool>());
    ASSERT_FALSE(hasMarkdownSyntax(shown["text"].get<std::string>()));
    f.msg.type = MessageType::kPrivate;
    f.msg.targetId = f.msg.senderId;
    try { shown = f.send("qq_official", ".r 3d6+2", PresentationStyle::kStandard); }
    catch (const std::exception& e) { threw = true; std::cerr << "  threw: " << e.what() << "\n"; }
    ASSERT_FALSE(threw);
    ASSERT_TRUE(shown.is_object());
}
