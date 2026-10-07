#include "test_framework.h"
#include "common/check_reply.h"
#include "common/weighted_templates.h"
#include "common/template_preview.h"
#include "core/command_router.h"
#include "storage/group_account_settings.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {
struct OutcomeTempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-outcome-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    OutcomeTempDir() { std::filesystem::create_directory(path); }
    ~OutcomeTempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct OutcomeFixture {
    OutcomeTempDir dir;
    Database db;
    ConfigManager cfg{u8str(dir.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;
    OutcomeFixture() {
        if (!db.open(u8str(dir.path / "test.db"))) throw std::runtime_error("cannot open test database");
        cfg.resetDefault();
        msg.platform = "onebot_v11"; msg.adapterId = "outcome-test"; msg.selfId = "9000";
        msg.senderId = "1000"; msg.senderName = "玩家"; msg.targetId = "2000";
        msg.type = MessageType::kGroup; msg.extra = {{"role", "admin"}};
    }
    std::string run(const std::string& command) {
        msg.content = command;
        return router.handleMessage(msg, Locale::kZhHans);
    }
    void set(const std::string& key, const std::string& value) { i18n.setOverride(Locale::kZhHans, key, value); }
    void family(const std::string& name, const std::string& suffix = "") {
        for (const auto& f : check_reply::families()) if (f.name == name)
            for (const auto& grade : f.grades) set(check_reply::key(name, grade), name + ":" + grade + suffix);
    }
    void setting(const std::string& key, const std::string& value) {
        setAccountGroupSetting(*db.getStorage(), msg.adapterId, msg.platform, msg.targetId, msg.targetId, key, value);
    }
};

class OutcomePrivateAdapter final : public IAdapter {
public:
    std::vector<Message> sent;
    std::string id() const override { return "outcome-test"; }
    std::string name() const override { return id(); }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "test"; }
    bool configure(const json&) override { return true; }
    bool start() override { return true; }
    void stop() override {}
    bool isConnected() const override { return true; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message& m) override { sent.push_back(m); }
    void sendReply(const Message&, const std::string&) override {}
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return "9000"; }
    std::string getLoginName() const override { return "test"; }
    std::string getGroupName(const std::string&) const override { return {}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
};

std::vector<std::string> split(const std::string& text, char delim = '|') {
    std::vector<std::string> out;
    std::istringstream stream(text); std::string part;
    while (std::getline(stream, part, delim)) out.push_back(part);
    return out;
}
}

TEST(OutcomeReplies, RegistryMatchesAllLocalesAndOptionalSlotsStartEmpty) {
    OutcomeFixture f;
    ASSERT_TRUE(f.i18n.load());
    size_t count = 0;
    for (const auto& family : check_reply::families()) for (const auto& grade : family.grades) {
        const auto key = check_reply::key(family.name, grade);
        ASSERT_TRUE(check_reply::familyForKey(key) != nullptr);
        ASSERT_TRUE(check_reply::variables(key).size() >= 9);
        for (const auto loc : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa}) {
            const auto defaults = f.i18n.flatten(loc);
            ASSERT_TRUE(defaults.count(key) != 0);
            ASSERT_EQ(defaults.at(key), "");
            for (const auto& fallback : check_reply::fallbackKeys(key)) ASSERT_TRUE(defaults.count(fallback) != 0);
        }
        ++count;
    }
    ASSERT_EQ(count, size_t(50));
    ASSERT_TRUE(check_reply::familyForKey("dice.outcome.invalid.regular") == nullptr);
}

TEST(OutcomeReplies, EmptyInheritanceLocalePersonaAndLiteralEmptyCandidates) {
    OutcomeFixture f;
    ASSERT_TRUE(f.i18n.load());
    const auto keys = check_reply::candidates("bonus", "regular");
    ASSERT_FALSE(f.i18n.trCandidates(Locale::kZhHans, keys).has_value());
    f.set(keys[1], "standard");
    f.set(keys[0], "");
    ASSERT_EQ(*f.i18n.trCandidates(Locale::kZhHans, keys), "standard");
    // Empty slots in another language do not unexpectedly borrow Chinese overrides.
    ASSERT_FALSE(f.i18n.trCandidates(Locale::kEn, keys).has_value());
    f.set(keys[0], "global bonus");
    f.i18n.setPersonaBundles(42, Locale::kZhHans, {{keys[0], ""}, {keys[1], "persona standard"}});
    {
        auto scope = f.i18n.scopedPersona(42);
        ASSERT_EQ(*f.i18n.trCandidates(Locale::kZhHans, keys), "global bonus");
        f.i18n.clearOverride(Locale::kZhHans, keys[0]);
        ASSERT_EQ(*f.i18n.trCandidates(Locale::kZhHans, keys), "persona standard");
    }
    const auto weightedEmpty = weighted_templates::encode(json::array({{{"text", ""}, {"weight", 1}}, {{"text", "disabled"}, {"weight", 0}}}));
    f.set(keys[0], weightedEmpty);
    const auto muted = f.i18n.trCandidates(Locale::kZhHans, keys);
    ASSERT_TRUE(muted.has_value()); ASSERT_EQ(*muted, "");
    f.set("dice.check.result", "");
    ASSERT_EQ(f.i18n.tr(Locale::kZhHans, "dice.check.result"), "");
}

TEST(OutcomeReplies, WeightedNestedSampleAndPreRenderedPlainRemainIntact) {
    OutcomeFixture f;
    const auto key = check_reply::key("standard", "critical");
    f.i18n.setOverride(Locale::kZhHans, key, weighted_templates::encode(json::array({
        {{"text", "never"}, {"weight", 0}},
        {{"text", "**{nick}** {sample:{sample:good|good}|good}: `{result}`"}, {"weight", 3}}
    })), ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kPlainText);
    const auto plain = f.i18n.trCandidates(Locale::kZhHans, {key}, {{"nick", "A*B"}, {"result", "1"}});
    const auto format = I18n::endOutboundCapture();
    ASSERT_TRUE(plain.has_value()); ASSERT_EQ(*plain, "A*B good: 1");
    ASSERT_TRUE(format == ContentFormat::kPlainText);
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    const auto rich = f.i18n.trCandidates(Locale::kZhHans, {key}, {{"nick", "A*B"}, {"result", "1"}});
    const auto richFormat = I18n::endOutboundCapture();
    ASSERT_EQ(*rich, "**A\\*B** good: `1`");
    ASSERT_TRUE(richFormat == ContentFormat::kMarkdown);
}

TEST(OutcomeReplies, CocAllGradesMultiHouseRulesAndLegacyFallback) {
    OutcomeFixture f;
    f.set("dice.check.result", "old:{roll}/{rate}|{level}");
    f.set("dice.check.result_reason", "reason:{reason}|{level}");
    // Imported legacy labels still affect original templates when new slots are unset.
    f.set(legacyv2::msgKeyMap().at("strSuccess"), "old success");
    ASSERT_EQ(f.run(".ra(50)60"), "old:50/60|old success");
    ASSERT_EQ(f.run(".ra(50)60 测试"), "reason:测试|old success");
    f.family("standard");
    for (const auto& [number, grade] : std::vector<std::pair<int, std::string>>{
        {1, "critical"}, {10, "extreme"}, {25, "hard"}, {50, "regular"}, {80, "failure"}, {100, "fumble"}})
        ASSERT_EQ(f.run(".ra(" + std::to_string(number) + ")60"), "standard:" + grade);
    ASSERT_EQ(f.run(".ra 3#(50)60"), "standard:regular\nstandard:regular\nstandard:regular");
    f.setting("cocRule", "7");
    ASSERT_EQ(f.run(".ra(1)60"), "standard:extreme");
    f.i18n.clearOverride(Locale::kZhHans, check_reply::key("standard", "regular"));
    ASSERT_EQ(f.run(".ra(50)60"), "old:50/60|old success");
}

TEST(OutcomeReplies, BonusPenaltyAttachedSyntaxAndUnjudgedRolls) {
    OutcomeFixture f;
    f.family("standard"); f.family("bonus"); f.family("penalty");
    for (const auto& cmd : {".rb 60", ".rab侦查60", ".ra b 60", ".rc b 60"})
        ASSERT_TRUE(f.run(cmd).starts_with("bonus:"));
    for (const auto& cmd : {".rp 60", ".rap侦查60", ".ra p 60", ".rc p 60"})
        ASSERT_TRUE(f.run(cmd).starts_with("penalty:"));
    for (const auto& family : {"bonus", "penalty"})
        for (const auto& grade : check_reply::families()[1].grades) f.set(check_reply::key(family, grade), "");
    ASSERT_TRUE(f.run(".rb 60").starts_with("standard:"));
    ASSERT_TRUE(f.run(".rp 60").starts_with("standard:"));
    f.set("dice.roll.result", "plain:{res}");
    f.set("dice.roll.result_reason", "plain:{res}");
    ASSERT_TRUE(f.run(".r d1").starts_with("plain:"));
    ASSERT_TRUE(f.run(".rb").starts_with("plain:"));
    ASSERT_TRUE(f.run(".rp").starts_with("plain:"));
}

TEST(OutcomeReplies, BrpSpecialIsNotCocExtremeAndResistanceUsesActualWinner) {
    OutcomeFixture f;
    f.family("brp", "|{result}"); f.family("resist");
    for (int i = 0; i < 40; ++i) {
        const auto parts = split(f.run(".ba 60"));
        ASSERT_EQ(parts.size(), size_t(2));
        const int result = std::stoi(parts[1]);
        const auto grade = result >= 99 ? "fumble" : result > 60 ? "failure" : result <= 3 ? "critical" : result <= 12 ? "special" : "regular";
        ASSERT_EQ(parts[0], std::string("brp:") + grade);
    }
    ASSERT_EQ(f.run(".bav 10000 1"), "resist:regular");
    ASSERT_EQ(f.run(".bav 1 10000"), "resist:failure");
}

TEST(OutcomeReplies, OpposedOutcomeIsRelativeToFirstSideNotItsSuccessLevel) {
    OutcomeFixture f;
    f.family("opposed", "|{ra}|{rb}");
    auto rank = [](int roll) { return roll == 100 ? 0 : roll == 1 ? 5 : roll <= 12 ? 4 : roll <= 30 ? 3 : roll <= 60 ? 2 : 1; };
    for (int i = 0; i < 40; ++i) {
        const auto parts = split(f.run(".rav 60 60")); ASSERT_EQ(parts.size(), size_t(3));
        const int a = std::stoi(parts[1]), b = std::stoi(parts[2]);
        const int ar = rank(a), br = rank(b);
        const auto grade = ar > br ? "regular" : ar < br ? "failure" : ar >= 2 && a != b ? (a < b ? "regular" : "failure") : "tie";
        ASSERT_EQ(parts[0], std::string("opposed:") + grade);
    }
}

TEST(OutcomeReplies, SanAndGrowthBranchesDoNotAlterCardMutations) {
    OutcomeFixture f;
    f.family("sanity", "|{result}|{loss}|{final}");
    f.family("growth", "|{result}|{final}");
    for (int i = 0; i < 12; ++i) {
        f.cards.setAttr(f.msg.senderId, f.msg.targetId, "理智", 60);
        const auto parts = split(f.run(".sc 0/1"));
        ASSERT_EQ(parts.size(), size_t(4));
        const int result = std::stoi(parts[1]);
        const int loss = result > 60 ? 1 : 0;
        ASSERT_EQ(std::stoi(parts[2]), loss);
        ASSERT_EQ(std::stoi(parts[3]), 60 - loss);
        ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "理智").value_or(-1), 60 - loss);
        const auto growth = split(f.run(".en 侦查 60 -1/+2"));
        ASSERT_EQ(growth.size(), size_t(3));
        const bool passed = std::stoi(growth[1]) > 60;
        ASSERT_EQ(growth[0], passed ? "growth:regular" : "growth:failure");
        ASSERT_EQ(std::stoi(growth[2]), passed ? 62 : 59);
        ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-1), passed ? 62 : 59);
    }
}

TEST(OutcomeReplies, DndRequiresThresholdAndUsesPerRoundOutcomeWithoutRerolling) {
    OutcomeFixture f; f.setting("dndMode", "1");
    f.set("dnd.rdc.result", "legacy:{detail}"); f.set("dnd.rc.result", "unjudged");
    f.cards.setAttr(f.msg.senderId, f.msg.targetId, "力量", 16);
    f.family("standard"); f.family("dnd_check", "|{turn}|{result}");
    ASSERT_EQ(f.run(".rc 力量"), "unjudged");
    ASSERT_TRUE(f.run(".rdc 力量").starts_with("legacy:"));
    ASSERT_TRUE(f.run(".rdc 力量 +100 15").starts_with("dnd_check:regular|1|"));
    ASSERT_TRUE(f.run(".rdc 力量 -100 15").starts_with("dnd_check:failure|1|"));
    const auto rounds = split(f.run(".rdc 9# 力量 13"), '\n');
    ASSERT_EQ(rounds.size(), size_t(9));
    for (size_t i = 0; i < rounds.size(); ++i) {
        const auto parts = split(rounds[i]); ASSERT_EQ(parts.size(), size_t(3));
        ASSERT_EQ(std::stoi(parts[1]), static_cast<int>(i + 1));
        ASSERT_EQ(parts[0], std::stoi(parts[2]) >= 13 ? "dnd_check:regular" : "dnd_check:failure");
    }
    for (const auto& grade : {"regular", "failure"}) {
        f.i18n.clearOverride(Locale::kZhHans, check_reply::key("dnd_check", grade));
        f.i18n.clearOverride(Locale::kZhHans, check_reply::key("standard", grade));
    }
    const auto legacy = f.run(".rdc 2# 力量 13");
    ASSERT_TRUE(legacy.starts_with("legacy:"));
    ASSERT_EQ(legacy.find("legacy:"), legacy.rfind("legacy:"));
}

TEST(OutcomeReplies, DeathSaveUsesNaturalDieAndPreservesTerminalStatus) {
    OutcomeFixture f;
    f.family("death_save", "|{roll}");
    f.set("dnd.ds.dead", "DEAD"); f.set("dnd.ds.stabilized", "STABLE");
    for (int i = 0; i < 30; ++i) {
        f.cards.setAttr(f.msg.senderId, f.msg.targetId, "hp", 0);
        f.cards.setAttr(f.msg.senderId, f.msg.targetId, "dssuccess", 2);
        f.cards.setAttr(f.msg.senderId, f.msg.targetId, "dsfail", 2);
        const auto out = f.run(".ds +100");
        const auto lines = split(out, '\n'); const auto parts = split(lines[0]);
        ASSERT_EQ(parts.size(), size_t(2));
        const int roll = std::stoi(parts[1]);
        ASSERT_EQ(parts[0], roll == 20 ? "death_save:critical" : roll == 1 ? "death_save:fumble" : "death_save:regular");
        if (roll == 20) ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "hp").value_or(-1), 1);
        else ASSERT_TRUE(out.ends_with(roll == 1 ? "\nDEAD" : "\nSTABLE"));
    }
}

TEST(OutcomeReplies, HiddenPsychologyAndHiddenChecksNeverExposeGradeToGroup) {
    OutcomeFixture f;
    auto adapter = std::make_shared<OutcomePrivateAdapter>(); f.adapters.registerAdapter(adapter);
    f.family("psychology", "|{target}|{gid}"); f.family("standard");
    f.set("dice.rx.receipt", "receipt"); f.set("dice.check.hidden", "hidden");
    f.msg.atList = {"3000"};
    ASSERT_EQ(f.run(".rx 60"), "receipt");
    ASSERT_EQ(adapter->sent.size(), size_t(1));
    ASSERT_TRUE(adapter->sent[0].type == MessageType::kPrivate);
    ASSERT_EQ(adapter->sent[0].targetId, f.msg.senderId);
    ASSERT_TRUE(adapter->sent[0].content.starts_with("psychology:"));
    f.msg.atList.clear();
    ASSERT_EQ(f.run(".rah(50)60"), "hidden");
    ASSERT_EQ(adapter->sent.size(), size_t(2));
    ASSERT_EQ(adapter->sent[1].content, "standard:regular");
}

TEST(LegacyCheckText, DistinctSingleMultiLabelsAndPrefixOnlyOnce) {
    OutcomeFixture f; ASSERT_TRUE(f.i18n.load());
    f.set("dice.level.regular", "MULTI");
    f.set("dice.compat.check.single.regular", "SINGLE:{result}");
    f.set("dice.compat.check.prefix", "P:{attr}:");
    f.set("dice.compat.check.prefix_reason", "WHY:{reason}:{attr}:");
    ASSERT_EQ(f.run(".ra(50)侦查60"), "P:侦查:50/60 SINGLE:50");
    ASSERT_EQ(f.run(".ra 3#(50)侦查60"), "P:侦查:50/60 MULTI\n50/60 MULTI\n50/60 MULTI");
    ASSERT_EQ(f.run(".ra(50)侦查60 测试"), "WHY:测试:侦查:50/60 SINGLE:50");
    f.set(check_reply::key("standard", "regular"), "NEW");
    ASSERT_EQ(f.run(".ra 2#(50)侦查60"), "NEW\nNEW");
    f.set(check_reply::key("standard", "regular"), "NEW:{level}|{outcome}");
    ASSERT_EQ(f.run(".ra(50)侦查60"), "NEW:SINGLE:50|SINGLE:50");
    f.i18n.clearOverride(Locale::kZhHans, check_reply::key("standard", "regular"));
    f.set("dice.check.result", "NATIVE:{level}");
    ASSERT_EQ(f.run(".ra(50)侦查60"), "NATIVE:SINGLE:50");
}

TEST(LegacyCheckText, SanConditionsUseSameRollLossAndCardMutation) {
    OutcomeFixture f; ASSERT_TRUE(f.i18n.load());
    f.set("dice.level.regular", "PASS"); f.set("dice.level.failure", "FAIL"); f.set("dice.level.fumble", "FUMBLE");
    f.set("dice.compat.sanity.result",
        "{res}|{grade:rank?2={strSuccess}&1={strFailure}&0={strFumble}}|{case:loss?0=ZERO&else=LOSS:{change}}|{loss}|{final}");
    for (int i = 0; i < 20; ++i) {
        f.cards.setAttr(f.msg.senderId, f.msg.targetId, "理智", 60);
        const auto parts = split(f.run(".sc 0/1d6"));
        ASSERT_EQ(parts.size(), size_t(5));
        ASSERT_TRUE(parts[0].starts_with("1D100=")); ASSERT_TRUE(parts[0].ends_with("/60"));
        const int roll = std::stoi(parts[0].substr(6)), loss = std::stoi(parts[3]);
        ASSERT_EQ(parts[1], roll == 100 ? "FUMBLE" : roll > 60 ? "FAIL" : "PASS");
        if (roll == 100) ASSERT_EQ(loss, 6);
        else if (roll <= 60) ASSERT_EQ(loss, 0);
        else ASSERT_TRUE(loss >= 1 && loss <= 6);
        ASSERT_TRUE(loss == 0 ? parts[2] == "ZERO" : parts[2].starts_with("LOSS:"));
        ASSERT_EQ(std::stoi(parts[4]), 60 - loss);
        ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "理智").value_or(-1), 60 - loss);
    }
    for (const auto& grade : check_reply::families()[1].grades) f.set(check_reply::key("standard", grade), "NEW:{res}");
    f.cards.setAttr(f.msg.senderId, f.msg.targetId, "理智", 60);
    const auto result = f.run(".sc 0/1");
    ASSERT_TRUE(result.starts_with("NEW:")); ASSERT_TRUE(result.find("1D100") == std::string::npos);
}

TEST(LegacyCheckText, GrowthReferencesStayLiveWithNativeOutcomePrecedence) {
    OutcomeFixture f; ASSERT_TRUE(f.i18n.load());
    f.set("dice.compat.growth.base", "BASE:{attr}:{res}");
    f.set("dice.compat.growth.unchanged", "{strEnRoll}|UNCHANGED:{final}");
    f.set("dice.compat.growth.success", "{strEnRoll}|CHANGE:{change}|{final}");
    for (int i = 0; i < 12; ++i) {
        const auto result = f.run(".en 侦查100");
        ASSERT_TRUE(result.starts_with("BASE:侦查:1D100="));
        const auto parts = split(result);
        const int roll = std::stoi(parts[0].substr(parts[0].find('=') + 1));
        if (roll > 95) { ASSERT_EQ(parts.size(), size_t(3)); ASSERT_TRUE(std::stoi(parts[2]) > 100); }
        else { ASSERT_EQ(parts.size(), size_t(2)); ASSERT_EQ(parts[1], "UNCHANGED:100"); }
    }
    f.set("dice.compat.growth.base", "EDITED:{res}");
    ASSERT_TRUE(f.run(".en 侦查0+2").starts_with("EDITED:"));
    f.set(check_reply::key("growth", "regular"), "NEW:{final}");
    ASSERT_EQ(f.run(".en 侦查0+2"), "NEW:2");
    ASSERT_EQ(f.cards.getAttr(f.msg.senderId, f.msg.targetId, "侦查").value_or(-1), 2);
}

TEST(LegacyCheckText, SafeNestedMacrosReferencesEscapingAndNoUserCode) {
    const std::string grade = "{grade:rank?2=PASS&1=FAIL&0=FUMBLE}";
    for (int rank = 0; rank < 6; ++rank)
        ASSERT_EQ(I18n::previewTemplate(grade, {{"rank", std::to_string(rank)}}), rank == 0 ? "FUMBLE" : rank == 1 ? "FAIL" : "PASS");
    ASSERT_EQ(I18n::previewTemplate("{sample:{case:loss?0=ZERO&else=LOSS}|ZERO}", {{"loss", "0"}}), "ZERO");
    const std::string inserted = "{case:loss?0=not-code&else=no}";
    ASSERT_EQ(I18n::previewTemplate("{nick}", {{"nick", inserted}, {"loss", "0"}}), inserted);
    const std::string unsafe = "{js:throw 1}{wait:3000}{py:print(1)}";
    ASSERT_EQ(I18n::previewTemplate(unsafe), unsafe);
    OutcomeFixture f; ASSERT_TRUE(f.i18n.load());
    f.set("dice.level.regular", "*literal*");
    ASSERT_EQ(f.i18n.previewWithReferences(Locale::kZhHans, "**{nick}** {strSuccess}", {{"nick", "A*B"}}, ContentFormat::kMarkdown),
        "**A\\*B** \\*literal\\*");
    f.i18n.setPersonaBundles(42, Locale::kZhHans, {{"dice.level.regular", "PERSONA"}});
    {
        auto scope = f.i18n.scopedPersona(42);
        ASSERT_EQ(f.i18n.previewWithReferences(Locale::kZhHans, "{strSuccess}", {}, ContentFormat::kPlainText), "PERSONA");
    }
    f.set("dice.compat.growth.base", "{text:dice.compat.growth.base}");
    ASSERT_EQ(f.i18n.tr(Locale::kZhHans, "dice.compat.growth.base"), "");
    const auto preview = outbound::templatePreview({{"text", "{grade:rank?2={strSuccess}&0=NO}"}, {"args", {{"rank", "2"}}}}, &f.i18n);
    ASSERT_EQ(preview["onebot"].get<std::string>(), "*literal*");
}
