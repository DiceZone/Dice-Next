#include "test_framework.h"
#include "legacy_import_test_support.h"
#include "core/help_catalog.h"
#include "core/command_router.h"
#include "storage/legacy_import_v2.h"

namespace {
using namespace dice;
struct HelpLibraryFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("dicenext-help-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Database db;
    ConfigManager cfg{u8str(root / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck decks;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, decks, adapters};
    Message message;
    HelpLibraryFixture() {
        if (!std::filesystem::create_directories(root / "old-conf") || !db.open(u8str(root / "app.db")))
            throw std::runtime_error("cannot create isolated help fixture");
        cfg.resetDefault();
        if (!i18n.load()) throw std::runtime_error("cannot load help resources");
        message.platform = "onebot_v11"; message.adapterId = "help-library-test";
        message.senderId = "help-reader"; message.senderName = "读者";
        message.selfId = "fake-bot"; message.targetId = "help-group"; message.type = MessageType::kGroup;
    }
    ~HelpLibraryFixture() { db.close(); } // Only synthetic data; never starts an adapter/listener.
    std::string run(const std::string& command, Locale loc = Locale::kZhHans) {
        message.content = command; return router.handleMessage(message, loc);
    }
};
const I18n::HelpTemplate* findHelpTemplate(const std::vector<I18n::HelpTemplate>& entries, const std::string& key) {
    for (const auto& entry : entries) if (entry.key == key) return &entry;
    return nullptr;
}
}

TEST(HelpLibrary, LegacyCustomTopicsAreDiscoverableCallableAndSurviveReload) {
    HelpLibraryFixture f;
    const std::string key = "help.topic.关于Master";
    const std::string text = "旧版介绍\n{sample:保留原文|保留原文}";
    { std::ofstream out(f.root / "old-conf" / "CustomHelp.json", std::ios::binary);
      out << json{{"关于Master", text}, {"关于御主", "另一条介绍"}}; }
    ASSERT_EQ(legacyv2::importHelp(f.db, f.i18n, f.root / "old-conf"), 2);
    const auto templates = f.i18n.helpTemplates(Locale::kZhHans);
    const auto* saved = findHelpTemplate(templates, key);
    ASSERT_TRUE(saved != nullptr); ASSERT_EQ(saved->value, text);
    ASSERT_FALSE(saved->builtin); ASSERT_TRUE(saved->customized);
    ASSERT_TRUE(saved->locale == Locale::kZhHans);
    ASSERT_EQ(f.run(".help 关于master"), "旧版介绍\n保留原文");
    ASSERT_EQ(f.run(".help 关于御主"), "另一条介绍");
    ASSERT_EQ(f.run(".helpdoc 关于Master"), "旧版介绍\n保留原文");
    bool found = false;
    for (const auto& entry : f.router.allHelp(Locale::kZhHans)) if (entry.key == "关于Master") {
        found = true; ASSERT_EQ(entry.source, "custom"); ASSERT_EQ(entry.content, text); ASSERT_EQ(entry.i18nKey, key);
    }
    ASSERT_TRUE(found);
    I18n restarted{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    ASSERT_TRUE(restarted.load());
    for (const auto& row : f.db.getStorage()->get_all<I18nOverrideRow>())
        restarted.setOverride(localeFromString(row.locale), row.key, row.value, contentFormatFromString(row.format));
    const auto reloaded = restarted.helpTemplates(Locale::kZhHans);
    const auto* restored = findHelpTemplate(reloaded, key);
    ASSERT_TRUE(restored != nullptr); ASSERT_EQ(restored->value, text);
    ASSERT_EQ(restarted.tr(Locale::kZhHans, key), "旧版介绍\n保留原文");
}

TEST(HelpLibrary, ActualBundleTopicsAndBuiltinOverridesKeepTheirOrigin) {
    HelpLibraryFixture f;
    f.i18n.setOverride(Locale::kZhHans, "help.topic.r", "自定义掷骰帮助", ContentFormat::kMarkdown);
    const auto templates = f.i18n.helpTemplates(Locale::kZhHans);
    for (const auto& name : {"r", "bind", "info", "cloud", "game"}) {
        const auto* entry = findHelpTemplate(templates, std::string("help.topic.") + name);
        ASSERT_TRUE(entry != nullptr); ASSERT_TRUE(entry->builtin);
    }
    const auto* roll = findHelpTemplate(templates, "help.topic.r");
    ASSERT_TRUE(roll->customized); ASSERT_TRUE(roll->format == ContentFormat::kMarkdown);
    f.i18n.setOverride(Locale::kZhHans, "help.topic.R", "case variant must not shadow the builtin");
    ASSERT_EQ(f.run(".help r"), "自定义掷骰帮助");
    const auto ordered = f.i18n.helpTemplates(Locale::kZhHans);
    HelpCatalog catalog(true);
    for (const auto& text : ordered) catalog.add({text.key.substr(11), text.value, text.builtin ? "builtin" : "custom"});
    const auto duplicate = catalog.select("case variant", "custom");
    ASSERT_EQ(duplicate.size(), size_t(1)); ASSERT_TRUE(duplicate[0]->shadowed);
    f.i18n.clearOverride(Locale::kZhHans, "help.topic.r");
    const auto reset = f.i18n.helpTemplates(Locale::kZhHans);
    ASSERT_FALSE(findHelpTemplate(reset, "help.topic.r")->customized);
    ASSERT_EQ(f.run(".help info"), f.i18n.tr(Locale::kZhHans, "help.topic.info"));
}

TEST(HelpLibrary, CustomAliasesChainAcrossSourcesWithoutEscapingScopeOrLooping) {
    HelpLibraryFixture f;
    f.i18n.setOverride(Locale::kZhHans, "help.topic.入口", "&二级");
    f.i18n.setOverride(Locale::kZhHans, "help.topic.二级", "&目标");
    f.i18n.setOverride(Locale::kZhHans, "help.topic.目标", "你好 {uid}");
    ASSERT_EQ(f.run(".help 入口"), "你好 help-reader");
    ASSERT_EQ(f.router.renderReplyWithGroups(f.message, "{help:入口}", {}), "你好 help-reader");
    f.i18n.setOverride(Locale::kZhHans, "help.topic.目标", "&入口");
    ASSERT_FALSE(f.run(".help 入口").empty());
    f.i18n.setOverride(Locale::kZhHans, "help.topic.二级", "&missing-exact-topic");
    ASSERT_TRUE(f.run(".help 入口").find("missing-exact-topic") != std::string::npos);
    setAccountGroupSetting(*f.db.getStorage(), f.message.adapterId, f.message.platform,
        f.message.targetId, f.message.targetId, "helpScope", "restricted-source");
    ASSERT_TRUE(f.run(".help 入口").find("你好") == std::string::npos);
}

TEST(HelpLibrary, ReplyMacrosAndHelpTreatInsertedContentAsData) {
    HelpLibraryFixture f;
    f.message.senderName = "{ran:9~9}";
    f.i18n.setOverride(Locale::kZhHans, "help.topic.安全", "你好 {nick}");
    ASSERT_EQ(f.run(".help 安全"), "你好 {ran:9~9}");
    ASSERT_EQ(f.router.renderReplyWithGroups(f.message, "{var:n={ran:5~5}}{case:n?5=好&else=坏}:{n}", {}), "好:5");
    ASSERT_EQ(f.router.renderReplyWithGroups(f.message, "{var:n={$1}}{n}", {"whole", "{ran:9~9}"}), "{ran:9~9}");
}

TEST(HelpLibrary, DynamicDeckDirectoriesAreScopedBoundedAndNotReparsed) {
    HelpLibraryFixture f;
    const auto source = f.root / "decks"; std::filesystem::create_directories(source);
    { std::ofstream out(source / "cards.json"); out << R"({"公开牌堆":["a"],"_隐藏牌堆":["b"],"{ran:9~9}":["c"]})"; }
    f.decks.loadDir(u8str(source));
    f.i18n.setOverride(Locale::kZhHans, "help.topic.目录", "{list_all_deck}");
    const auto result = f.run(".help 目录");
    ASSERT_TRUE(result.find("公开牌堆") != std::string::npos);
    ASSERT_TRUE(result.find("_隐藏牌堆") == std::string::npos);
    ASSERT_TRUE(result.find("{ran:9~9}") != std::string::npos);
    f.router.setTemplateDirectory([](const Message& message, bool orders) {
        return std::vector<std::string>{message.adapterId + (orders ? "-order" : "-key")};
    });
    ASSERT_EQ(f.router.templateContextValue(f.message,"list_extern_order").value_or("MISSING"), "help-library-test-order");
    ReplyManager replies(f.db, f.cfg);
    ReplyRule rule; rule.matchType=MatchType::kKeyword; rule.matchContent="secret-key"; rule.replyContent="ok";
    rule.channelScope="account"; rule.channelTarget="help-library-test";
    rule.scopeMode="allow"; rule.scopeIds="help-group"; rule.scopeUsersMode="allow"; rule.scopeUsers="help-reader";
    replies.addRule(rule);
    ASSERT_TRUE(replies.directoryTriggers({"onebot_v11","help-group","help-reader","help-library-test"}, false) == std::vector<std::string>({"secret-key"}));
    ASSERT_TRUE(replies.directoryTriggers({"onebot_v11","other","help-reader","help-library-test"}, false).empty());
    ASSERT_TRUE(replies.directoryTriggers({"onebot_v11","help-group","other","help-library-test"}, false).empty());
    ASSERT_TRUE(replies.directoryTriggers({"onebot_v11","help-group","help-reader","other"}, false).empty());
}

TEST(HelpLibrary, RawLanguageFormatEmptyAndPersonaLayersArePreserved) {
    HelpLibraryFixture f;
    const std::string key = "help.topic.library-locale-test";
    f.i18n.setOverride(Locale::kZhHans, key, "**中文**\n{sample:甲|乙}", ContentFormat::kMarkdown);
    const auto fallback = f.i18n.helpTemplates(Locale::kEn);
    const auto* chinese = findHelpTemplate(fallback, key);
    ASSERT_TRUE(chinese != nullptr); ASSERT_TRUE(chinese->locale == Locale::kZhHans);
    ASSERT_EQ(chinese->value, "**中文**\n{sample:甲|乙}");
    ASSERT_TRUE(chinese->format == ContentFormat::kMarkdown);
    f.i18n.setOverride(Locale::kEn, key, "English");
    const auto english = f.i18n.helpTemplates(Locale::kEn);
    ASSERT_EQ(findHelpTemplate(english, key)->value, "English");
    ASSERT_TRUE(findHelpTemplate(english, key)->locale == Locale::kEn);
    f.i18n.setPersonaBundles(97, Locale::kEn, {{key, "Persona"}, {"help.topic.persona-only-topic", "Persona topic"}});
    {
        auto scope = f.i18n.scopedPersona(97);
        const auto management = f.i18n.helpTemplates(Locale::kEn);
        const auto runtime = f.i18n.helpTemplates(Locale::kEn, true);
        ASSERT_EQ(findHelpTemplate(management, key)->value, "English");
        ASSERT_TRUE(findHelpTemplate(management, "help.topic.persona-only-topic") == nullptr);
        ASSERT_EQ(findHelpTemplate(runtime, key)->value, "Persona");
        ASSERT_TRUE(findHelpTemplate(runtime, "help.topic.persona-only-topic") != nullptr);
    }
    f.i18n.setOverride(Locale::kEn, key, "");
    const auto empty = f.i18n.helpTemplates(Locale::kEn);
    ASSERT_EQ(findHelpTemplate(empty, key)->value, "");
    ASSERT_TRUE(findHelpTemplate(empty, key)->locale == Locale::kEn);
    f.i18n.clearOverride(Locale::kEn, key);
    const auto afterDelete = f.i18n.helpTemplates(Locale::kEn);
    ASSERT_TRUE(findHelpTemplate(afterDelete, key)->locale == Locale::kZhHans);
    f.i18n.clearOverride(Locale::kZhHans, key);
    ASSERT_TRUE(findHelpTemplate(f.i18n.helpTemplates(Locale::kEn), key) == nullptr);
}

TEST(HelpLibrary, SourceKindsCoverCustomBuiltinPluginsPacksAndUnknown) {
    ASSERT_EQ(helpSourceKind("builtin"), "builtin");
    ASSERT_EQ(helpSourceKind("custom"), "custom"); ASSERT_EQ(helpSourceKind("file:旧帮助"), "custom");
    ASSERT_EQ(helpSourceKind("plugin:dailynews"), "plugin"); ASSERT_EQ(helpSourceKind("lua:查询"), "plugin");
    ASSERT_EQ(helpSourceKind("rule:COC7"), "rule"); ASSERT_EQ(helpSourceKind("helpdoc:COC7"), "helpdoc");
    ASSERT_EQ(helpSourceKind("future-source:test"), "other");
}

TEST(HelpLibrary, FilteringKeepsShadowingAndEmptyRecordsAndComposesWithSearch) {
    HelpCatalog catalog(true);
    catalog.add({"Topic", "默认", "builtin"});
    catalog.add({"topic", "CUSTOM detail", "custom"});
    catalog.add({"指南", "正文", "file:指南"});
    catalog.add({"空文档", "", "custom"});
    catalog.add({"topic", "plugin", "lua:help-mod"});
    catalog.add({"other", "自定义", "plugin:js-mod"});
    const auto custom = catalog.select("", "custom");
    ASSERT_EQ(custom.size(), size_t(3)); ASSERT_TRUE(custom[0]->shadowed);
    ASSERT_EQ(catalog.select("DETAIL", "custom", "custom").size(), size_t(1));
    ASSERT_EQ(catalog.select("正文", "custom", "file:指南").size(), size_t(1));
    ASSERT_TRUE(catalog.select("", "plugin", "file:指南").empty());
    ASSERT_EQ(catalog.select("", "plugin").size(), size_t(2));
    ASSERT_EQ(catalog.select("", "custom", "custom").size(), size_t(2));
    HelpCatalog runtime(false);
    runtime.add({"Topic", "默认", "builtin"}); runtime.add({"topic", "custom", "custom"});
    runtime.add({"空文档", "", "custom"});
    ASSERT_TRUE(runtime.select("", "custom").empty());
}

TEST(HelpLibrary, FiltersCountAcrossLargePluginLibrariesNotJustTheCurrentPage) {
    HelpCatalog catalog(true);
    for (int i = 0; i < 20010; ++i)
        catalog.add({"topic-" + std::to_string(i), "detail", i % 2 ? "lua:large" : "plugin:large"});
    catalog.add({"尾页自定义", "最后一条", "custom"});
    const auto plugins = catalog.select("", "plugin");
    ASSERT_EQ(plugins.size(), size_t(20010));
    ASSERT_EQ(catalog.select("", "plugin", "lua:large").size(), size_t(10005));
    ASSERT_EQ(catalog.select("", "custom").size(), size_t(1));
    ASSERT_EQ(catalog.select("最后", "custom")[0]->key, "尾页自定义");
}

TEST(HelpLibrary, RulePackEmbeddedDocumentsKeepTheirOriginAndCompatibleSourceName) {
    HelpLibraryFixture f;
    const auto dir = f.root / "rule-helpdoc";
    std::filesystem::create_directory(dir);
    { std::ofstream out(dir / "help.json", std::ios::binary);
      out << json{{"mod", "速查"}, {"helpdoc", {{"规则词条", "原文"}}}}; }
    struct ScopedHelpDocs {
        std::unique_lock<std::shared_mutex> lock{CommandRouter::helpDocLock()};
        std::vector<CommandRouter::HelpDocItem> previous = std::move(CommandRouter::helpDocs());
        ~ScopedHelpDocs() { CommandRouter::helpDocs() = std::move(previous); }
    } scope;
    ASSERT_EQ(CommandRouter::scanHelpDocDir(u8str(dir), "我的规则"), 1);
    const auto& item = CommandRouter::helpDocs().front();
    ASSERT_EQ(item.rulePack, "我的规则"); ASSERT_EQ(item.pack, "我的规则/速查");
    HelpDocumentEntry entry{item.topic, item.content, "helpdoc:" + item.pack};
    entry.kind = item.rulePack.empty() ? "helpdoc" : "rule";
    HelpCatalog catalog(true); catalog.add(std::move(entry));
    ASSERT_EQ(catalog.select("", "rule").size(), size_t(1));
    ASSERT_EQ(catalog.select("", "rule")[0]->source, "helpdoc:我的规则/速查");
    ASSERT_TRUE(catalog.select("", "helpdoc").empty());
}
