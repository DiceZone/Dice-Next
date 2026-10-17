#include "test_framework.h"
#include "legacy_import_test_support.h"
#include "../src/core/character/card_store.h"
#include "../src/adapter/adapter_manager.h"
#include "../src/core/command_router.h"
#include "../src/service/backup_service.h"
#include "../src/storage/legacy_import_v2.h"

using namespace dice;
using namespace dice::legacyv2;
namespace {
struct ReleaseImportFixture {
    fs::path previous = fs::current_path();
    fs::path root = fs::temp_directory_path() / ("dicenext-release-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Database db;
    ConfigManager cfg{"config"};
    I18n i18n{migration_resources::utf8(fs::path(__FILE__).parent_path().parent_path() / "i18n")};
    LuaPluginManager lua;
    ReleaseImportFixture() {
        if (!fs::create_directory(root)) throw std::runtime_error("cannot create isolated fixture");
        fs::current_path(root); fs::create_directories("old/user"); fs::create_directories("old/conf");
        if (!db.open("app.db")) throw std::runtime_error("cannot open isolated database");
        cfg.resetDefault();
    }
    ~ReleaseImportFixture() { db.close(); fs::current_path(previous); } // Leave only synthetic evidence; never delete source data.
    void file(const fs::path& path, const std::string& text) { fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << text; }
};
template<class T> void raw(std::ofstream& out, T value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
void str(std::ofstream& out, const std::string& text) { raw<int16_t>(out, static_cast<int16_t>(text.size())); out << text; }
void utf(std::ofstream& out, const std::string& text) { raw<char>(out, 20); str(out, text); }
void card(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    raw<int32_t>(out, 1); raw<int64_t>(out, 10001); raw<int16_t>(out, 1); raw<int16_t>(out, 1); raw<uint16_t>(out, 0);
    str(out, "Tag"); str(out, "原卡名"); str(out, "Attr"); raw<int16_t>(out, 4);
    str(out, "__Type"); utf(out, "DND5E"); str(out, "__Name"); utf(out, "原卡名");
    str(out, "&伤害"); utf(out, "3+4"); str(out, "小数"); raw<char>(out, 3); raw<double>(out, 1.75);
    str(out, "Info"); raw<int16_t>(out, 1); str(out, "来源"); str(out, "旧版"); str(out, "END");
    raw<int16_t>(out, 1); raw<uint64_t>(out, 0); raw<uint16_t>(out, 0);
}
}

TEST(ReleaseMigration, RevokedBlacklistDoesNotReviveAndLocalAliasesImport) {
    ReleaseImportFixture f;
    f.file("old/conf/BlackList.json", R"([{"type":"ban","fromUID":1,"erase":["fromUID"]},{"type":"kick","fromUID":2,"danger":0},{"type":"local","fromQQ":3,"danger":2},{"type":"extern","fromUID":4}])");
    json report = json::array(); ASSERT_EQ(importBlacklist(f.db, "old/conf", &report), 1);
    const auto rows = f.db.getStorage()->get_all<BanlistRow>(); ASSERT_EQ(rows.size(), size_t(1)); ASSERT_EQ(rows[0].targetId, "3");
}
TEST(ReleaseMigration, UnsupportedRestrictionsDisableWithoutLosingImplicitKeywords) {
    ReleaseImportFixture f; ReplyManager replies(f.db, f.cfg);
    f.file("old/conf/CustomMsgReply.json", R"({"implicit":{"answer":"ok"},"restricted":{"match":["test"],"answer":"no","limit":"user_var:trust=4+"},"scoped":{"match":["timer"],"answer":"no","limit":"cd:@user=60"}})");
    json report = json::array(); ASSERT_EQ(importReplies(replies, "old/conf", {}, nullptr, &report), 3);
    ASSERT_TRUE(replies.matchMessage("test").empty()); ASSERT_TRUE(replies.matchMessage("timer").empty());
    ASSERT_EQ(replies.matchMessage("implicit").size(), size_t(1)); ASSERT_EQ(report.size(), size_t(2));
    ASSERT_TRUE(report[0]["disabled"].get<bool>());
}
TEST(ReleaseMigration, RegexRuntimeLimitsAreCheckedAtImport) {
    ReleaseImportFixture f; ReplyManager replies(f.db, f.cfg); std::string pattern;
    for (int i=0; i<100; ++i) pattern += "a{1}"; pattern += "$";
    f.file("old/conf/CustomMsgReply.json", json{{"long", {{"regex", {pattern}}, {"answer", "text"}}}}.dump());
    json report = json::array(); ASSERT_EQ(importReplies(replies, "old/conf", {}, nullptr, &report), 0);
    ASSERT_EQ(report.size(), size_t(1)); ASSERT_EQ(report[0]["status"], "preserved"); ASSERT_TRUE(report[0].contains("original"));
}
TEST(ReleaseMigration, CardsDecodeCurrentTypeInfoExpressionsAndNameFromGoodBackup) {
    ReleaseImportFixture f; card("old/user/PlayerCards.bak"); f.file("old/user/PlayerCards.RDconf", "");
    ImportResult report; int users = 0; ASSERT_EQ(importCards(f.db, "old/user", users, {}, &report), 1);
    CharacterCardStore cards(f.db);
    ASSERT_EQ(cards.displayName("10001", ""), "原卡名");
    ASSERT_EQ(cards.getValueByName("10001", "", "__Type")->get<std::string>(), "DND5E");
    ASSERT_EQ(cards.getValueByName("10001", "", "来源")->get<std::string>(), "旧版");
    ASSERT_EQ(cards.getExpressionsByName("10001", "").at(CharacterCardStore::canonical("伤害")), "3+4");
    ASSERT_EQ(cards.getValueByName("10001", "", "小数")->get<double>(), 1.75);
    ASSERT_EQ(f.db.getStorage()->get_all<UserSettingRow>()[0].groupId, "");
}
TEST(ReleaseMigration, CorruptBinaryIsNotCommittedAndRootReportIsNotFalseSuccess) {
    ReleaseImportFixture f; ReplyManager replies(f.db, f.cfg);
    const auto empty = runImport(f.db, f.cfg, f.i18n, replies, "old", nullptr, &f.lua); ASSERT_FALSE(empty["ok"].get<bool>());
    std::ofstream out("old/user/PlayerCards.RDconf", std::ios::binary); raw<int32_t>(out, 1); raw<int64_t>(out, 10001); out.close();
    const auto result = runImport(f.db, f.cfg, f.i18n, replies, "old", nullptr, &f.lua);
    if (!result.contains("reportPath") || !fs::exists(migration_resources::fromUtf8(result["reportPath"].get<std::string>())))
        std::cerr << "isolated migration diagnostics: " << result.dump() << '\n';
    ASSERT_FALSE(result["ok"].get<bool>()); ASSERT_FALSE(result["complete"].get<bool>());
    ASSERT_FALSE(result["errors"].empty()); ASSERT_TRUE(fs::exists(migration_resources::fromUtf8(result["reportPath"].get<std::string>())));
    ASSERT_EQ(f.db.getCardStorage()->count<CharacterCardRow>(), 0);
}
TEST(ReleaseMigration, GroupsKeepOffOnFirstMessageAndMapNativeReplySwitch) {
    ReleaseImportFixture f;
    std::ofstream out("old/user/ChatConf.dat", std::ios::binary);
    raw<int32_t>(out,1); raw<int64_t>(out,20001); raw<int64_t>(out,20001); raw<int16_t>(out,11); raw<int16_t>(out,2);
    str(out,"停用指令"); raw<char>(out,2); raw<int32_t>(out,1);
    str(out,"禁用回复"); raw<char>(out,2); raw<int32_t>(out,1); raw<int16_t>(out,-1); out.close();
    int groups=0; ASSERT_TRUE(importChatConf(f.db, "old/user", groups, &f.lua) > 0);
    AdapterManager manager(f.db); Message msg; msg.type=MessageType::kGroup; msg.platform="onebot_v11";
    msg.adapterId="audit-only"; msg.targetId="20001"; msg.senderId="10001"; msg.content="isolated"; manager.routeMessage(msg);
    ASSERT_EQ(accountGroupSetting(*f.db.getStorage(), msg.adapterId, msg.platform, msg.targetId, "enabled"), "0");
    ASSERT_EQ(accountGroupSetting(*f.db.getStorage(), msg.adapterId, msg.platform, msg.targetId, "replyDisabled"), "1");
    ImportOptions milky; milky.targetPlatform="milky"; milky.targetAdapterId="second";
    ASSERT_TRUE(importChatConf(f.db, "old/user", groups, &f.lua, milky) > 0);
    ASSERT_EQ(accountGroupSetting(*f.db.getStorage(), "second", "milky", msg.targetId, "enabled"), "0");
}
TEST(ReleaseMigration, SessionsAreStableAndRouletteOnlyRestoresUnrolledPrefix) {
    ReleaseImportFixture f;
    f.file("old/user/session/game.json", R"({"chats":[{"gid":20001}],"conf":{"rule":"DND5E","money":20},"roulette":{"3":{"copy":1,"pool":[1,2,3],"rest":1}},"decks":{"x":{"size":1}}})");
    std::map<std::string, LegacyLogContext> contexts; ImportResult report;
    ASSERT_EQ(importSessions("old", &f.lua, contexts, {}, &report), 1);
    const auto id=f.lua.confGet("game:20001","__session"), scope="game:session:"+id;
    ASSERT_EQ(json::parse(f.lua.confGet(scope,"__rou"))["3"]["left"].size(), size_t(1));
    ASSERT_EQ(f.lua.confGet(scope,"money"), "20"); ASSERT_FALSE(f.lua.confGet(scope,"__legacy_decks").empty());
    ASSERT_EQ(importSessions("old", &f.lua, contexts, {}, &report), 1);
    ASSERT_EQ(f.lua.confGet("game:20001","__session"), id); ASSERT_EQ(json::parse(f.lua.confGet("game:index","sessions")).size(), size_t(1));
}
TEST(ReleaseMigration, LogsAppendAndDoNotCollideBetweenSourceRoots) {
    ReleaseImportFixture f;
    const std::string first="Alice(10001) 2026-08-01 12:00:00\nfirst\n";
    f.file("old/user/log/20001_test.txt", first);
    std::map<std::string, LegacyLogContext> contexts; int messages=0; ImportResult report;
    ASSERT_EQ(importLogs(f.db,"old",contexts,messages,{},&report),1); ASSERT_EQ(messages,1);
    f.file("old/user/log/20001_test.txt",first+"Bob(10002) 2026-08-01 12:01:00\nsecond\n");
    ASSERT_EQ(importLogs(f.db,"old",contexts,messages,{},&report),1); ASSERT_EQ(messages,2);
    ASSERT_EQ(importLogs(f.db,"old",contexts,messages,{},&report),0);
    f.file("other/user/log/20001_test.txt",first);
    ASSERT_EQ(importLogs(f.db,"other",contexts,messages,{},&report),1);
    ASSERT_EQ(f.db.getLogStorage()->count<GameLogRow>(),2);
}
// 2026-10-17 B03：338 份日志里 3 份有残缺的 UTF-8 字节，以前整个日志阶段失败、一份都没导入
TEST(ReleaseMigration, BrokenUtf8LogIsRepairedAndDoesNotSinkTheOthers) {
    ReleaseImportFixture f;
    f.file("old/user/log/20001_a.txt", "Alice(10001) 2026-08-01 12:00:00\nfine\n");
    f.file("old/user/log/20001_b.txt", "Bob(10002) 2026-08-01 12:01:00\n\xEF\xBC\x9F)\r\n\xB8\xAA\xE5\xB0\x8F\n");
    f.file("old/user/log/20001_c.txt", "no header at all\n");
    std::map<std::string, LegacyLogContext> contexts; int messages = 0; ImportResult report;
    ASSERT_EQ(importLogs(f.db, "old", contexts, messages, {}, &report), 2);
    ASSERT_EQ(messages, 2);
    bool repaired = false, headerless = false;
    for (const auto& item : report.details) {
        if (item.name == "20001_b.txt" && item.status == "success") repaired = true;
        if (item.name == "20001_c.txt" && item.status == "failed") headerless = true;
    }
    ASSERT_TRUE(repaired);
    ASSERT_TRUE(headerless);
}
TEST(ReleaseMigration, ExistingCardsAndOtherLocaleHelpArePreserved) {
    ReleaseImportFixture f; card("old/user/PlayerCards.RDconf");
    CharacterCardRow row; row.userId="10001"; row.name=""; row.attrs=R"({"力量":99})"; f.db.getCardStorage()->insert(row);
    int users=0; ImportResult report; ASSERT_EQ(importCards(f.db,"old/user",users,{},&report),0);
    ASSERT_EQ(json::parse(f.db.getCardStorage()->get_all<CharacterCardRow>()[0].attrs)["力量"],99);
    I18nOverrideRow en; en.locale="en"; en.key="help.topic.test"; en.value="Owner English"; f.db.getStorage()->insert(en);
    f.file("old/conf/CustomHelp.json", R"({"test":"旧帮助"})"); ASSERT_EQ(importHelp(f.db,f.i18n,"old/conf"),1);
    ASSERT_EQ(f.db.getStorage()->count<I18nOverrideRow>(),2);
}
TEST(ReleaseMigration, KnownSelfDataAndMediaAreCopiedWithoutExecutingPlugins) {
    ReleaseImportFixture f; f.file("old/selfdata/plugin.json", R"({"saved":true})"); f.file("old/image/picture.png","synthetic");
    f.file("old/CardTemp/old.xml", "<old/>"); const auto report=importAncillaryResources("old");
    if (!fs::exists("data/self_data/plugin.json")) std::cerr << "isolated resource diagnostics: " << report.dump() << '\n';
    ASSERT_TRUE(fs::exists("data/self_data/plugin.json")); ASSERT_TRUE(fs::exists("image/picture.png"));
    ASSERT_EQ(report["preservedOnly"].size(),size_t(1));
}

TEST(ReleaseMigration, ScalarTypesAndNestedVariablesStayLossless) {
    ReleaseImportFixture f;
    std::ofstream out("old/user/UserConf.dat",std::ios::binary);
    raw<int32_t>(out,1); raw<int64_t>(out,10001); str(out,"Cfg"); raw<int16_t>(out,5);
    str(out,"decimal"); raw<char>(out,3); raw<double>(out,1.75);
    str(out,"flag"); raw<char>(out,1); raw<char>(out,0);
    str(out,"empty"); utf(out,""); str(out,"digits"); utf(out,"01");
    str(out,"table"); raw<char>(out,21); raw<int16_t>(out,1); str(out,"value"); raw<char>(out,2); raw<int32_t>(out,7);
    str(out,"END"); out.close();
    ASSERT_EQ(importUsers(f.db,"old/user",&f.lua),1);
    ASSERT_EQ(f.lua.confGet("u:10001","decimal"),"1.75"); ASSERT_EQ(f.lua.confGet("u:10001","flag"),"false");
    json value;
    ASSERT_TRUE(legacy_conf::decode(f.lua.confGet("u:10001","digits"),value)); ASSERT_EQ(value,"01");
    ASSERT_TRUE(legacy_conf::decode(f.lua.confGet("u:10001","empty"),value)); ASSERT_EQ(value,"");
    ASSERT_TRUE(legacy_conf::decode(f.lua.confGet("u:10001","table"),value)); ASSERT_EQ(value["value"],7);
}

TEST(ReleaseMigration, ProtectedProfileDoesNotGrantLuaHigherLegacyTrust) {
    ReleaseImportFixture f; PlayerProfileRow profile;
    profile.platform="onebot_v11"; profile.userId="10001"; profile.trustLevel=0; f.db.getStorage()->insert(profile);
    std::ofstream out("old/user/UserConf.dat",std::ios::binary);
    raw<int32_t>(out,1); raw<int64_t>(out,10001); str(out,"Cfg"); raw<int16_t>(out,1);
    str(out,"trust"); raw<char>(out,2); raw<int32_t>(out,4); str(out,"END"); out.close();
    ImportResult report; ASSERT_EQ(importUsers(f.db,"old/user",&f.lua,{},&report),1);
    ASSERT_EQ(f.db.getStorage()->get_all<PlayerProfileRow>()[0].trustLevel,0);
    ASSERT_TRUE(f.lua.confGet("u:10001","trust").empty()); ASSERT_TRUE(report.skipped>0);
}

TEST(ReleaseMigration, AliasedCardFieldsReadWithoutRewritingConflicts) {
    ReleaseImportFixture f;
    CharacterCardRow row; row.userId="10001"; row.name=""; row.attrs=R"({"STR":70,"HP":12})";
    f.db.getCardStorage()->insert(row); CharacterCardStore cards(f.db);
    ASSERT_EQ(cards.getAttr("10001","20001","力量").value(),70);
    ASSERT_EQ(cards.getAttr("10001","20001","STR").value(),70);
    ASSERT_EQ(cards.getAttrs("10001","20001").at("生命值"),12);
    ASSERT_TRUE(cards.exportCard("10001","")["data"].contains("STR"));
    cards.setAttr("10001","20001","力量",80);
    ASSERT_EQ(cards.getAttr("10001","20001","STR").value(),80);
    ASSERT_FALSE(cards.exportCard("10001","")["data"].contains("STR"));
}

TEST(ReleaseMigration, TypedCardCommandsEditDeleteClearExportAndSwitchTogether) {
    ReleaseImportFixture f; ASSERT_TRUE(f.i18n.load());
    CharacterCardStore cards(f.db); cards.createCard("10001","");
    cards.setValueByName("10001","","&伤害","3+4"); cards.setValueByName("10001","","小数",1.75);
    cards.setValueByName("10001","","职业","记者"); cards.setAttr("10001","20001","力量",60);
    DiceEngine engine(f.cfg); LocaleResolver resolver(f.db,f.cfg); CardDeck deck; AdapterManager adapters(f.db);
    CommandRouter router(f.db,f.cfg,engine,f.i18n,resolver,cards,deck,adapters);
    Message msg; msg.platform="onebot_v11"; msg.adapterId="test"; msg.type=MessageType::kGroup;
    msg.senderId="10001"; msg.senderName="Tester"; msg.targetId="20001";
    auto run=[&](const std::string& text){msg.content=text; return router.handleMessage(msg,Locale::kZhHans);};
    ASSERT_TRUE(run(".st show 小数").find("1.75")!=std::string::npos);
    ASSERT_TRUE(run(".st show 伤害").find("3+4")!=std::string::npos);
    const auto exported=run(".st export"); const auto marker=exported.find("json "); ASSERT_TRUE(marker!=std::string::npos);
    const auto original=cards.getValuesByName("10001","");
    ASSERT_FALSE(run(".st clr").empty()); ASSERT_TRUE(cards.getValuesByName("10001","").empty());
    ASSERT_FALSE(run(".st "+exported.substr(marker)).empty()); ASSERT_EQ(cards.getValuesByName("10001",""),original);
    ASSERT_FALSE(run(".st &伤害=9").empty()); ASSERT_EQ(cards.getValueByName("10001","","&伤害").value(),nlohmann::json("9"));
    ASSERT_TRUE(run(".r 伤害").find("9")!=std::string::npos);
    std::string js; ASSERT_TRUE(router.jsCardGetStr("onebot_v11","10001","20001","伤害",js)); ASSERT_EQ(js,"9");
    cards.createCard("10001","另一张卡"); cards.bindCard("10001","20001","另一张卡");
    ASSERT_FALSE(cards.getValueByName("10001","另一张卡","&伤害").has_value());
    ASSERT_FALSE(run(".st &伤害=5").empty());
    cards.bindCard("10001","20001",""); ASSERT_EQ(cards.getValueByName("10001","","&伤害").value(),nlohmann::json("9"));
    ASSERT_FALSE(run(".st del 伤害").empty()); ASSERT_TRUE(cards.getExpressionsByName("10001","").empty());
    ASSERT_FALSE(run(".st clr").empty()); ASSERT_TRUE(cards.getValuesByName("10001","").empty());
}

TEST(ReleaseMigration, NativeJsonExportPreservesConflictingRawNames) {
    ReleaseImportFixture f; CharacterCardStore cards(f.db);
    const json values={{"STR",70},{"力量",80},{"职业","记者"},{"小数",1.75},{"flags",legacy_conf::setValue(json::array({"x",1}))}};
    cards.replaceValuesByName("10001","first",values); cards.replaceValuesByName("10001","second",cards.getValuesByName("10001","first"));
    ASSERT_EQ(cards.getValuesByName("10001","second"),values);
    ASSERT_EQ(cards.getAttrByName("10001","second","STR").value(),70);
    ASSERT_EQ(cards.getAttrByName("10001","second","力量").value(),80);
}

TEST(ReleaseMigration, BothSourceWidthsValidateBeforeCommittingAndFunctionsAreReported) {
    for(const bool source64:{true,false}) {
        ReleaseImportFixture f;
        std::ofstream out("old/user/UserConf.dat",std::ios::binary);
        raw<int32_t>(out,1); raw<int64_t>(out,10001); str(out,"Cfg"); raw<int16_t>(out,2);
        str(out,"table"); raw<char>(out,21); raw<int16_t>(out,1); str(out,"callback"); raw<char>(out,6);
        if(source64)raw<uint64_t>(out,4); else raw<uint32_t>(out,4); out << "code";
        str(out,"flags"); raw<char>(out,8); if(source64)raw<uint64_t>(out,1); else raw<uint32_t>(out,1);
        raw<char>(out,2); raw<int32_t>(out,7); str(out,"END"); out.close();
        ReplyManager replies(f.db,f.cfg); const auto report=runImport(f.db,f.cfg,f.i18n,replies,"old",nullptr,&f.lua);
        ASSERT_TRUE(report["ok"].get<bool>()); ASSERT_FALSE(report["complete"].get<bool>()); ASSERT_TRUE(report["errors"].empty());
        ASSERT_EQ(report["profiles"],1); ASSERT_FALSE(report["diagnostics"].empty());
        json flags; ASSERT_TRUE(legacy_conf::decode(f.lua.confGet("u:10001","flags"),flags)); ASSERT_TRUE(legacy_conf::isSet(flags));
        json table; ASSERT_TRUE(legacy_conf::decode(f.lua.confGet("u:10001","table"),table)); ASSERT_TRUE(legacy_conf::isFunction(table["callback"]));
    }
}

TEST(ReleaseMigration, ConsoleClocksAreNotReportedAsCompleted) {
    ReleaseImportFixture f; f.file("old/conf/console.yaml","master: 10001\nclock:\n  - moment: '04:00'\n    task: 'off'\n");
    ReplyManager replies(f.db,f.cfg); const auto report=runImport(f.db,f.cfg,f.i18n,replies,"old",nullptr,&f.lua);
    ASSERT_TRUE(report["ok"].get<bool>()); ASSERT_FALSE(report["complete"].get<bool>());
    ASSERT_FALSE(report["diagnostics"].empty());
}

TEST(ReleaseMigration, SelfNameAndSelfCallMoveIntoSettings) {
    // 原版的自称在 CustomMsg.json，DiceNext 的在设置里。以前进了无效文本，迁完自称就成了 QQ 昵称
    ReleaseImportFixture f; ASSERT_TRUE(f.i18n.load());
    f.file("old/conf/CustomMsg.json", R"({"strSelfName":" 希亚 ","strSelfCall":"&strSelfName","strRollFumble":"大失败"})");
    int orphans = 0; json details = json::array();
    ASSERT_EQ(importCustomMsg(f.db, f.i18n, "old/conf", orphans, &details, {}, &f.cfg), 2);
    ASSERT_EQ(f.cfg.get<std::string>("dice/self_name", std::string()), "希亚");
    // "&strSelfName" 是原版默认的「同名」，不是一个名字
    ASSERT_TRUE(f.cfg.get<std::string>("dice/self_call", std::string()).empty());
    ASSERT_EQ(orphans, 0);
    ASSERT_FALSE(f.i18n.hasConfigured(Locale::kZhHans, "legacy.strSelfName"));
}

TEST(ReleaseMigration, SelfNameKeepsExistingSettingsAndLeavesTemplatesForReview) {
    ReleaseImportFixture f; ASSERT_TRUE(f.i18n.load());
    f.cfg.set<std::string>("dice/self_name", "新名字");
    f.file("old/conf/CustomMsg.json", R"({"strSelfName":"旧名字","strSelfCall":"{self}"})");
    int orphans = 0; json details = json::array();
    ASSERT_EQ(importCustomMsg(f.db, f.i18n, "old/conf", orphans, &details, {}, &f.cfg), 0);
    ASSERT_EQ(f.cfg.get<std::string>("dice/self_name", std::string()), "新名字");
    ASSERT_TRUE(f.cfg.get<std::string>("dice/self_call", std::string()).empty());
    ASSERT_EQ(orphans, 2);
    ASSERT_EQ(f.i18n.tr(Locale::kZhHans, "legacy.strSelfName"), "旧名字");
    ASSERT_EQ(f.i18n.tr(Locale::kZhHans, "legacy.strSelfCall"), "{self}");
    ImportOptions overwrite; overwrite.overwriteData = true; orphans = 0;
    ASSERT_EQ(importCustomMsg(f.db, f.i18n, "old/conf", orphans, nullptr, overwrite, &f.cfg), 1);
    ASSERT_EQ(f.cfg.get<std::string>("dice/self_name", std::string()), "旧名字");
}

TEST(ReleaseMigration, FullImportCarriesTheSelfName) {
    ReleaseImportFixture f; ASSERT_TRUE(f.i18n.load());
    f.file("old/conf/CustomMsg.json", R"({"strSelfName":"希亚","strSelfCall":"小希"})");
    ReplyManager replies(f.db, f.cfg);
    const auto report = runImport(f.db, f.cfg, f.i18n, replies, "old", nullptr, &f.lua);
    ASSERT_TRUE(report["ok"].get<bool>());
    ASSERT_EQ(f.cfg.get<std::string>("dice/self_name", std::string()), "希亚");
    ASSERT_EQ(f.cfg.get<std::string>("dice/self_call", std::string()), "小希");
    ConfigManager reread{"config"}; ASSERT_TRUE(reread.load());
    ASSERT_EQ(reread.get<std::string>("dice/self_name", std::string()), "希亚");
}

TEST(ReleaseMigration, ScopedMastersNotificationsAndSharedNoticeOffAreConsistent) {
    ReleaseImportFixture f; ASSERT_TRUE(f.i18n.load());
    f.file("old/conf/console.yaml","master: 10001\n"); f.file("old/conf/NoticeList.json",R"([{"gid":20001,"type":15}])");
    ImportOptions opts; opts.targetAdapterId="selected"; ASSERT_EQ(importMasters(f.cfg,"old/conf",nullptr,opts),1);
    ASSERT_EQ(importNotices(f.cfg,"old/conf",opts),1);
    ASSERT_EQ(f.cfg.get<json>("dice/masters",json())[0]["adapter_id"],"selected");
    CharacterCardStore cards(f.db); DiceEngine engine(f.cfg); LocaleResolver resolver(f.db,f.cfg); CardDeck deck; AdapterManager adapters(f.db);
    CommandRouter router(f.db,f.cfg,engine,f.i18n,resolver,cards,deck,adapters);
    Message msg; msg.platform="onebot_v11"; msg.type=MessageType::kGroup; msg.senderId="10001"; msg.targetId="20001"; msg.adapterId="other";
    msg.content=".system stats"; const auto denied=router.handleMessage(msg,Locale::kZhHans);
    ASSERT_TRUE(denied.empty() || denied==f.i18n.tr(Locale::kZhHans,"gate.not_master"));
    msg.adapterId="selected"; msg.content=".notice status"; ASSERT_TRUE(router.handleMessage(msg,Locale::kZhHans).find("15")!=std::string::npos);
    auto notice=f.cfg.get<json>("dice/notice",json()); notice["windows"][0].erase("adapter_id"); f.cfg.set("dice/notice",notice);
    msg.content=".notice off"; ASSERT_FALSE(router.handleMessage(msg,Locale::kZhHans).empty());
    ASSERT_TRUE(f.cfg.get<json>("dice/notice",json())["windows"].empty());
}

TEST(ReleaseMigration, PriorAndCopiedSessionSourcesRequireConfirmationAndReuseCodes) {
    ReleaseImportFixture f; const std::string original=R"({"name":"旧团","chats":[{"gid":20001}],"conf":{"money":20}})";
    f.file("old/user/session/game.json",original);
    f.lua.confSet("game:index","sessions",R"({"keep-code":"legacy-prior"})");
    f.lua.confSet("game:session:legacy-prior","__name","旧团"); f.lua.confSet("game:session:legacy-prior","__areas","[\"20001\"]");
    f.lua.confSet("game:session:legacy-prior","__code","keep-code"); f.lua.confSet("game:20001","__session","legacy-prior");
    std::map<std::string,LegacyLogContext> contexts; ImportResult detail;
    ASSERT_EQ(importSessions("old",&f.lua,contexts,{},&detail),0); ASSERT_EQ(detail.confirmations.size(),size_t(1));
    ASSERT_EQ(json::parse(f.lua.confGet("game:index","sessions")).size(),size_t(1));
    ImportOptions opts; opts.sessionMappings[detail.confirmations[0]["key"]]="legacy-prior";
    ASSERT_EQ(importSessions("old",&f.lua,contexts,opts,&detail),1); ASSERT_EQ(f.lua.confGet("game:session:legacy-prior","__code"),"keep-code");
    f.file("copy/user/session/game.json",original); ImportResult copied;
    ASSERT_EQ(importSessions("copy",&f.lua,contexts,{},&copied),0); ASSERT_EQ(copied.confirmations.size(),size_t(1));
    opts.sessionMappings[copied.confirmations[0]["key"]]="legacy-prior";
    ASSERT_EQ(importSessions("copy",&f.lua,contexts,opts,&copied),1);
    ASSERT_EQ(importSessions("copy",&f.lua,contexts,{},&copied),1);
    ASSERT_EQ(json::parse(f.lua.confGet("game:index","sessions")).size(),size_t(1));
}

TEST(ReleaseMigration, SessionDeckImportsKeepDepletionAndDoNotRefillOnRepeatedImport) {
    ReleaseImportFixture f;
    f.file("old/user/session/game.json",R"({"name":"Cards","chats":[{"gid":20001}],"decks":{"X":{"meta":["spent","left"],"idxs":[1,0],"size":1}}})");
    std::map<std::string,LegacyLogContext> contexts; ImportResult detail;
    ASSERT_EQ(importSessions("old",&f.lua,contexts,{},&detail),1);
    const auto session=f.lua.confGet("game:20001","__session"), scope="game:session:"+session;
    auto decks=json::parse(f.lua.confGet(scope,"__decks"));
    ASSERT_EQ(decks["X"]["size"],1); ASSERT_EQ(decks["X"]["idxs"][0],1);
    std::mt19937_64 rng(1); ASSERT_EQ(session_deck::draw(decks["X"],rng).text,std::string("left"));
    f.lua.confSet(scope,"__decks",decks.dump());
    ASSERT_EQ(importSessions("old",&f.lua,contexts,{},&detail),1);
    ASSERT_EQ(json::parse(f.lua.confGet(scope,"__decks"))["X"]["size"],0);
    ImportOptions options; options.overwriteData=true;
    ASSERT_EQ(importSessions("old",&f.lua,contexts,options,&detail),1);
    ASSERT_EQ(json::parse(f.lua.confGet(scope,"__decks"))["X"]["size"],1);
    ASSERT_TRUE(!f.lua.confGet(scope,"__legacy_decks").empty());
}

TEST(ReleaseMigration, InvalidSessionDecksRemainArchivedWithoutPartialActivation) {
    ReleaseImportFixture f;
    f.file("old/user/session/game.json",R"({"name":"Cards","chats":[{"gid":20001}],"decks":{"ok":{"meta":["card"],"idxs":[0],"size":1},"bad":{"meta":["card"],"idxs":[99],"size":1}}})");
    std::map<std::string,LegacyLogContext> contexts; ImportResult detail;
    ASSERT_EQ(importSessions("old",&f.lua,contexts,{},&detail),1);
    const auto scope="game:session:"+f.lua.confGet("game:20001","__session");
    ASSERT_TRUE(f.lua.confGet(scope,"__decks").empty());
    const auto archived=json::parse(f.lua.confGet(scope,"__legacy_decks"));
    ASSERT_TRUE(archived.contains("ok")); ASSERT_TRUE(archived.contains("bad"));
}

TEST(ReleaseMigration, RootMediaIsBackedUpAndRestoredWithoutLosingOldFormatCompatibility) {
    ReleaseImportFixture f; f.file("image/picture.png","original"); f.file("data/legacy_assets/deck/picture.png","deck");
    fs::path archive; std::string error; ASSERT_TRUE(backup::createArchive(f.db,"config",archive,error,backup::Selection::full()));
    const auto contents=backup::runArchive(backup::archiveListCommand(archive)); ASSERT_EQ(contents.exitCode,0);
    auto normalized=contents.output; std::replace(normalized.begin(),normalized.end(),'\\','/');
    ASSERT_TRUE(normalized.find("image/picture.png")!=std::string::npos);
    f.file("image/picture.png","changed"); f.file("image/new.png","new");
    ASSERT_TRUE(backup::stageStoredRestore(archive.filename().string(),false,error));
    f.db.close(); ASSERT_TRUE(backup::applyPendingRestore(error));
    ASSERT_EQ(readFile("image/picture.png"),"original"); ASSERT_FALSE(fs::exists("image/new.png"));
    ASSERT_EQ(readFile("data/legacy_assets/deck/picture.png"),"deck");
    ASSERT_TRUE(backup::allowedArchivePath("image/test.png")); ASSERT_FALSE(backup::allowedArchivePath("image/../secret"));
}


// 2026-10-17 B03 46230：旧版 Dice! 群名里的 emoji 是 GB18030 四字节序列，按 GBK 解会让整份
// ChatConf.dat 判为无法读取，40 个群的设置一个都没迁过来
TEST(LegacyGbk, FourByteGb18030EmojiInGroupNamesDecodes) {
    const std::string clover = "\xD3\xCE\xDF\xAE\xA1\xA2\xCC\xAB\xC9\xCF\xD5\xD5\xC9\xD5\xD6\xD0\xD2\xA9\xB2\xC4\xA1\xA2\x94\x39\xB6\x32";
    ASSERT_EQ(dice::legacy2::gbkToUtf8(clover),
              "\xE6\xB8\xB8\xE5\xBC\x8B\xE3\x80\x81\xE5\xA4\xAA\xE4\xB8\x8A\xE7\x85\xA7\xE7\x83\xA7\xE4\xB8\xAD\xE8\x8D\xAF\xE6\x9D\x90\xE3\x80\x81\xF0\x9F\x8D\x80");
    const std::string satellite = "\xC6\xFA\xD4\xBC\xC9\xE7\xBB\xE1\x95\x30\x96\x36\x84\x31\x82\x35";
    ASSERT_EQ(dice::legacy2::gbkToUtf8(satellite),
              "\xE5\xBC\x83\xE7\xBA\xA6\xE7\xA4\xBE\xE4\xBC\x9A\xF0\x9F\x9B\xB0\xEF\xB8\x8F");
}

TEST(LegacyGbk, PlainGbkIsUnchangedAndGarbageStillFails) {
    ASSERT_EQ(dice::legacy2::gbkToUtf8("\xD2\xD1\xC8\xEB\xC8\xBA"), "\xE5\xB7\xB2\xE5\x85\xA5\xE7\xBE\xA4");  // 已入群
    ASSERT_EQ(dice::legacy2::gbkToUtf8("Room NO.9"), "Room NO.9");
    bool threw = false;
    try { dice::legacy2::gbkToUtf8(std::string("\xFF\xFF\xFF", 3)); } catch (const std::exception&) { threw = true; }
    ASSERT_TRUE(threw);  // 布局探测仍然靠它拒绝错位读出的乱码
}
