#include "test_framework.h"
#include "core/command_router.h"
#include "storage/group_account_settings.h"
#include <numeric>

using namespace dice;
namespace {
struct CocFixture {
    std::filesystem::path dir=std::filesystem::temp_directory_path() /
        ("dicenext-coc7r-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Database db;
    ConfigManager cfg{u8str(dir/"config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path()/"i18n")};
    LocaleResolver resolver{db,cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db,cfg,engine,i18n,resolver,cards,deck,adapters};
    Message msg;
    CocFixture() {
        std::filesystem::create_directories(dir);
        if (!db.open(u8str(dir/"test.db")) || !i18n.load()) throw std::runtime_error("fixture init");
        cfg.resetDefault();
        CommandRouter::reloadRulePacks({u8str(std::filesystem::path(__FILE__).parent_path().parent_path()/"resources/default-data/rules")});
        msg.platform="onebot_v11"; msg.adapterId="coc-account"; msg.selfId="9000";
        msg.senderId="1000"; msg.senderName="玩家"; msg.targetId="2000"; msg.type=MessageType::kGroup;
        msg.extra={{"role","admin"}};
        setting("ruleSystem","coc7r");
    }
    ~CocFixture() { CommandRouter::reloadRulePacks({}); db.close(); std::error_code ec; std::filesystem::remove_all(dir,ec); }
    void setting(const std::string& key,const std::string& value) {
        setAccountGroupSetting(*db.getStorage(),msg.adapterId,msg.platform,msg.targetId,msg.targetId,key,value);
    }
    std::string run(const std::string& content) { msg.content=content; return router.handleMessage(msg,Locale::kZhHans); }
    void set(const std::string& key,int value,const std::string& user="1000") { cards.setAttr(user,msg.targetId,key,value); }
    int get(const std::string& key) { return cards.getAttr(msg.senderId,msg.targetId,key).value_or(-999); }
    json data() { return cards.snapshot(msg.senderId,cards.boundCard(msg.senderId,msg.targetId))->data; }
    void stats() { set("POW",50); set("INT",50); set("SAN",50); set("APP",60); }
    json state() {
        const auto d=data();
        if (!d.contains("__meta") || !d["__meta"].contains("coc7r")) return json::object();
        return d["__meta"]["coc7r"].value(json::array({msg.platform,msg.adapterId,msg.targetId}).dump(),json::object());
    }
};
}
TEST(Coc7r, AttributeArraysAndMovementBoundaries) {
    for (const auto& a:coc7r::arrays()) {
        ASSERT_EQ(std::accumulate(a.begin(),a.end(),0),480);
        for (int v:a) ASSERT_TRUE(v>=30 && v<=90);
    }
    ASSERT_EQ(coc7r::movement(50,50,50),7);
    ASSERT_EQ(coc7r::movement(51,50,50),8);
    ASSERT_EQ(coc7r::movement(51,51,50),9);
    for (int age=40;age<=80;age+=10) ASSERT_EQ(coc7r::movement(51,51,50,age),9-(age-30)/10);
}
TEST(Coc7r, CostBoundsAndMalformedInput) {
    for (const auto* s:{"0","1","d4","2D6+2","2d4-1","1d4+1d6"}) ASSERT_TRUE(coc7r::cost(s).has_value());
    ASSERT_EQ(coc7r::cost("2d4-1")->minimum,1);
    ASSERT_EQ(coc7r::cost("2d4-1")->maximum,7);
    ASSERT_EQ(coc7r::cost("1d4+1d6")->maximum,10);
    ASSERT_EQ(coc7r::cost(coc7r::reassureFailureCost)->maximum,4);
    for (const auto* s:{"","-1","1-2","1d0","101d6","1d10001","999999999999999999","1+","1--2","1d6/2","0/1","d"}) ASSERT_FALSE(coc7r::cost(s).has_value());
}
TEST(Coc7r, OptionalSkillBasesAndSpecialtyFloor) {
    const std::map<std::string,int> values={{"敏捷",65},{"力量",70},{"教育",60},{"外貌",55},{"斗殴",80},{"手枪",70},{"斧",10}};
    auto lookup=[&](const std::string& k)->std::optional<int> {auto i=values.find(k);return i==values.end()?std::nullopt:std::optional<int>(i->second);};
    ASSERT_EQ(*coc7r::base("安抚",lookup,false),11);
    ASSERT_EQ(*coc7r::base("急救",lookup,true),30);
    ASSERT_EQ(*coc7r::base("斗殴",lookup,true),35);
    ASSERT_EQ(*coc7r::base("宗教:道教",lookup,false),10);
    ASSERT_FALSE(coc7r::base("宗教",lookup,false).has_value());
    ASSERT_FALSE(coc7r::base("冲锋枪",lookup,false).has_value());
    ASSERT_EQ(*coc7r::value("斧",lookup,false,false),10);
    ASSERT_EQ(*coc7r::value("斧",lookup,false,true),40);
    ASSERT_EQ(*coc7r::value("步枪/霰弹枪",lookup,false,true),35);
}
TEST(Coc7r, SanityFiveTenAndFifthBoundaries) {
    coc7r::SanityState state;
    for (int loss=0;loss<=12;++loss) {
        auto out=coc7r::loseSanity(state,60,loss,51,true);
        ASSERT_EQ(out.intRequired,loss>=5);
        ASSERT_EQ(out.newChanged,loss>=10); // Fifth POW value rounds down.
    }
    state.changed=true;
    ASSERT_FALSE(coc7r::loseSanity(state,60,9,50,true).newShock);
    ASSERT_TRUE(coc7r::loseSanity(state,60,10,50,true).newShock);
    state.shock=true;
    auto immune=coc7r::loseSanity(state,60,30,50,true);
    ASSERT_EQ(immune.loss,0); ASSERT_EQ(immune.remaining,60);
    state={}; auto lost=coc7r::loseSanity(state,3,100,50,true);
    ASSERT_EQ(lost.loss,3); ASSERT_TRUE(lost.lost); ASSERT_FALSE(lost.newShock);
    auto failedInt=coc7r::loseSanity(state,60,6,50,false);
    ASSERT_FALSE(failedInt.newShock); ASSERT_EQ(failedInt.state.chapterLoss,6);
}
TEST(Coc7r, SafeStateParsingAndRealityProtection) {
    ASSERT_EQ(coc7r::sanityState(json{{"chapterLoss","bad"},{"shock",1}}).chapterLoss,0);
    ASSERT_FALSE(coc7r::sanityState(json{{"shock",1}}).shock);
    coc7r::SanityState state; state.realityProtected=true;
    ASSERT_TRUE(coc7r::loseSanity(state,50,0,50).state.realityProtected);
    ASSERT_FALSE(coc7r::loseSanity(state,50,1,50).state.realityProtected);
}
TEST(Coc7r, ChaseAndAutomaticFireBoundaries) {
    ASSERT_EQ(coc7r::automaticMultiplier(20),5); ASSERT_EQ(coc7r::automaticMultiplier(50),10);
    ASSERT_EQ(coc7r::automaticMultiplier(100),20); ASSERT_EQ(coc7r::automaticMultiplier(30),0);
    auto a=coc7r::chase(1,2,false,1); ASSERT_TRUE(a.out); ASSERT_EQ(a.location,2);
    auto b=coc7r::chase(7,2,false,6); ASSERT_FALSE(b.out); ASSERT_EQ(b.location,3); ASSERT_EQ(b.mvp,1);
    ASSERT_TRUE(coc7r::chase(9,2,false,0,true).out);
}
TEST(Coc7r, ActivationAndNoGlobalAliasLeak) {
    CocFixture f; f.stats(); f.set("取悦",80);
    const auto before=f.data();
    ASSERT_TRUE(f.run(".setcoc7r").find("Keeper Companion")!=std::string::npos);
    ASSERT_TRUE(f.run(".st show 取悦").find("10")!=std::string::npos);
    ASSERT_TRUE(f.data()==before);
    ASSERT_TRUE(f.run(".set coc7").find("Call of Cthulhu")!=std::string::npos);
    ASSERT_TRUE(f.run(".st show 取悦").find("80")!=std::string::npos);
    ASSERT_EQ(CharacterCardStore::canonical("取悦"),std::string("取悦"));
}
TEST(Coc7r, ProfileInputAliasesAndUnknownSkillIsolation) {
    CocFixture f; f.stats();
    f.run(".st 取悦55"); ASSERT_EQ(f.get("说服"),55);
    ASSERT_EQ(f.get("取悦"),-999);
    ASSERT_TRUE(f.run(".st show 安抚").find("12")!=std::string::npos);
    ASSERT_TRUE(f.run(".ra 冲锋枪").find("冲锋枪")!=std::string::npos);
    ASSERT_TRUE(f.run(".rb SAN").find("不能使用")!=std::string::npos);
}
TEST(Coc7r, GroupAccountOptionsDoNotLeak) {
    CocFixture f; f.set("DEX",60); f.set("斗殴",80); f.set("斧",10);
    f.run(".coc7r options specialties on");
    ASSERT_TRUE(f.run(".st show 斧").find("40")!=std::string::npos);
    f.msg.adapterId="second-account"; f.setting("ruleSystem","coc7r");
    ASSERT_TRUE(f.run(".st show 斧").find("10")!=std::string::npos);
    f.msg.targetId="other-group"; f.setting("ruleSystem","coc7r");
    ASSERT_TRUE(f.run(".coc7r options").find("specialties=off")!=std::string::npos);
}
TEST(Coc7r, NonAdminCannotActivateOrChangeOptions) {
    CocFixture f; f.msg.extra={{"role","member"}};
    ASSERT_EQ(f.run(".coc7r options bases on"),f.i18n.tr(Locale::kZhHans,"fun.set.no_perm"));
    ASSERT_EQ(f.run(".set coc7"),f.i18n.tr(Locale::kZhHans,"fun.set.no_perm"));
    ASSERT_EQ(f.run(".coc7r reassure 2001"),f.i18n.tr(Locale::kZhHans,"gate.no_perm"));
}
TEST(Coc7r, ExplicitSanAndInvalidArgumentsNeverWriteCard) {
    CocFixture f; f.stats(); const auto before=f.data();
    for (const auto* cmd:{".sc 0/1 40",".sc 0/0 50 mythos",".sc 0/1 wrong",".scb 0/1",".sc 0/1 40 50",
                         ".sc -1/1",".sc 0/999999999999",".coc7r loss -1"})
        ASSERT_TRUE(!f.run(cmd).empty());
    ASSERT_TRUE(f.data()==before);
}
TEST(Coc7r, ChapterSleepAndChangedHaveDifferentLifetimes) {
    CocFixture f; f.stats();
    for (int i=0;i<10;++i) f.run(".coc7r loss 1");
    ASSERT_EQ(f.get("SAN"),40); ASSERT_TRUE(f.state().at("changed").get<bool>());
    f.run(".coc7r sleep confirm"); ASSERT_TRUE(f.state().at("changed").get<bool>());
    f.run(".coc7r chapter confirm"); ASSERT_TRUE(f.state().at("changed").get<bool>());
    ASSERT_EQ(f.state().at("chapterLoss").get<int>(),0);
    f.run(".coc7r recover confirm"); ASSERT_FALSE(f.state().at("changed").get<bool>());
}
TEST(Coc7r, LockedAndStaleCardsCannotBeUpdated) {
    CocFixture f; f.stats();
    auto snapshot=*f.cards.snapshot("1000",""); auto updated=snapshot.data; updated["理智"]=12;
    f.set("INT",60);
    ASSERT_FALSE(f.cards.replaceSnapshot("1000",snapshot,updated)); ASSERT_EQ(f.get("SAN"),50);
    f.cards.lockCard("1000","2000","w"); const auto before=f.data();
    ASSERT_TRUE(f.run(".sc 1/1").find("锁")!=std::string::npos);
    ASSERT_TRUE(f.run(".coc7r loss 1").find("锁")!=std::string::npos);
    ASSERT_TRUE(f.run(".en 侦查50").find("锁")!=std::string::npos);
    ASSERT_TRUE(f.data()==before);
}
TEST(Coc7r, StateSurvivesRenameAndStoreRecreation) {
    CocFixture f; f.cards.createCard("1000","Alice"); f.cards.bindCard("1000","2000","Alice"); f.stats();
    f.run(".coc7r loss 1");
    ASSERT_TRUE(f.cards.renameCard("1000","Alice","Bob"));
    ASSERT_EQ(f.state().at("chapterLoss").get<int>(),1);
    CharacterCardStore reopened(f.db);
    auto card=reopened.snapshot("1000","Bob");
    ASSERT_TRUE(card && card->data["__meta"]["coc7r"].is_object());
    f.cards.createCard("1000","Another"); f.cards.bindCard("1000","2000","Another"); f.stats();
    ASSERT_TRUE(f.state().empty());
}
TEST(Coc7r, MythosRequiresConfirmedSourceAndCannotBeRepeated) {
    CocFixture f; f.stats(); f.set("POW",5);
    f.run(".coc7r loss 1 mythos");
    ASSERT_EQ(f.state().at("mythosPending").get<int>(),1);
    f.run(".coc7r mythos first"); ASSERT_EQ(f.get("CM"),5);
    const auto before=f.data();
    f.run(".coc7r mythos first"); ASSERT_TRUE(f.data()==before);
}
TEST(Coc7r, InitAndCharacterGenerationAreNonDestructive) {
    CocFixture f; f.stats(); const auto before=f.data();
    for (const auto* cmd:{".coc7r",".coc7r array 1",".coc7r array 10",".coc",".pc build",".coc7r init"})
        ASSERT_TRUE(!f.run(cmd).empty());
    ASSERT_TRUE(f.data()==before);
    ASSERT_TRUE(f.run(".coc7").find("coc7r")==std::string::npos);
}
TEST(Coc7r, ReassureSelfAndMissingTargetAreSafe) {
    CocFixture f; f.stats(); const auto before=f.data();
    ASSERT_TRUE(f.run(".coc7r reassure 1000").find("自己")!=std::string::npos);
    ASSERT_TRUE(f.run(".coc7r reassure 9999").find("缺少")!=std::string::npos);
    ASSERT_TRUE(f.data()==before);
}
TEST(Coc7r, AutoCardStartsSanityAtPow) {
    // p.85: starting SAN equals POW; 99−Mythos is only the ceiling.
    CocFixture f;
    f.run(".st 力量60 体质60 体型50 意志55 智力70");
    ASSERT_EQ(f.get("理智"),55);
    ASSERT_TRUE(f.run(".coc7r init").find("已有")!=std::string::npos);
    f.run(".st 意志65");                 // POW does not drive the SAN ceiling
    ASSERT_EQ(f.get("理智"),55);
    f.msg.senderId="1002"; f.set("POW",60,"1002"); f.set("INT",50,"1002");
    f.run(".sc 0/0");                     // no SAN on the card yet → checks against POW
    ASSERT_EQ(f.get("理智"),60);
}
TEST(Coc7r, CoreAutoCardStartsSanityAtPow) {
    // Original Dice! COC7 template: 理智=&意志. 99−Mythos is only a ceiling.
    CocFixture f; f.setting("ruleSystem","coc7");
    f.run(".st 体质60 体型50");
    ASSERT_EQ(f.get("理智"),-999);       // no POW yet → nothing to derive
    f.run(".st 意志55");
    ASSERT_EQ(f.get("理智"),55);
    f.run(".st 克苏鲁神话10");             // ceiling 89: SAN 55 untouched
    ASSERT_EQ(f.get("理智"),55);
    f.set("SAN",95);
    f.run(".st 克苏鲁神话20");             // ceiling 79: only the excess is cut
    ASSERT_EQ(f.get("理智"),79);
    f.msg.senderId="1001"; f.set("POW",60,"1001");
    f.run(".sc 0/0");
    ASSERT_EQ(f.get("理智"),60);
}
TEST(Coc7r, ReassureAcceptsMentionedTarget) {
    // Adapters strip the @ segment from the text; the target arrives in atList.
    CocFixture f; f.stats();
    for (auto [k,v]:{std::pair{"POW",50},{"INT",50},{"SAN",40}}) f.set(k,v,"2001");
    f.msg.atList={"2001"};
    ASSERT_TRUE(f.run(".coc7r reassure").find("2001")!=std::string::npos);
    ASSERT_TRUE(f.run(".coc7r reassure push").find("2001")!=std::string::npos);
    ASSERT_TRUE(f.run(".coc7r reassure later").find("reassure")!=std::string::npos);
    f.msg.atList={"1000"};
    ASSERT_TRUE(f.run(".coc7r reassure").find("自己")!=std::string::npos);
}
TEST(Coc7r, CoreHouseRuleIsRestoredWhenSwitchingBack) {
    CocFixture f; f.setting("cocRule","3");
    ASSERT_TRUE(f.run(".setcoc show").find("规则0")!=std::string::npos);
    f.run(".setcoc 2"); f.run(".set coc7");
    ASSERT_TRUE(f.run(".setcoc show").find("规则3")!=std::string::npos);
    f.run(".set coc7r"); ASSERT_TRUE(f.run(".setcoc show").find("规则2")!=std::string::npos);
}
TEST(Coc7r, AllLocalesResolveProfileMessages) {
    CocFixture f; f.stats();
    for (auto loc:{Locale::kZhHans,Locale::kZhHant,Locale::kEn,Locale::kJa}) {
        f.msg.content=".coc7r help"; auto out=f.router.handleMessage(f.msg,loc);
        ASSERT_TRUE(out.size()>200); ASSERT_TRUE(out!="coc7r.help");
    }
}
TEST(Coc7r, SkillChecksKeepStandardThresholdsAndAliases) {
    CocFixture f; f.stats(); f.set("说服",60);
    for (int n=1;n<=100;++n) {
        auto a=f.run(".ra("+std::to_string(n)+") 说服");
        auto b=f.run(".ra("+std::to_string(n)+") 取悦");
        ASSERT_TRUE(!a.empty() && !b.empty());
        ASSERT_TRUE(a.find("60")!=std::string::npos && b.find("60")!=std::string::npos);
        const auto key=n==1?"dice.level.critical":n==100?"dice.level.fumble":n<=12?"dice.level.extreme":
            n<=30?"dice.level.hard":n<=60?"dice.level.regular":"dice.level.failure";
        const auto level=f.i18n.tr(Locale::kZhHans,key);
        ASSERT_TRUE(a.find(level)!=std::string::npos && b.find(level)!=std::string::npos);
    }
}

TEST(Coc7r, RewardsRespectCapAndDoNotReduceChapterLoss) {
    CocFixture f; f.stats(); f.set("CM",47);
    f.run(".coc7r loss 1"); f.run(".coc7r reward 100");
    ASSERT_EQ(f.get("SAN"),52); ASSERT_EQ(f.state().at("chapterLoss").get<int>(),1);
    f.set("SAN",0); const auto before=f.data();
    f.run(".coc7r reward 100"); ASSERT_TRUE(f.data()==before);
}
TEST(Coc7r, SelfHelpSettlesExistingRollAndSpendsLuckAtomically) {
    CocFixture f; f.stats(); f.set("luck",10);
    f.run(".coc7r selfhelp INT 30 5");
    ASSERT_EQ(f.get("luck"),5); ASSERT_TRUE(f.get("SAN")>=51 && f.get("SAN")<=56);
    const int san=f.get("SAN");
    f.run(".coc7r selfhelp INT 26"); ASSERT_EQ(f.get("SAN"),san-1);
    const auto before=f.data(); f.run(".coc7r selfhelp INT 100 1"); ASSERT_TRUE(f.data()==before);
    f.run(".coc7r selfhelp INT 30 6"); ASSERT_TRUE(f.data()==before);
}
TEST(Coc7r, SkillSpecializationsAcceptParentheses) {
    CocFixture f; f.stats(); f.set("EDU",55);
    f.run(".st 宗教(道教)30");
    ASSERT_TRUE(f.run(".st show 宗教(道教)").find("30")!=std::string::npos);
    ASSERT_TRUE(f.run(".st show 母语(汉语)").find("55")!=std::string::npos);
    ASSERT_EQ(coc7r::canonical("Science(人类学)"),std::string("人类学"));
    ASSERT_EQ(coc7r::canonical("Fighting(Brawl)"),std::string("斗殴"));
}
TEST(Coc7r, ExplicitGrowthIsNotOverriddenByTakeFive) {
    const auto custom=legacy_args::growth("侦查0+3"); ASSERT_TRUE(custom.explicitGrowth);
    const auto standard=legacy_args::growth("侦查0"); ASSERT_FALSE(standard.explicitGrowth);
    CocFixture f; f.stats(); f.run(".coc7r options take5 on");
    f.run(".en 侦查0"); ASSERT_EQ(f.get("侦查"),5); // Every D100 exceeds skill 0.
    f.run(".en 侦查0+3"); ASSERT_EQ(f.get("侦查"),3);
    ASSERT_FALSE(coc7r::isSkill("力量")); ASSERT_TRUE(coc7r::isSkill("侦查"));
}
TEST(Coc7r, BotOffAndRollOffDoNotMutateCards) {
    CocFixture f; f.stats(); const auto before=f.data();
    f.setting("enabled","0"); ASSERT_TRUE(f.run(".coc7r loss 1").empty());
    ASSERT_TRUE(f.data()==before);
}

