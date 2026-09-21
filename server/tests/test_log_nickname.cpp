// 日志里存的是名字本身，不是回复里那个带包裹的显示形式。
//
// # 这条用例是从一次真实故障来的
//
// 骰子有两个取显示名的函数：displayName 会套上 dice/nick_prefix|suffix（默认
// 尖括号），给「<希亚> 掷出了 27」这种回复文案用；displayNameRaw 不套，给存储
// 和比较用。日志写入误用了前者，于是存进去、并且上传到日志站的名字是「<希亚>」。
//
// 后果不是「难看」而已。日志站的染色器解析每一行时，名字那一组明确排除 `<`
// （story-painter 的 EditLogImporter），所以**每一个玩家的行都解析失败**，只剩
// 骰娘自己那行认得出来——用户看到的是「七个角色变成一个角色」。
//
// 所以这里守两件事：存进库的名字不带包裹，上传载荷里的 nickname 也不带。

#include "test_framework.h"
#include "core/command_router.h"
#include "service/log_service.h"
#include "storage/group_account_settings.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct NickTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-log-nick-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    NickTestDir() {
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("cannot create test directory");
    }
    ~NickTestDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct NickFixture {
    NickTestDir dir;
    Database db;
    ConfigManager cfg{u8str(dir.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{"unused-log-nick-test-i18n"};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;

    NickFixture() {
        if (!db.open(u8str(dir.path / "test.db")))
            throw std::runtime_error("cannot open test database");
        cfg.resetDefault();
        msg.platform = "onebot_v11";
        msg.adapterId = "log-nick-test";
        msg.selfId = "9000";
        msg.senderId = "1000";
        msg.senderName = "希亚";
        msg.targetId = "2100";
        msg.type = MessageType::kGroup;
        msg.extra = {{"role", "admin"}};
        // 关掉计时器，否则 .log new 会多回一行「已开始计时」，断言对不上。
        // （注意 ASSERT_EQ 失败时会把左侧再求值一遍，于是第二次 .log new 撞上
        // 已存在的日志，错误信息显示成 log.exists——和真正的原因无关。）
        setting("logTimerOff", "1");
    }
    void setting(const std::string& key, const std::string& value) {
        setAccountGroupSetting(*db.getStorage(), msg.adapterId, msg.platform,
            msg.targetId, msg.targetId, key, value);
    }

    std::string run(const std::string& content) {
        msg.content = content;
        return router.handleMessage(msg, Locale::kZhHans);
    }
    void say(const std::string& content) {
        msg.content = content;
        router.recordIncoming(msg);
    }
    auto messages() {
        return db.getLogStorage()->get_all<GameLogMessageRow>(
            sqlite_orm::order_by(&GameLogMessageRow::id));
    }
    int logId() { return db.getLogStorage()->get_all<GameLogRow>().front().id; }
};

/// 日志站的染色器按行解析，名字那一组不接受以 `<` 开头的。
bool parsableByLogSite(const std::string& nickname) {
    return !nickname.empty() && nickname.front() != '<';
}

} // namespace

TEST(LogNickname, StoresTheNameItselfNotTheWrappedDisplayForm) {
    NickFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.say("大家好");

    auto rows = f.messages();
    ASSERT_EQ(rows.size(), size_t(1));
    ASSERT_EQ(rows.back().sender, "希亚");
    ASSERT_TRUE(parsableByLogSite(rows.back().sender));
}

TEST(LogNickname, TheNicknameSetByNnIsAlsoStoredClean) {
    NickFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.run(".nn 调查员希亚");
    f.say("我来查线索");

    auto rows = f.messages();
    ASSERT_FALSE(rows.empty());
    ASSERT_EQ(rows.back().sender, "调查员希亚");
    ASSERT_TRUE(parsableByLogSite(rows.back().sender));
}

TEST(LogNickname, ChangingTheWrapSymbolsDoesNotLeakIntoStorage) {
    // 包裹符号是骰主可配置的。存储这一侧不该受它影响——换成任何符号，
    // 存进日志的都该是名字本身。
    NickFixture f;
    f.cfg.set<std::string>("dice/nick_prefix", "【");
    f.cfg.set<std::string>("dice/nick_suffix", "】");
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.say("换了包裹符号");

    auto rows = f.messages();
    ASSERT_FALSE(rows.empty());
    ASSERT_EQ(rows.back().sender, "希亚");
}

TEST(LogNickname, TheUploadedPayloadCarriesTheCleanName) {
    // 真正传到日志站去的是这一份。库里干净而载荷里不干净的话，用户看到的
    // 还是坏的——所以这一层单独验。
    NickFixture f;
    ASSERT_EQ(f.run(".log new 测试记录"), "log.new");
    f.say("上传前最后一句");

    json items = logsvc::renderSealItems(f.db, f.logId(), &f.cfg, f.msg.selfId);
    ASSERT_FALSE(items.empty());
    bool foundPlayer = false;
    for (const auto& item : items) {
        const std::string nickname = item.value("nickname", std::string());
        ASSERT_TRUE(parsableByLogSite(nickname));
        if (nickname == "希亚") foundPlayer = true;
    }
    ASSERT_TRUE(foundPlayer);
}
