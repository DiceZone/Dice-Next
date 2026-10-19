#include "test_framework.h"
#include "test_watchdog.h"
#include <future>
#include "../src/core/mod/js_plugin_manager.h"
#include "../src/adapter/adapter_interface.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dice;
namespace fs = std::filesystem;

TEST(JsPluginManager, DirectoryMetadataIsScopedDynamicAndNeverExecutesAccessors) {
    const auto root = fs::temp_directory_path() / ("dice_js_directory_" + newLogCorrelationId());
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path,ec); } } cleanup{root};
    fs::create_directories(root);
    { std::ofstream file(root / "catalog.js"); file << R"JS(
globalThis.directoryReads=0;
const ext = seal.ext.new('catalog','test','1');
const command = seal.ext.newCmdItemInfo(); command.solve=()=>{directoryReads++;return seal.ext.newCmdExecuteResult(true)};
ext.cmdMap.public=command;
Object.defineProperty(ext.cmdMap,'hidden',{enumerable:true,get(){directoryReads++;throw new Error('directory must not read getters')}});
ext.cmdMap.proxy=new Proxy({}, {getOwnPropertyDescriptor(){directoryReads++;throw new Error('proxy trap')}});
seal.ext.register(ext);
)JS"; }
    JsPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir(root.string()),1);
    manager.setGroupGate([](const auto& platform,const auto& group,const auto& plugin,const auto& account) {
        return platform=="onebot_v11" && group=="allowed" && plugin=="js:catalog.js" && account=="A";
    });
    Message message; message.platform="onebot_v11"; message.adapterId="A"; message.targetId="allowed"; message.type=MessageType::kGroup;
    ASSERT_TRUE(manager.directoryTriggers(message)==std::vector<std::string>({"public"}));
    ASSERT_TRUE(manager.evalString("directoryReads")==std::optional<std::string>("0"));
    message.adapterId="B"; ASSERT_TRUE(manager.directoryTriggers(message).empty());
    message.adapterId="A"; message.targetId="blocked"; ASSERT_TRUE(manager.directoryTriggers(message).empty());
    message.targetId="allowed";
    ASSERT_TRUE(manager.evalString("seal.ext.find('catalog').cmdMap.later=seal.ext.find('catalog').cmdMap.public;'ok'"));
    ASSERT_TRUE(manager.directoryTriggers(message)==std::vector<std::string>({"later","public"}));
    ASSERT_TRUE(manager.evalString("delete seal.ext.find('catalog').cmdMap.public;'ok'"));
    ASSERT_TRUE(manager.directoryTriggers(message)==std::vector<std::string>({"later"}));
    ASSERT_TRUE(manager.evalString("directoryReads")==std::optional<std::string>("0"));
    fs::remove(root / "catalog.js"); ASSERT_EQ(manager.reload(root.string()),0);
    ASSERT_TRUE(manager.directoryTriggers(message).empty());
}

TEST(JsPluginManager, DirectoryReadsDoNotWaitForTheRuntimeLock) {
    TestWatchdog watchdog(std::chrono::seconds(20));
    JsPluginManager manager; ASSERT_TRUE(manager.init());
    ASSERT_TRUE(manager.evalString("var e=seal.ext.new('test','test','1');var c=seal.ext.newCmdItemInfo();c.solve=()=>{};e.cmdMap.test=c;seal.ext.register(e);'ok'"));
    std::promise<void> entered, release;
    auto released=release.get_future();
    manager.setDiceEval([&](const std::string&) { entered.set_value(); released.wait(); return "1"; });
    auto executing=std::async(std::launch::async,[&] { return manager.evalString("seal.format({},'{1d1}')"); });
    entered.get_future().wait();
    Message message; message.type=MessageType::kPrivate;
    const auto commands=manager.directoryTriggers(message);
    release.set_value();
    ASSERT_TRUE(executing.get().has_value());
    ASSERT_TRUE(commands==std::vector<std::string>({"test"}));
}

TEST(JsPluginManager, ConsoleLevelsAndCommandTimerOriginsArePreserved) {
    const auto nonce = newLogCorrelationId();
    const auto root = fs::temp_directory_path() / ("dice_js_logs_" + nonce);
    fs::create_directories(root);
    std::ofstream file(root / "origin.js", std::ios::binary);
    file << R"JS(
// ==UserScript==
// @name 日报来源测试
// ==/UserScript==
const ext = seal.ext.new('origin', 'test', '1.0');
const cmd = seal.ext.newCmdItemInfo();
cmd.solve = (ctx, msg) => {
  console.info('own-info-marker'); console.warn('own-warn-marker');
  console.error('own-error-marker'); console.debug('own-debug-marker'); console.trace('own-trace-marker');
  seal.replyToSender(ctx, msg, 'immediate-reply');
  setTimeout(() => { console.info('timer-log-marker'); seal.replyToSender(ctx, msg, 'timer-reply'); }, 1);
  return seal.ext.newCmdExecuteResult(true);
};
ext.cmdMap.origin = cmd; seal.ext.register(ext);
)JS";
    file.close();
    auto previous = std::string(logLevelName(logRecordingLevel()));
    configureLogger(nlohmann::json{{"server", {{"log_level", "trace"}}}});
    std::vector<std::function<void()>> callbacks;
    std::string timerSource;
    {
        JsPluginManager manager;
        manager.setScheduler([&](double, std::function<void()> cb) { callbacks.push_back(std::move(cb)); });
        manager.setSender([&](const auto&, bool, const auto&, const auto&, const auto& text) {
            if (text == "timer-reply") timerSource = activeProcessingContext.sources[0].file;
        });
        ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir(root.string()), 1);
        Message message; message.platform = "onebot_v11"; message.senderId = "user"; message.targetId = "group"; message.content = ".origin";
        auto result = manager.handle(message, "origin");
        ASSERT_EQ(result.reply, std::string("immediate-reply"));
        ASSERT_EQ(result.sources.size(), size_t(1)); ASSERT_EQ(result.sources[0].file, std::string("origin.js"));
        ASSERT_EQ(callbacks.size(), size_t(1));
        auto callback = std::move(callbacks[0]); callbacks.clear(); callback();
    }
    bool info = false, warning = false, error = false, debug = false, trace = false, timer = false;
    for (const auto& record : recentLogRecords(1000)) {
        const auto content = record["message"].get<std::string>();
        if (content.find("own-info-marker") != std::string::npos) info = record["level"] == "info";
        if (content.find("own-warn-marker") != std::string::npos) warning = record["level"] == "warn";
        if (content.find("own-error-marker") != std::string::npos) error = record["level"] == "error";
        if (content.find("own-debug-marker") != std::string::npos) debug = record["level"] == "debug";
        if (content.find("own-trace-marker") != std::string::npos) trace = record["level"] == "trace";
        if (content.find("timer-log-marker") != std::string::npos) timer = record["details"]["processing"]["sources"][0]["file"] == "origin.js";
    }
    configureLogger(nlohmann::json{{"server", {{"log_level", previous}}}});
    std::error_code ec; fs::remove_all(root, ec);
    ASSERT_TRUE(info && warning && error && debug && trace && timer);
    ASSERT_EQ(timerSource, std::string("origin.js"));
}

TEST(JsPluginManager, RejectedPromisesNeverReportCompletedProcessing) {
    const auto root = fs::temp_directory_path() / ("dice_js_rejected_log_" + newLogCorrelationId());
    fs::create_directories(root);
    { std::ofstream file(root / "reject-logs.js", std::ios::binary); file << R"JS(
const ext = seal.ext.new('reject-logs', 'test', '1.0');
ext.onLoad = async () => { await Promise.resolve(); throw new Error('init-rejection-marker'); };
const cmd = seal.ext.newCmdItemInfo();
cmd.solve = async (ctx, msg) => {
  seal.replyToSender(ctx, msg, 'reply-before-rejection');
  setTimeout(async () => { await Promise.resolve(); throw new Error('timer-rejection-marker'); }, 1);
  await Promise.resolve(); throw new Error('command-rejection-marker');
};
ext.cmdMap.rejectlogs = cmd; seal.ext.register(ext);
)JS"; }
    std::vector<std::function<void()>> callbacks;
    {
        JsPluginManager manager;
        manager.setScheduler([&](double, std::function<void()> cb) { callbacks.push_back(std::move(cb)); });
        ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir(root.string()), 1);
        Message message; message.content = ".rejectlogs"; message.platform = "onebot_v11";
        auto result = manager.handle(message, "rejectlogs");
        ASSERT_TRUE(result.failed); ASSERT_FALSE(result.pending);
        ASSERT_EQ(result.reply, std::string("reply-before-rejection"));
        ASSERT_EQ(callbacks.size(), size_t(1));
        auto callback = std::move(callbacks[0]); callbacks.clear(); callback();
    }
    bool init = false, command = false, timer = false, falseCompletion = false;
    for (const auto& record : recentLogRecords(1000)) {
        const auto text = record["message"].get<std::string>();
        if (text.find("init-rejection-marker") != std::string::npos) init = record["level"] == "error";
        if (text.find("command-rejection-marker") != std::string::npos) command = record["level"] == "error";
        if (text.find("timer-rejection-marker") != std::string::npos) timer = record["level"] == "error";
        if (text.find("reject-logs") != std::string::npos && text.find("定时回调执行完成") != std::string::npos) falseCompletion = true;
    }
    std::error_code ec; fs::remove_all(root, ec);
    ASSERT_TRUE(init && command && timer); ASSERT_FALSE(falseCompletion);
}

TEST(JsPluginManager, AwaitContinuationUsesItsOwnPluginNotTheResolvingTimersOwner) {
    const auto root = fs::temp_directory_path() / ("dice_js_async_origin_" + newLogCorrelationId());
    fs::create_directories(root);
    { std::ofstream file(root / "owner-a.js", std::ios::binary); file << R"JS(
globalThis.originPromise = new Promise(resolve => setTimeout(() => resolve('ready'), 1));
)JS"; }
    { std::ofstream file(root / "owner-b.js", std::ios::binary); file << R"JS(
const ext = seal.ext.new('owner-b', 'test', '1.0');
const cmd = seal.ext.newCmdItemInfo();
cmd.solve = async (ctx, msg) => {
  await globalThis.originPromise;
  console.info('cross-owner-async-marker');
  seal.replyToSender(ctx, msg, 'owned-by-b');
  return seal.ext.newCmdExecuteResult(true);
};
ext.cmdMap.originb = cmd; seal.ext.register(ext);
)JS"; }
    std::vector<std::function<void()>> callbacks;
    std::string sendOwner;
    {
        JsPluginManager manager;
        manager.setScheduler([&](double, std::function<void()> cb) { callbacks.push_back(std::move(cb)); });
        manager.setSender([&](const auto&, bool, const auto&, const auto&, const auto& text) {
            if (text == "owned-by-b") sendOwner = activeProcessingContext.sources[0].file;
        });
        ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir(root.string()), 2);
        Message message; message.platform = "onebot_v11"; message.senderId = "u"; message.targetId = "g"; message.content = ".originb";
        ASSERT_TRUE(manager.handle(message, "originb").reply.empty());
        ASSERT_EQ(callbacks.size(), size_t(1)); callbacks[0]();
    }
    bool correct = false;
    for (const auto& record : recentLogRecords(1000))
        if (record["message"].get<std::string>().find("cross-owner-async-marker") != std::string::npos)
            correct = record["details"]["processing"]["sources"][0]["file"] == "owner-b.js";
    std::error_code ec; fs::remove_all(root, ec);
    ASSERT_TRUE(correct); ASSERT_EQ(sendOwner, std::string("owner-b.js"));
}

TEST(JsPluginManager, ExecutionTimeoutMemoryLimitAndRecovery) {
    TestWatchdog watchdog;
    JsPluginManager manager(std::chrono::milliseconds(100), 8 * 1024 * 1024);
    ASSERT_TRUE(manager.init());
    ASSERT_FALSE(manager.evalString("while (true) {}"));
    ASSERT_TRUE(manager.evalString("1 + 1") == std::optional<std::string>("2"));
    ASSERT_FALSE(manager.evalString("while(true) { try { while(true) {} } catch(e) {} }"));
    ASSERT_FALSE(manager.evalString("({toString() { while(true) {} }})"));
    ASSERT_FALSE(manager.evalString("'x'.repeat(32 * 1024 * 1024)"));
    ASSERT_TRUE(manager.evalString("'recovered'") == std::optional<std::string>("recovered"));
}

TEST(JsPluginManager, BusyRuntimeRejectsManagementQuickly) {
    TestWatchdog watchdog;
    JsPluginManager manager(std::chrono::milliseconds(600));
    ASSERT_TRUE(manager.init());
    std::promise<void> entered;
    manager.setHttpFetch([&](const std::string&, const std::string&, const std::string&, const std::string&, int& status) {
        entered.set_value();
        status = 200;
        return std::string("ok");
    });
    auto running = std::async(std::launch::async, [&] { return manager.evalString("fetch('https://example.invalid'); while(true) {}"); });
    entered.get_future().wait();
    const auto start = std::chrono::steady_clock::now();
    bool busy = false;
    try { manager.listAll(); } catch (const ScriptBusy&) { busy = true; }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto result = running.get();
    ASSERT_TRUE(busy);
    ASSERT_TRUE(elapsed < std::chrono::milliseconds(400));
    ASSERT_FALSE(result);
    ASSERT_TRUE(manager.evalString("'available'") == std::optional<std::string>("available"));
}

TEST(JsPluginManager, TimerFailureStopsRepeatingAndBridgeDoesNotSelfDeadlock) {
    TestWatchdog watchdog;
    JsPluginManager manager(std::chrono::milliseconds(100));
    std::vector<std::function<void()>> callbacks;
    manager.setScheduler([&](double, std::function<void()> callback) { callbacks.push_back(std::move(callback)); });
    ASSERT_TRUE(manager.init());
    ASSERT_TRUE(manager.evalString("setInterval(() => { while(true) {} }, 250)").has_value());
    ASSERT_EQ(callbacks.size(), size_t(1));
    auto callback = std::move(callbacks.front()); callbacks.clear(); callback();
    ASSERT_TRUE(callbacks.empty());
    // Clearing a timer within its callback must not free the JS_Call callee.
    ASSERT_TRUE(manager.evalString("globalThis.t = setInterval(() => clearInterval(t), 250)").has_value());
    callback = std::move(callbacks.front()); callbacks.clear(); callback();
    ASSERT_TRUE(callbacks.empty());
    manager.setScheduler({});
    ASSERT_TRUE(manager.evalString("WebSocket.call({}, 'wss://example.invalid').readyState") == std::optional<std::string>("0"));
}

TEST(JsPluginManager, ReloadAfterInitializationDoesNotSelfDeadlock) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_js_reload_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    {
        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 0);
        // WebUI upload/toggle/delete all finish by calling this reload path.
        std::ofstream plugin(root / "upload-regression.js", std::ios::binary);
        plugin << "const ext = seal.ext.new('upload-regression', 'Dice!Next', '1.0.0');\n"
                  "seal.ext.register(ext);\n";
        plugin.close();
        ASSERT_EQ(manager.reload(root.string()), 1);
        std::string error;
        ASSERT_TRUE(manager.deletePlugin("upload-regression.js", error));
        ASSERT_EQ(manager.reload(root.string()), 0);
    }

    fs::remove_all(root, ec);
}
TEST(JsPluginManager, SealDiceLifecycleMessageFieldsAndHookOrder) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_js_message_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    {
        std::ofstream plugin(root / "message-hooks.js", std::ios::binary);
        plugin << R"JS(
const ext = seal.ext.new('message-hooks', 'Dice!Next', '1.0.0');
let loaded = false;
ext.onLoad = () => { loaded = ext.isLoaded; };
globalThis.__sent = '';
ext.onMessageSend = (ctx, msg, flag) => globalThis.__sent =
  `sent:${ctx.player.userId}:${msg.sender.userId}:${msg.message}:${flag}`;
ext.onMessageReceived = (ctx, msg) => seal.replyToSender(ctx, msg,
  `recv:${msg.rawId}:${msg.guildId}:${msg.channelId}:${msg.segment.length}:${msg.message}:${loaded}`);
ext.onNotCommandReceived = (ctx, msg) => seal.replyToSender(ctx, msg, 'not-command');
ext.onCommandReceived = (ctx, msg, cmdArgs) =>
  seal.replyToSender(ctx, msg, `command-hook:${cmdArgs.command}:${cmdArgs.rawText}:` +
    `${cmdArgs.amIBeMentioned}:${cmdArgs.amIBeMentionedFirst}`);
const cmd = seal.ext.newCmdItemInfo();
cmd.name = 'probe';
cmd.solve = (ctx, msg, cmdArgs) => {
  seal.replyToSender(ctx, msg, `solve:${cmdArgs.rawArgs}`);
  return seal.ext.newCmdExecuteResult(true);
};
ext.cmdMap.probe = cmd;
seal.ext.register(ext);
const passive = seal.ext.newCmdItemInfo();
passive.name = 'passive';
ext.cmdMap.passive = passive;
const temp = seal.ext.newCmdItemInfo();
temp.name = 'temp';
temp.solve = (ctx, msg) => {
  seal.replyToSender(ctx, msg, seal.format(
    ctx, '{$t玩家_RAW}|{$t游戏模式}|{$t群号}|{$t群名}|{$t平台}|{$t消息类型}|' +
         '{$t骰子帐号}|{$t个人骰子面数}|{$t群组骰子面数}|{$t当前骰子面数}') +
         `|${ctx.endPoint.userId}`);
  return seal.ext.newCmdExecuteResult(true);
};
ext.cmdMap.temp = temp;
)JS";
        plugin.close();

        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 1);
        JsPluginManager::EndpointInfo endpoint;
        endpoint.id = "adapter-1";
        endpoint.nickname = "Dice";
        endpoint.userId = "bot-1";
        endpoint.platform = "QQ";
        endpoint.protocolType = "onebot";
        endpoint.state = 1;
        endpoint.groupNum = 3;
        manager.setEndpointProvider([endpoint]() {
            return std::vector<JsPluginManager::EndpointInfo>{endpoint};
        });
        const auto endpointText = manager.evalString("JSON.stringify(seal.getEndPoints())");
        ASSERT_TRUE(endpointText.has_value());
        const auto endpointJson = json::parse(*endpointText);
        ASSERT_EQ(endpointJson.size(), static_cast<size_t>(1));
        ASSERT_EQ(endpointJson[0]["id"].get<std::string>(), std::string("adapter-1"));
        ASSERT_EQ(endpointJson[0]["platform"].get<std::string>(), std::string("QQ"));
        ASSERT_EQ(endpointJson[0]["groupNum"].get<int>(), 3);
        manager.setGroupSystemResolver([](const std::string&, const std::string&,
                                          const std::string&) { return std::string("dnd5e"); });
        manager.setDiceSidesResolver([](const Message&) {
            return std::array<int, 3>{20, 30, 20};
        });

        Message msg;
        msg.id = "raw-42";
        msg.platform = "onebot_v11";
        msg.adapterId = "adapter-1";
        msg.selfId = "bot-1";
        msg.atList = {"bot-1", "other"};
        msg.senderId = "user-1";
        msg.senderName = "Tester";
        msg.targetId = "channel-9";
        msg.type = MessageType::kChannel;
        msg.content = "probe abc";
        msg.rawContent = "RAW-CONTENT";
        msg.timestamp = 123456;
        msg.extra = json{{"guild_id", "guild-7"}, {"channel_id", "channel-9"},
                         {"groupName", "Camel Group"},
                         {"segments", json::array({json{{"type", "text"}, {"data", "x"}}})}};

        manager.setGroupGate([](const std::string&, const std::string& group,
                                const std::string&) { return group != "blocked"; });
        ASSERT_TRUE(manager.hasCommand(msg, "probe"));
        Message blocked = msg;
        blocked.targetId = "blocked";
        ASSERT_FALSE(manager.hasCommand(blocked, "probe"));
        // Account-scoped switches must gate every callback before plugin code runs.
        manager.setGroupGate([](const std::string&, const std::string& group,
                                const std::string&, const std::string& adapterId) {
            return group != "blocked" && adapterId != "disabled-account";
        });
        blocked = msg;
        blocked.adapterId = "disabled-account";
        ASSERT_FALSE(manager.hasCommand(blocked, "probe"));
        ASSERT_TRUE(manager.handle(blocked, "probe abc").reply.empty());
        ASSERT_TRUE(manager.handleMessageReceived(blocked).reply.empty());
        ASSERT_TRUE(manager.handleNonCommand(blocked).reply.empty());
        ASSERT_TRUE(manager.handleCommandReceived(blocked, "roll reason").reply.empty());
        ASSERT_TRUE(manager.hasCommand(msg, "probe"));

        const auto received = manager.handleMessageReceived(msg);
        ASSERT_EQ(received.reply,
                  std::string("recv:raw-42:guild-7:channel-9:1:RAW-CONTENT:true"));

        const auto nonCommand = manager.handleNonCommand(msg);
        ASSERT_EQ(nonCommand.reply, std::string("not-command"));

        const auto command = manager.handle(msg, "probe abc");
        ASSERT_EQ(command.reply,
                  std::string("solve:abc\ncommand-hook:probe:RAW-CONTENT:true:true"));

        const auto tempReply = manager.handle(msg, "temp");
        ASSERT_EQ(tempReply.reply,
                  std::string("Tester|dnd5e|channel-9|Camel Group|QQ|group|bot-1|20|30|20|bot-1"
                              "\ncommand-hook:temp:RAW-CONTENT:true:true"));

        const auto builtin = manager.handleCommandReceived(msg, "roll reason");
        ASSERT_EQ(builtin.reply,
                  std::string("command-hook:roll:RAW-CONTENT:true:true"));

        const auto unknown = manager.handle(msg, "unknown value");
        ASSERT_EQ(unknown.reply,
                  std::string("command-hook:unknown:RAW-CONTENT:true:true"));

        const auto passive = manager.handle(msg, "passive value");
        ASSERT_EQ(passive.reply,
                  std::string("command-hook:passive:RAW-CONTENT:true:true"));

        Message sent = msg;
        sent.senderId = "bot-1"; sent.senderName = "Dice"; sent.fromSelf = true;
        sent.content = "DONE"; sent.rawContent = "DONE"; sent.displayContent = "DONE";
        manager.handleMessageSend(msg, sent, "delivered");
        const auto sentState = manager.evalString("globalThis.__sent");
        ASSERT_TRUE(sentState && *sentState == "sent:user-1:bot-1:DONE:delivered");
    }

    fs::remove_all(root, ec);
}

TEST(JsPluginManager, SealDiceEventHooksUseCoreEventShapes) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_js_events_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    {
        std::ofstream plugin(root / "event-hooks.js", std::ios::binary);
        plugin << R"JS(
const ext = seal.ext.new('event-hooks', 'Dice!Next', '1.0.0');
globalThis.__lastEvent = '';
ext.onPoke = (ctx, event) => globalThis.__lastEvent =
  `poke:${event.groupId}:${event.senderId}:${event.targetId}:${event.isPrivate}`;
ext.onGroupLeave = (ctx, event) => globalThis.__lastEvent =
  `leave:${event.groupId}:${event.userId}:${event.operatorId}`;
ext.onGroupMemberJoined = (ctx, msg) => globalThis.__lastEvent =
  `join:${msg.sender.userId}:${msg.groupId}`;
ext.onBecomeFriend = (ctx, msg) => globalThis.__lastEvent =
  `friend:${ctx.player.name}:${msg.sender.nickname}`;
ext.onGroupJoined = (ctx, msg) => globalThis.__lastEvent =
  `inviter:${ctx.player.name}:${msg.sender.nickname}`;
ext.onQQInteraction = (ctx, event, msg) => globalThis.__lastEvent =
  `interaction:${event.interaction_type}:${event.userId}:${ctx.player.name}:${msg.sender.nickname}`;
ext.onQQSubscriptionStatus = (ctx, event) => globalThis.__lastEvent =
  `subscription:${event.subscription_results[0].op}:${event.userId}`;
ext.onQQOfficialEvent = (ctx, event) => globalThis.__lastEvent =
  `official:${event.event_type}:${event.userId}`;
seal.ext.register(ext);
)JS";
        plugin.close();

        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 1);

        BotEvent poke;
        poke.type = EventType::kPoke; poke.platform = "onebot"; poke.groupId = "g";
        poke.userId = "bot"; poke.operatorId = "u";
        manager.handleEvent(poke);
        const auto pokeState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(pokeState && *pokeState == "poke:g:u:bot:false");

        BotEvent leave;
        leave.type = EventType::kGroupDecrease; leave.platform = "onebot"; leave.groupId = "g";
        leave.userId = "kicked"; leave.operatorId = "admin";
        manager.handleEvent(leave);
        const auto leaveState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(leaveState && *leaveState == "leave:g:kicked:admin");

        BotEvent joined;
        joined.type = EventType::kGroupIncrease; joined.platform = "onebot"; joined.groupId = "g";
        joined.selfId = "bot"; joined.userId = "newcomer"; joined.operatorId = "inviter";
        manager.handleEvent(joined);
        const auto joinState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(joinState && *joinState == "join:newcomer:g");

        BotEvent friendAdded;
        friendAdded.type = EventType::kFriendAdd; friendAdded.platform = "qq_official";
        friendAdded.userId = "public-user"; friendAdded.extra = {{"user_name", "已缓存昵称"}};
        manager.handleEvent(friendAdded);
        const auto friendState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(friendState && *friendState == "friend:已缓存昵称:已缓存昵称");

        BotEvent botJoined;
        botJoined.type = EventType::kGroupIncrease; botJoined.platform = "qq_official";
        botJoined.userId = botJoined.selfId = "bot"; botJoined.groupId = "public-group";
        botJoined.operatorId = "public-inviter"; botJoined.extra = {{"operator_name", "邀请人昵称"}};
        manager.handleEvent(botJoined);
        const auto inviterState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(inviterState && *inviterState == "inviter:邀请人昵称:邀请人昵称");

        BotEvent interaction;
        interaction.type = EventType::kOther; interaction.platform = "qq_official"; interaction.userId = "public-user";
        interaction.subType = "interaction"; interaction.extra = {{"interaction_type", 11}, {"user_name", "互动者"}};
        manager.handleEvent(interaction);
        const auto interactionState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(interactionState && *interactionState == "interaction:11:public-user:互动者:互动者");
        interaction.subType = "subscription_status";
        interaction.extra["subscription_results"] = json::array({{{"op", 2}}});
        manager.handleEvent(interaction);
        const auto subscriptionState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(subscriptionState && *subscriptionState == "subscription:2:public-user");
        interaction.subType.clear(); interaction.extra["event_type"] = "FRIEND_DEL";
        manager.handleEvent(interaction);
        const auto otherState = manager.evalString("globalThis.__lastEvent");
        ASSERT_TRUE(otherState && *otherState == "official:FRIEND_DEL:public-user");
    }

    fs::remove_all(root, ec);
}
