#include "test_framework.h"
#include "core/command_router.h"
#include "core/plugin_command_priority.h"
#include "core/identity/identity_binding.h"
#include "core/mod/js_plugin_manager.h"
#include "storage/group_account_settings.h"
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dice;
namespace {
struct ShortcutTemp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-shortcuts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ShortcutTemp() { std::filesystem::create_directory(path); }
    ~ShortcutTemp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct ShortcutFixture {
    ShortcutTemp temp;
    Database db;
    ConfigManager cfg{u8str(temp.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;
    ShortcutFixture() {
        if (!db.open(u8str(temp.path / "test.db")) || !i18n.load()) throw std::runtime_error("shortcut fixture failed");
        cfg.resetDefault();
        msg.platform = "onebot_v11"; msg.adapterId = "shortcut-test";
        msg.senderId = "1000"; msg.senderName = "玩家"; msg.selfId = "9000";
        msg.targetId = "2000"; msg.type = MessageType::kGroup; msg.extra = json::object();
    }
    std::string run(const std::string& text, Locale loc = Locale::kZhHans,
                    std::optional<ContentFormat> format = ContentFormat::kPlainText) {
        msg.content = text;
        // Match production OneBot's explicit plain-text capture by default;
        // tests composing their own capture pass nullopt instead of resetting it.
        if (format) I18n::beginOutboundCapture(*format);
        auto reply = router.handleMessage(msg, loc);
        if (format) (void)I18n::endOutboundCapture();
        return reply;
    }
    void setting(const std::string& key, const std::string& value) {
        setAccountGroupSetting(*db.getStorage(), msg.adapterId, msg.platform, msg.targetId, msg.targetId, key, value);
    }
    void master() { cfg.set<json>("dice/masters", json::array({{{"platform", msg.platform}, {"adapter_id", msg.adapterId}, {"id", msg.senderId}}})); }
    void privateChat() { msg.type = MessageType::kPrivate; msg.targetId = msg.senderId; }
};
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}

TEST(CommandShortcuts, ParserFlagsCompactInvocationAndReservedNames) {
    auto args = shortcut::management("--my 攻击 .r 1 --foo=bar");
    ASSERT_TRUE(args.personal); ASSERT_EQ(args.action, "攻击"); ASSERT_EQ(args.tail, ".r 1 --foo=bar");
    args = shortcut::management("del --my 攻击");
    ASSERT_TRUE(args.personal); ASSERT_EQ(args.action, "del"); ASSERT_EQ(args.tail, "攻击");
    args = shortcut::management("say .plugincmd \"--my\" --flag");
    ASSERT_FALSE(args.personal); ASSERT_EQ(args.tail, ".plugincmd \"--my\" --flag");
    ASSERT_EQ(*shortcut::invocation("&攻击 60"), "攻击 60");
    ASSERT_EQ(*shortcut::invocation("a 攻击"), "攻击");
    ASSERT_EQ(*shortcut::invocation("a攻击"), "攻击");
    for (const auto* name : {"alias", "ai", "ak", "admin", "apple"}) ASSERT_FALSE(shortcut::invocation(name).has_value());
    ASSERT_TRUE(plugin_command_priority::isReservedCoreCommand("a"));
    ASSERT_TRUE(plugin_command_priority::isReservedCoreCommand("&"));
    ASSERT_FALSE(plugin_command_priority::isReservedCoreCommand("apple"));
    ASSERT_FALSE(shortcut::validName(std::string(81, 'a')));
    ASSERT_FALSE(shortcut::validName("two words"));
    ASSERT_FALSE(shortcut::validCommand(".r 1\n.admin list"));
}

TEST(CommandShortcuts, OrdinaryMemberCRUDAndRealBuiltinDispatch) {
    ShortcutFixture f;
    ASSERT_TRUE(contains(f.run(".alias 答案 .r 42"), "已添加群"));
    ASSERT_EQ(shortcut::list(f.db, f.msg, false).at("答案"), ".r 42");
    ASSERT_TRUE(contains(f.run(".&答案"), "※ 群快捷指令 .r 42"));
    ASSERT_TRUE(f.router.lastReplyWasRoll());
    ASSERT_TRUE(f.router.lastShortcutHandled());
    ASSERT_TRUE(contains(f.run(".a 答案 测试"), ".r 42 测试"));
    ASSERT_TRUE(f.router.lastReplyWasRoll());
    ASSERT_TRUE(contains(f.run(".alias 答案 .r 43"), "已替换群"));
    ASSERT_EQ(shortcut::list(f.db, f.msg, false).size(), size_t(1));
    ASSERT_TRUE(contains(f.run(".alias show"), ".&答案 → .r 43"));
    ASSERT_TRUE(contains(f.run(".alias rm 答案"), "已删除群"));
    ASSERT_TRUE(shortcut::list(f.db, f.msg, false).empty());
    ASSERT_TRUE(contains(f.run(".alias del 答案"), "未找到"));
    ASSERT_TRUE(contains(f.run(".alias list"), "没有可用"));
    f.run(".help"); ASSERT_FALSE(f.router.lastShortcutHandled());
}

TEST(CommandShortcuts, PersonalCrossGroupPrivateAndGroupPrecedence) {
    ShortcutFixture f;
    f.run(".alias --my 答案 .r 1");
    f.run(".alias 答案 .r 2");
    ASSERT_TRUE(contains(f.run(".& 答案"), "群快捷指令 .r 2"));
    auto list = f.run(".alias show");
    ASSERT_TRUE(contains(list, "[群] .&答案 → .r 2"));
    ASSERT_TRUE(contains(list, "[个人] .&答案 → .r 1"));
    ASSERT_TRUE(list.find("[群]") < list.find("[个人]"));
    f.msg.targetId = "3000";
    ASSERT_TRUE(contains(f.run(".a 答案"), "个人快捷指令 .r 1"));
    f.privateChat();
    ASSERT_TRUE(contains(f.run(".alias 私聊 .r 3"), "已添加个人"));
    ASSERT_TRUE(contains(f.run(".&答案"), "个人快捷指令 .r 1"));
    f.msg.type = MessageType::kGroup; f.msg.targetId = "2000";
    ASSERT_TRUE(contains(f.run(".&私聊"), "个人快捷指令 .r 3"));
    f.run(".alias del --my 答案");
    ASSERT_TRUE(shortcut::list(f.db, f.msg, true).count("答案") == 0);
    ASSERT_TRUE(shortcut::list(f.db, f.msg, false).count("答案") == 1);
}

TEST(CommandShortcuts, ScopeDoesNotLeakBetweenPlayersGroupsOrAdapterAccounts) {
    ShortcutFixture f;
    f.run(".alias 群指令 .r 1"); f.run(".alias --my 私人 .r 2");
    f.msg.senderId = "1001";
    ASSERT_TRUE(contains(f.run(".&群指令"), "群快捷指令"));
    ASSERT_TRUE(contains(f.run(".&私人"), "未找到"));
    f.msg.adapterId = "another-bot";
    ASSERT_TRUE(contains(f.run(".&群指令"), "未找到"));
    f.msg.senderId = "1000";
    ASSERT_TRUE(contains(f.run(".&私人"), "个人快捷指令"));
    f.msg.adapterId = "shortcut-test"; f.msg.targetId = "9999";
    ASSERT_TRUE(contains(f.run(".&群指令"), "未找到"));
}

TEST(CommandShortcuts, CallerCannotInheritTheCreatorsMasterPrivileges) {
    ShortcutFixture f; f.master();
    f.run(".alias 管理 .admin add 123456");
    f.msg.senderId = "1001";
    ASSERT_TRUE(contains(f.run(".&管理"), f.i18n.tr(Locale::kZhHans, "gate.not_master")));
    ASSERT_EQ(f.db.getStorage()->count<PlayerProfileRow>(), 0);
    f.msg.senderId = "1000";
    ASSERT_TRUE(contains(f.run(".&管理"), "123456"));
    ASSERT_EQ(f.db.getStorage()->count<PlayerProfileRow>(), 1);
}

TEST(CommandShortcuts, BotOffHardLockBlacklistAndSilentGlobalStayEffective) {
    ShortcutFixture f;
    int calls = 0;
    f.router.setPluginCommandClaim([](const Message&, const std::string& body) { return body.rfind("testplugin", 0) == 0; });
    f.router.setShortcutPluginBridge([&](const Message&, const std::string&) -> std::optional<std::string> { ++calls; return "plugin"; });
    f.run(".alias 插件 .testplugin");
    f.setting("enabled", "0");
    ASSERT_TRUE(f.run(".&插件").empty()); ASSERT_EQ(calls, 0); ASSERT_TRUE(f.router.lastShortcutHandled());
    f.msg.atList = {f.msg.selfId}; ASSERT_TRUE(contains(f.run(".&插件"), "plugin")); ASSERT_EQ(calls, 1);
    f.setting("locked", "1"); ASSERT_TRUE(f.run(".&插件").empty()); ASSERT_EQ(calls, 1);
    f.setting("locked", "0"); f.setting("enabled", "1"); f.msg.atList.clear();
    f.cfg.set<bool>("dice/silent_global", true);
    ASSERT_TRUE(f.run(".&插件").empty()); ASSERT_EQ(calls, 1);
    f.cfg.set<bool>("dice/silent_global", false);
    BanlistRow ban; ban.targetType = 0; ban.listType = 0; ban.targetId = f.msg.senderId; f.db.getStorage()->insert(ban);
    ASSERT_TRUE(f.run(".&插件").empty()); ASSERT_EQ(calls, 1);
}

TEST(CommandShortcuts, TargetRollAndCommandDisableGatesAreNotBypassed) {
    ShortcutFixture f;
    f.run(".alias 今日 .jrrp"); f.run(".alias 骰点 .r 1");
    f.setting("disabledCmds", "jrrp");
    ASSERT_TRUE(contains(f.run(".&今日"), f.i18n.tr(Locale::kZhHans, "gate.in_group", {{"cmd", "jrrp"}})));
    f.setting("rollEnabled", "0"); ASSERT_TRUE(f.run(".&骰点").empty());
    ASSERT_TRUE(f.router.lastShortcutHandled());
    f.setting("rollEnabled", "1"); f.cfg.set<bool>("dice/disabled_jrrp", true);
    ASSERT_TRUE(contains(f.run(".&今日"), f.i18n.tr(Locale::kZhHans, "gate.jrrp_global")));
}

TEST(CommandShortcuts, PluginArgumentsCallerIdentityAndDeliberateSilenceArePreserved) {
    ShortcutFixture f;
    Message seen; std::string seenBody; int calls = 0;
    f.router.setPluginCommandClaim([](const Message&, const std::string& body) { return body.rfind("testplugin", 0) == 0; });
    f.router.setShortcutPluginBridge([&](const Message& msg, const std::string& body) -> std::optional<std::string> {
        seen = msg; seenBody = body; ++calls; return "";
    });
    f.run(".alias 新闻 .testplugin topic --flag");
    f.msg.senderId = "1001";
    ASSERT_TRUE(f.run(".&新闻 detail --other=1").empty());
    ASSERT_EQ(calls, 1); ASSERT_EQ(seen.senderId, "1001"); ASSERT_EQ(seen.adapterId, f.msg.adapterId);
    ASSERT_EQ(seen.targetId, "2000"); ASSERT_EQ(seen.content, ".testplugin topic --flag detail --other=1");
    ASSERT_EQ(seenBody, "testplugin topic --flag detail --other=1");
    ASSERT_TRUE(f.router.lastShortcutHandled()); ASSERT_TRUE(f.router.lastShortcutWasPlugin());
    f.setting("pluginEnabled", "0"); f.run(".&新闻"); ASSERT_EQ(calls, 1);
}

TEST(CommandShortcuts, RejectsRecursionVerificationTargetsInvalidPrefixesAndOversizeCommands) {
    ShortcutFixture f;
    for (const auto* target : {".&x", ".a x", ".bind confirm 12345678", ".bind qq 123456", "r 1"})
        ASSERT_TRUE(contains(f.run(std::string(".alias 坏 ") + target), "目标必须"));
    ASSERT_TRUE(shortcut::list(f.db, f.msg, false).empty());
    f.run(".alias 正常 .r 1");
    ASSERT_TRUE(contains(f.run(".&正常 " + std::string(4096, 'x')), "目标必须"));
    f.cfg.set<std::vector<std::string>>("dice/command_prefixes", {">"});
    ASSERT_TRUE(contains(f.run(">&正常"), "目标必须"));
    f.run(">alias 新 >r 2"); ASSERT_TRUE(contains(f.run(">&新"), ">r 2"));
}

TEST(CommandShortcuts, RealSealPluginUsesExpandedMessageAndTheInvokingPlayersPrivileges) {
    ShortcutFixture f;
    {
        std::ofstream file(f.temp.path / "probe.js", std::ios::binary);
        file << R"JS(
const ext = seal.ext.new('shortcut-probe', 'Dice!Next', '1.0.0');
ext.onCommandReceived = () => { globalThis.__shortcutHooks = (globalThis.__shortcutHooks || 0) + 1; };
const cmd = seal.ext.newCmdItemInfo();
cmd.name = 'shortcutprobe';
cmd.solve = (ctx, msg, args) => {
    globalThis.__shortcutCalls = (globalThis.__shortcutCalls || 0) + 1;
    seal.replyToSender(ctx, msg, [
        ctx.player.userId, msg.sender.userId, ctx.privilegeLevel, msg.message,
        args.rawArgs, ctx.group.groupId, ctx.endPoint.id, msg.segment[0].data.text
    ].join('|'));
    return seal.ext.newCmdExecuteResult(true);
};
ext.cmdMap.shortcutprobe = cmd;
seal.ext.register(ext);
)JS";
    }
    JsPluginManager plugins;
    ASSERT_TRUE(plugins.init());
    ASSERT_EQ(plugins.loadDir(u8str(f.temp.path)), 1);
    plugins.setGroupGate([&](const std::string&, const std::string&, const std::string&, const std::string&) {
        return f.router.groupFeatureEnabled(f.msg, "plugin");
    });
    f.router.setPluginCommandClaim([&](const Message& msg, const std::string& body) {
        return plugins.hasCommand(msg, shortcut::split(body).first);
    });
    f.router.setShortcutPluginBridge([&](const Message& msg, const std::string& body) -> std::optional<std::string> {
        const auto result = plugins.handle(msg, body, f.router.jsPrivilegeLevel(msg));
        return result.matched ? std::optional<std::string>(result.reply) : std::nullopt;
    });
    f.master(); f.run(".alias 插件 .shortcutprobe fixed");
    f.msg.senderId = "1001";
    f.msg.extra["raw"] = {{"message", json::array({{{"type", "text"}, {"data", {{"text", ".&插件"}}}}})}};
    const auto reply = f.run(".&插件 more --flag=1");
    ASSERT_TRUE(contains(reply, "1001|1001|0|.shortcutprobe fixed more --flag=1|fixed more --flag=1"));
    ASSERT_TRUE(contains(reply, "2000|shortcut-test"));
    ASSERT_TRUE(contains(reply, "shortcut-test|.shortcutprobe fixed more --flag=1"));
    ASSERT_TRUE(f.router.lastShortcutWasPlugin());
    ASSERT_EQ(*plugins.evalString("String(globalThis.__shortcutCalls)"), "1");
    ASSERT_EQ(*plugins.evalString("String(globalThis.__shortcutHooks)"), "1");
    f.setting("pluginEnabled", "0"); f.run(".&插件");
    ASSERT_EQ(*plugins.evalString("String(globalThis.__shortcutCalls)"), "1");
    f.setting("pluginEnabled", "1"); f.msg.senderId = "1000";
    ASSERT_TRUE(contains(f.run(".a 插件"), "1000|1000|100|"));
    ASSERT_EQ(*plugins.evalString("String(globalThis.__shortcutHooks)"), "2");
}

TEST(CommandShortcuts, TargetMarkdownAndPersonaOverridesRemainIntact) {
    ShortcutFixture f;
    f.run(".alias 答案 .r 42");
    f.cfg.set<std::string>("dice/nick_prefix", ""); f.cfg.set<std::string>("dice/nick_suffix", "");
    f.i18n.setOverride(Locale::kZhHans, "dice.roll.result", "**{nick}** = `{res}`", ContentFormat::kMarkdown);
    f.i18n.setPersonaBundles(42, Locale::kZhHans, {{"shortcut.trigger_prefix", "PERSONA:{shortcut}\n"}});
    f.i18n.setPersona(42);
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    const auto markdown = f.run(".&答案", Locale::kZhHans, std::nullopt);
    ASSERT_TRUE(contains(markdown, "PERSONA:答案"));
    ASSERT_TRUE(contains(markdown, "**玩家**"));
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kPlainText);
    const auto plain = f.run(".&答案", Locale::kZhHans, std::nullopt);
    ASSERT_TRUE(contains(plain, "PERSONA:答案"));
    ASSERT_FALSE(contains(plain, "**"));
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kPlainText);
}

TEST(CommandShortcuts, RuntimeRecursionGuardAlsoCoversPluginGeneratedInvocations) {
    ShortcutFixture f; int calls = 0;
    f.router.setPluginCommandClaim([](const Message&, const std::string& body) { return body == "testplugin"; });
    f.router.setShortcutPluginBridge([&](const Message& msg, const std::string&) -> std::optional<std::string> {
        ++calls; auto nested = msg; nested.content = ".&循环"; return f.router.handleMessage(nested, Locale::kZhHans);
    });
    f.run(".alias 循环 .testplugin");
    ASSERT_TRUE(contains(f.run(".&循环"), "不能调用其他")); ASSERT_EQ(calls, 1);
    ASSERT_EQ(CommandRouter::shortcutDepth_, 0);
}

TEST(CommandShortcuts, RulePackAliasesKeepTheirExpandedPluginArgumentsAndRestrictions) {
    ShortcutFixture f;
    struct RuleRestore {
        std::vector<CommandRouter::RulePack> saved = CommandRouter::rulePacks();
        ~RuleRestore() { CommandRouter::rulePacks() = std::move(saved); }
    } restore;
    CommandRouter::RulePack rule; rule.name = "shortcut-test-rule";
    rule.cmdAlias = {{"magic", "testplugin fixed"}, {"verify", "bind confirm"}, {"loop", "a another"}};
    CommandRouter::rulePacks().push_back(rule);
    f.setting("ruleSystem", rule.name);
    Message seen; std::string body;
    f.router.setPluginCommandClaim([](const Message&, const std::string& cmd) { return cmd.rfind("testplugin", 0) == 0; });
    f.router.setShortcutPluginBridge([&](const Message& message, const std::string& cmd) -> std::optional<std::string> {
        seen = message; body = cmd; return "plugin-reply";
    });
    f.run(".alias 魔法 .magic");
    ASSERT_TRUE(contains(f.run(".&魔法 appended"), "plugin-reply"));
    ASSERT_EQ(body, "testplugin fixed appended");
    ASSERT_EQ(seen.content, ".testplugin fixed appended");
    ASSERT_EQ(seen.extra["segments"][0]["data"]["text"].get<std::string>(), seen.content);
    f.run(".alias 验证 .verify");
    ASSERT_TRUE(contains(f.run(".&验证"), "目标必须"));
    f.run(".alias 循环 .loop");
    ASSERT_TRUE(contains(f.run(".&循环"), "不能调用其他"));
}

TEST(CommandShortcuts, MixedMarkdownPrefixesAndLiteralTargetsComposeWithoutReinterpretingText) {
    ShortcutFixture f;
    f.run(".alias 骰 .r 1");
    f.i18n.setOverride(Locale::kZhHans, "dice.roll.result_reason", "**{nick}** {res} {reason}", ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    const auto richTarget = f.run(".&骰 测试**literal**", Locale::kZhHans, std::nullopt);
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kMarkdown);
    ASSERT_TRUE(contains(richTarget, markdown::escapeLiteral(".r 1 测试**literal**")));
    f.router.setPluginCommandClaim([](const Message&, const std::string& cmd) { return cmd == "testplugin"; });
    f.router.setShortcutPluginBridge([](const Message&, const std::string&) -> std::optional<std::string> {
        return "plain **not-bold** [CQ:image,file=test.png]";
    });
    f.run(".alias 插件 .testplugin");
    f.i18n.setOverride(Locale::kZhHans, "shortcut.trigger_prefix", "**{source}**\n", ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    const auto richPrefix = f.run(".&插件", Locale::kZhHans, std::nullopt);
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kMarkdown);
    ASSERT_TRUE(contains(richPrefix, "**群**"));
    ASSERT_TRUE(contains(richPrefix, markdown::escapeLiteral("plain **not-bold** [CQ:image,file=test.png]")));
    I18n::beginOutboundCapture(ContentFormat::kPlainText);
    ASSERT_EQ(f.run(".&插件", Locale::kZhHans, std::nullopt), "群\nplain **not-bold** [CQ:image,file=test.png]");
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kPlainText);
}

TEST(CommandShortcuts, CensoredStoredTargetsDoNotEchoTheBlockedCommand) {
    ShortcutFixture f;
    f.run(".alias 查 .r 1 测试拦截词");
    f.cfg.set<json>("dice/censor", {{"enabled", true}, {"words", {{"测试拦截词", "Danger"}}}});
    f.router.reloadSensitiveWordRules();
    const auto result = f.run(".&查");
    ASSERT_FALSE(result.empty()); ASSERT_FALSE(contains(result, "测试拦截词"));
    ASSERT_FALSE(contains(result, "快捷指令 .r"));
}

TEST(CommandShortcuts, LegacyAccountPermissionsRemainAvailableOnlyUnderAdmin) {
    ShortcutFixture f; f.master();
    f.run(".admin account-alias add 1001 1000");
    ASSERT_EQ(f.cfg.get<json>("dice/aliases", json::array()).size(), size_t(1));
    f.run(".alias 查 .r 1");
    ASSERT_EQ(f.cfg.get<json>("dice/aliases", json::array()).size(), size_t(1));
    ASSERT_TRUE(contains(f.run(".alias add 1002 1000"), ".admin account-alias"));
    f.msg.senderId = "1001";
    ASSERT_TRUE(contains(f.run(".admin account-alias list"), "1001 -> 1000"));
    f.msg.senderId = "1002";
    ASSERT_EQ(f.run(".admin account-alias list"), f.i18n.tr(Locale::kZhHans, "gate.not_master"));
}

TEST(CommandShortcuts, PersistenceAndBoundIdentityMigrationUseExistingSettingsTables) {
    ShortcutFixture f; f.privateChat();
    auto& identities = identity::BindingStore::instance();
    f.msg.platform = "discord";
    f.msg.senderId = identities.observeVirtual(f.db, "discord", "discord-bot", "native-user", identity::Kind::User);
    f.run(".alias 数据 .r 1");
    std::string error;
    ASSERT_TRUE(identities.bindPlatformToQQ(f.db, "discord", "native-user", "123456", identity::Kind::User, error));
    f.msg.platform = "onebot_v11"; f.msg.senderId = "123456";
    ASSERT_EQ(shortcut::list(f.db, f.msg, true).at("数据"), ".r 1");
    f.db.close(); ASSERT_TRUE(f.db.open(u8str(f.temp.path / "test.db")));
    ASSERT_EQ(shortcut::list(f.db, f.msg, true).at("数据"), ".r 1");
    ASSERT_TRUE(contains(f.run(".&数据"), "个人快捷指令"));
}

TEST(CommandShortcuts, LimitsAndAllFourReplyLocalesHaveCompleteCoverage) {
    ShortcutFixture f;
    for (size_t i = 0; i < shortcut::kMaxEntries; ++i)
        ASSERT_TRUE(shortcut::write(f.db, f.msg, true, "name" + std::to_string(i), ".r 1") == shortcut::WriteResult::Added);
    ASSERT_TRUE(shortcut::write(f.db, f.msg, true, "extra", ".r 1") == shortcut::WriteResult::Limit);
    ASSERT_TRUE(shortcut::write(f.db, f.msg, true, "name0", ".r 2") == shortcut::WriteResult::Replaced);
    const auto base = f.i18n.flatten(Locale::kZhHans);
    for (Locale loc : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa}) {
        const auto flat = f.i18n.flatten(loc);
        for (const auto& [key, value] : base) if (key.rfind("shortcut.", 0) == 0) {
            ASSERT_TRUE(flat.count(key) == 1); ASSERT_FALSE(flat.at(key).empty());
        }
    }
}

TEST(CommandShortcuts, OrdinaryPluginGeneratedMessagesDoNotInheritTheShortcutConsumedFlag) {
    ShortcutFixture f;
    f.router.setPluginCommandClaim([](const Message&, const std::string& body) { return body == "testplugin"; });
    bool callbackCanFallBack = false;
    f.router.setShortcutPluginBridge([&](const Message& msg, const std::string&) -> std::optional<std::string> {
        auto generated = msg; generated.content = ".other-plugin-command";
        const auto reply = f.router.handleMessage(generated, Locale::kZhHans);
        callbackCanFallBack = reply.empty() && !f.router.lastShortcutHandled();
        return "callback-ok";
    });
    f.run(".alias 插件 .testplugin");
    ASSERT_TRUE(contains(f.run(".&插件"), "callback-ok"));
    ASSERT_TRUE(callbackCanFallBack);
    ASSERT_TRUE(f.router.lastShortcutHandled());
    ASSERT_TRUE(f.router.lastShortcutWasPlugin());
    ASSERT_FALSE(CommandRouter::shortcutNextPersona_.has_value());
}

TEST(CommandShortcuts, FragmentCapturePreservesPreferencesNestingAndExceptionState) {
    I18n::beginOutboundCapture(ContentFormat::kMarkdown, PresentationStyle::kVisual);
    const auto rich = I18n::captureFragment([] { return I18n::previewTemplate("**bold**", {}, ContentFormat::kMarkdown); });
    const auto plain = I18n::captureFragment([] { return "literal **stars**"; });
    ASSERT_TRUE(rich.format == ContentFormat::kMarkdown);
    ASSERT_TRUE(plain.format == ContentFormat::kPlainText);
    ASSERT_TRUE(I18n::outboundPresentationStyle() == PresentationStyle::kVisual);
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kPlainText, PresentationStyle::kTraditional);
    const auto nested = I18n::captureFragment([] { return I18n::captureFragment([] {
        return I18n::previewTemplate("**plain**", {}, ContentFormat::kMarkdown);
    }).text; });
    ASSERT_EQ(nested.text, "plain");
    ASSERT_TRUE(nested.format == ContentFormat::kPlainText);
    bool thrown = false;
    try { I18n::captureFragment([]() -> std::string { throw std::runtime_error("test"); }); }
    catch (...) { thrown = true; }
    ASSERT_TRUE(thrown);
    ASSERT_TRUE(I18n::outboundPresentationStyle() == PresentationStyle::kTraditional);
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kPlainText);
}
