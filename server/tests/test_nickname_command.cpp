#include "test_framework.h"
#include "core/command_router.h"
#include "storage/group_account_settings.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct NicknameTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-nickname-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    NicknameTestDir() {
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("cannot create nickname test directory");
    }
    ~NicknameTestDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct NicknameFixture {
    NicknameTestDir dir;
    Database db;
    ConfigManager cfg{u8str(dir.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db}; // No adapters, sockets or external accounts.
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;

    NicknameFixture() {
        if (!db.open(u8str(dir.path / "test.db")))
            throw std::runtime_error("cannot open nickname test database");
        cfg.resetDefault();
        cfg.set<bool>("dice/auto_card", false);
        if (!i18n.load()) throw std::runtime_error("cannot load nickname test locales");
        // Keep names and rolled details observable without Markdown escaping.
        i18n.setOverride(Locale::kZhHans, "dice.roll.result", "{nick}|{res}");
        msg.platform = "onebot_v11";
        msg.adapterId = "nickname-test";
        msg.selfId = "9000";
        msg.senderId = "1000";
        msg.senderName = "平台昵称";
        msg.targetId = "2000";
        msg.type = MessageType::kGroup;
        msg.extra = {{"role", "admin"}};
        setAccountGroupSetting(*db.getStorage(), msg.adapterId, msg.platform,
            msg.targetId, msg.targetId, "logTimerOff", "1");
    }

    std::string run(const std::string& command, Locale locale = Locale::kZhHans) {
        msg.content = command;
        return router.handleMessage(msg, locale);
    }
    void nick(const std::string& scope, const std::string& name) {
        UserSettingRow row;
        row.userId = msg.senderId;
        row.groupId = scope;
        row.key = "nick";
        row.value = name;
        db.getStorage()->insert(row);
    }
    void placeholder() { cards.setValueByName(msg.senderId, "", "__Name", "角色卡"); }
};

} // namespace

TEST(NicknameCommand, DefaultPlaceholderDoesNotHideNnOrReappearAfterUntag) {
    NicknameFixture f;
    f.placeholder(); // Same representation as an imported default card.
    const auto initialRoll = f.run("。r");
    ASSERT_TRUE(initialRoll.rfind("<平台昵称>|", 0) == 0);
    const auto renamed = f.run(".nn随春生");
    ASSERT_EQ(renamed, "已将「平台昵称」改称为「随春生」");
    const auto empty = f.run(".st show");
    ASSERT_EQ(empty, "<随春生> 没有可展示的人物卡属性，请用 .st 录入属性");
    const auto nickRoll = f.run("。r");
    ASSERT_TRUE(nickRoll.rfind("<随春生>|", 0) == 0);

    const auto created = f.run(".pc new 随春生");
    ASSERT_EQ(created, "随春生 已新建并绑定人物卡：随春生");
    ASSERT_EQ(f.cards.boundCard("1000", "2000"), "随春生");
    const auto boundRoll = f.run("。r");
    ASSERT_TRUE(boundRoll.rfind("<随春生>|", 0) == 0);
    const auto untagged = f.run(".pc untag");
    ASSERT_TRUE(untagged.find("已解除绑定人物卡：随春生") != std::string::npos);
    ASSERT_EQ(f.cards.boundCard("1000", "2000"), "");
    ASSERT_TRUE(f.cards.cardExists("1000", "随春生"));
    const auto list = f.run(".pc list");
    ASSERT_TRUE(list.find("★默认卡(0)") != std::string::npos);
    const auto unboundRoll = f.run("。r");
    ASSERT_TRUE(unboundRoll.rfind("<随春生>|", 0) == 0);
    // Fix name resolution, not the default card's key or global metadata.
    ASSERT_EQ(f.cards.displayName("1000", ""), "角色卡");
    ASSERT_EQ(f.cards.listCards("1000").size(), size_t(2));
}

TEST(NicknameCommand, PlaceholderFallsBackThroughLocalGlobalGroupAndPlatformNames) {
    NicknameFixture f;
    f.placeholder();
    auto reply = f.run(".r d1");
    ASSERT_EQ(reply, "<平台昵称>|D1=1");
    f.msg.extra["card"] = "群名片";
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<群名片>|D1=1");
    f.nick("", "全局昵称");
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<全局昵称>|D1=1");
    f.nick("2000", "本群昵称");
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<本群昵称>|D1=1");
    const auto cleared = f.run(".nn");
    ASSERT_FALSE(cleared.empty());
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<全局昵称>|D1=1");
    f.msg.targetId = "3000";
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<全局昵称>|D1=1");
    ASSERT_EQ(f.cards.displayName("1000", ""), "角色卡");
}

TEST(NicknameCommand, RealDefaultCardNameRemainsHigherPriorityThanNickname) {
    NicknameFixture f;
    f.cards.setValueByName("1000", "", "__Name", "褚楚");
    f.cards.setAttrByName("1000", "", "侦查", 50);
    f.run(".nn 随春生");
    const auto reply = f.run(".r d1");
    ASSERT_EQ(reply, "<褚楚>|D1=1");
    ASSERT_EQ(f.cards.displayName("1000", ""), "褚楚");
    ASSERT_EQ(f.cards.getAttrByName("1000", "", "侦查").value_or(-1), 50);
}

TEST(NicknameCommand, NamedCardKeepsPriorityAndReturnsToNicknameWhenUnbound) {
    NicknameFixture f;
    f.placeholder();
    f.run(".nn 随春生");
    f.run(".pc new 观月涉");
    f.cards.setAttr("1000", "2000", "侦查", 50);
    auto reply = f.run(".r d1");
    ASSERT_EQ(reply, "<观月涉>|D1=1");
    f.run(".pc untag");
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<随春生>|D1=1");
    ASSERT_EQ(f.cards.getAttrByName("1000", "观月涉", "侦查").value_or(-1), 50);
}

TEST(NicknameCommand, NnStillRenamesNamedCardsWithoutLosingAttributesOrBindings) {
    NicknameFixture f;
    f.run(".pc new 调查员");
    f.cards.setAttr("1000", "2000", "侦查", 50);
    f.cards.bindCard("1000", "3000", "调查员");
    const auto reply = f.run(".nn随春生");
    ASSERT_EQ(reply, "已将「调查员」改称为「随春生」");
    ASSERT_EQ(f.cards.boundCard("1000", "2000"), "随春生");
    ASSERT_EQ(f.cards.boundCard("1000", "3000"), "随春生");
    ASSERT_FALSE(f.cards.cardExists("1000", "调查员"));
    ASSERT_EQ(f.cards.getAttrByName("1000", "随春生", "侦查").value_or(-1), 50);
}

TEST(NicknameCommand, ExplicitPlaceholderNamedCardAlsoFallsBackAndCanStillBeRenamed) {
    NicknameFixture f;
    f.run(".pc new 角色卡");
    f.nick("2000", "本群昵称");
    auto reply = f.run(".r d1");
    ASSERT_EQ(reply, "<本群昵称>|D1=1");
    f.run(".nn随春生");
    ASSERT_EQ(f.cards.boundCard("1000", "2000"), "随春生");
    reply = f.run(".r d1");
    ASSERT_EQ(reply, "<随春生>|D1=1");
}

TEST(NicknameCommand, RollAndCheckFamiliesUseTheSamePlaceholderFallback) {
    NicknameFixture f;
    f.placeholder();
    f.run(".nn随春生");
    for (const auto* command : {".r d1", ".ra 侦查50", ".rb 侦查50", ".rp 侦查50",
                                ".sc 0/0 60", ".en 侦查50"}) {
        const auto reply = f.run(command);
        ASSERT_TRUE(reply.find("<随春生") != std::string::npos);
        ASSERT_TRUE(reply.find("角色卡") == std::string::npos);
    }
}

TEST(NicknameCommand, EmptyAttributesMessageDoesNotClaimTheCardIsMissing) {
    NicknameFixture f;
    f.placeholder();
    f.run(".nn随春生");
    // Reserved legacy fields may be counted in .pc list but are not attributes
    // that .st show can display. The card and its metadata still exist.
    for (int i = 0; i < 6; ++i)
        f.cards.setValueByName("1000", "", "__legacy_" + std::to_string(i), i);
    ASSERT_EQ(f.cards.attrCount("1000", ""), 6);
    const auto reply = f.run(".st show");
    ASSERT_EQ(reply, "<随春生> 没有可展示的人物卡属性，请用 .st 录入属性");
    ASSERT_TRUE(f.cards.cardExists("1000", ""));
    for (Locale locale : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa}) {
        const auto localized = f.run(".st show", locale);
        const auto expected = f.i18n.tr(locale, "card.empty", {{"nick", "<随春生>"}});
        ASSERT_EQ(localized, expected);
    }
}

TEST(CharacterCardShow, BareShowReadsMixedTypedAttributesRepeatedlyWithoutChangingTheCard) {
    NicknameFixture f;
    const std::string longKey = "自定义_" + std::string(96, 'x');
    const std::string background = std::string(512, 'b') + "调查员背景";
    f.cards.replaceValuesByName("1000", "",
                                {{"力量", 60},
                                 {"生命值", 12},
                                 {"职业", "记者"},
                                 {"小数", 1.75},
                                 {"受伤", true},
                                 {"道具", json::array({"手电", 2})},
                                 {"标签", {{"来源", "旧版"}}},
                                 {"备注", nullptr},
                                 {"&伤害", "3+4"},
                                 {longKey, "长属性名"}});
    f.cards.setTextByName("1000", "", "背景", background);
    const auto before = f.cards.getValuesByName("1000", "");
    for (const auto locale : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa}) {
        f.i18n.setOverride(locale, "card.show", "{nick}|{detail}", ContentFormat::kPlainText);
        for (const auto type : {MessageType::kPrivate, MessageType::kGroup}) {
            f.msg.type = type;
            f.msg.targetId = type == MessageType::kPrivate ? "1000" : "2000";
            for (int i = 0; i < 12; ++i) {
                const auto reply = f.run(".st show", locale);
                ASSERT_TRUE(reply.find("力量:60") != std::string::npos);
                ASSERT_TRUE(reply.find(CharacterCardStore::canonical("伤害") + "=3+4(=7)") !=
                            std::string::npos);
                for (const auto *name : {"职业", "小数", "受伤", "道具", "标签", "备注", "背景"})
                    ASSERT_TRUE(reply.find(std::string(name) + "=" + before.at(name).dump()) !=
                                std::string::npos);
                ASSERT_TRUE(reply.find(longKey + "=\"长属性名\"") != std::string::npos);
                ASSERT_EQ(f.cards.getValuesByName("1000", ""), before);
            }
        }
    }
}

TEST(CharacterCardShow, BoundGroupDefaultPrivateAndOfficialCardViewsStayIsolated) {
    NicknameFixture f;
    f.cards.setValueByName("1000", "", "职业", "默认卡职业");
    ASSERT_TRUE(f.cards.createCard("1000", "群人物卡"));
    f.cards.setValueByName("1000", "群人物卡", "职业", "群卡职业");
    f.cards.bindCard("1000", "2000", "群人物卡");
    f.i18n.setOverride(Locale::kZhHans, "card.show", "**{nick}**：{detail}", ContentFormat::kMarkdown);
    for (const auto *platform : {"onebot_v11", "qq_official"}) {
        f.msg.platform = platform;
        for (const auto type : {MessageType::kGroup, MessageType::kPrivate}) {
            f.msg.type = type;
            f.msg.targetId = type == MessageType::kGroup ? "2000" : "1000";
            const auto rendered = I18n::captureFragment([&] { return f.run(".st show"); });
            const auto plain = rendered.format == ContentFormat::kMarkdown
                                   ? markdown::toPlainText(rendered.text)
                                   : rendered.text;
            const auto expected = type == MessageType::kGroup ? "群卡职业" : "默认卡职业";
            const auto absent = type == MessageType::kGroup ? "默认卡职业" : "群卡职业";
            ASSERT_TRUE(plain.find(expected) != std::string::npos);
            ASSERT_TRUE(plain.find(absent) == std::string::npos);
            if (f.msg.platform == "qq_official") {
                const auto official = f.router.lastOfficialCard();
                ASSERT_TRUE(official != nullptr);
                ASSERT_EQ(official->original, rendered.text);
            } else
                ASSERT_TRUE(f.router.lastOfficialCard() == nullptr);
            ASSERT_EQ(f.cards.boundCard("1000", "2000"), "群人物卡");
            ASSERT_EQ(f.cards.getValueByName("1000", "", "职业").value(), json("默认卡职业"));
            ASSERT_EQ(f.cards.getValueByName("1000", "群人物卡", "职业").value(), json("群卡职业"));
        }
    }
}

TEST(CharacterCardShow, EmptyLegacyTextNamesAndReadLocksRemainSafe) {
    NicknameFixture f;
    f.i18n.setOverride(Locale::kZhHans, "card.show", "{detail}", ContentFormat::kPlainText);
    // Older/plugin-owned metadata can contain an empty text key. It is not a
    // displayable attribute and must never reach string::front().
    f.cards.setTextByName("1000", "", "", "invalid-empty-field");
    const auto empty = f.run(".st show");
    ASSERT_TRUE(empty.find("invalid-empty-field") == std::string::npos);
    f.cards.setValueByName("1000", "", "职业", "记者");
    ASSERT_TRUE(f.cards.lockCard("1000", "2000", "r"));
    const auto locked = f.i18n.tr(Locale::kZhHans, "card.locked_r");
    for (const auto *command : {".st show", ".st show 职业", ".st show 职业 力量", ".st show 50"})
        ASSERT_EQ(f.run(command), locked);
    ASSERT_TRUE(f.cards.unlockCard("1000", "2000", "r"));
    ASSERT_TRUE(f.run(".st show").find("职业=\"记者\"") != std::string::npos);
    ASSERT_TRUE(f.run(".st show 职业").find("记者") != std::string::npos);
}
