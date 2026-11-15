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

TEST(JsPluginManager, SyntaxFailureLogsActualPluginAndInterpreterErrorOnlyOnce) {
    const auto nonce = newLogCorrelationId();
    const auto root = fs::temp_directory_path() / ("dice_js_syntax_notice_" + nonce);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    fs::create_directories(root);
    const auto filename = "watermelon-" + nonce + ".js";
    {
        std::ofstream file(root / filename, std::ios::binary);
        file << "// ==UserScript==\n// @name 合成大西瓜\n// ==/UserScript==\n"
             << std::string(118, '\n') << "const broken = [1，2];\n";
    }
    JsPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir(root.string()), 0);
    int errors = 0;
    for (const auto &record : recentLogRecords(1000)) {
        if (record["level"] != "error" || record.dump().find(filename) == std::string::npos)
            continue;
        ++errors;
        const auto text = record["message"].get<std::string>();
        ASSERT_TRUE(text.find("插件「合成大西瓜」加载失败") != std::string::npos);
        ASSERT_TRUE(text.find("SyntaxError") != std::string::npos);
        ASSERT_TRUE(record["details"]["error"].get<std::string>().find(filename) != std::string::npos);
        ASSERT_TRUE(record["details"]["error"].get<std::string>().find(":123") != std::string::npos);
        ASSERT_EQ(record["details"]["operation"].get<std::string>(), "加载");
    }
    ASSERT_EQ(errors, 1);
}

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

TEST(JsPluginManager, GroupRequestHookCanApproveOrReject) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_js_group_request_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    {
        std::ofstream plugin(root / "join-audit.js", std::ios::binary);
        plugin << R"JS(
const ext = seal.ext.new('join-audit', 'Dice!Next', '1.0.0');
globalThis.__lastRequest = '';
ext.onGroupRequest = (ctx, event) => {
  globalThis.__lastRequest =
    `${event.groupId}:${event.userId}:${event.flag}:${event.subType}:${event.comment}:${ctx.endPoint.id}`;
  if (event.userId === 'ok') dicenext.setGroupAddRequest(ctx, event, true, '');
  else if (event.userId === 'no') dicenext.setGroupAddRequest(ctx, event.flag, event.subType, false, '仅限公寓骰主');
};
seal.ext.register(ext);
)JS";
        plugin.close();

        struct Call {
            std::string adapterId, platform, flag, subType, reason;
            bool approve = false;
        };
        std::vector<Call> calls;
        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 1);
        const auto sealApi = manager.evalString("typeof seal.setGroupAddRequest");
        const auto nextApi = manager.evalString("typeof dicenext.setGroupAddRequest");
        ASSERT_TRUE(sealApi && *sealApi == "undefined");
        ASSERT_TRUE(nextApi && *nextApi == "function");
        manager.setGroupRequest([&](const std::string& adapterId, const std::string& platform,
                                    const std::string& flag, const std::string& subType,
                                    bool approve, const std::string& reason) {
            calls.push_back(Call{adapterId, platform, flag, subType, reason, approve});
        });

        BotEvent ignore;
        ignore.type = EventType::kGroupRequest;
        ignore.platform = "onebot_v11"; ignore.adapterId = "adapter-1";
        ignore.groupId = "1071356242"; ignore.userId = "wait";
        ignore.flag = "f-wait"; ignore.subType = "add"; ignore.comment = "申请";
        manager.handleEvent(ignore);
        ASSERT_FALSE(manager.requestDecisionTaken());
        ASSERT_TRUE(calls.empty());
        const auto ignored = manager.evalString("globalThis.__lastRequest");
        ASSERT_TRUE(ignored && *ignored == "1071356242:wait:f-wait:add:申请:adapter-1");

        BotEvent approve;
        approve.type = EventType::kGroupRequest;
        approve.platform = "onebot_v11"; approve.adapterId = "adapter-1";
        approve.groupId = "1071356242"; approve.userId = "ok";
        approve.flag = "f-ok"; approve.subType = "add";
        manager.handleEvent(approve);
        ASSERT_TRUE(manager.requestDecisionTaken());
        ASSERT_TRUE(manager.lastGroupRequestApproved());
        ASSERT_EQ(manager.lastGroupRequestReason(), std::string());
        ASSERT_EQ(calls.size(), 1u);
        ASSERT_EQ(calls[0].flag, std::string("f-ok"));
        ASSERT_EQ(calls[0].subType, std::string("add"));
        ASSERT_TRUE(calls[0].approve);
        ASSERT_EQ(calls[0].adapterId, std::string("adapter-1"));

        BotEvent reject;
        reject.type = EventType::kGroupRequest;
        reject.platform = "onebot_v11"; reject.adapterId = "adapter-1";
        reject.groupId = "1071356242"; reject.userId = "no";
        reject.flag = "f-no"; reject.subType = "add";
        manager.handleEvent(reject);
        ASSERT_TRUE(manager.requestDecisionTaken());
        ASSERT_FALSE(manager.lastGroupRequestApproved());
        ASSERT_EQ(manager.lastGroupRequestReason(), std::string("仅限公寓骰主"));
        ASSERT_EQ(calls.size(), 2u);
        ASSERT_EQ(calls[1].flag, std::string("f-no"));
        ASSERT_FALSE(calls[1].approve);
        ASSERT_EQ(calls[1].reason, std::string("仅限公寓骰主"));
    }

    fs::remove_all(root, ec);
}

TEST(JsPluginManager, DiceNextNoticeHooksAndFriendRequestVetoOnly) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_js_notices_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    {
        std::ofstream plugin(root / "notice-hooks.js", std::ios::binary);
        plugin << R"JS(
const ext = seal.ext.new('notice-hooks', 'Dice!Next', '1.0.0');
globalThis.__log = [];
const push = (name, event) => {
  globalThis.__log.push(`${name}:${event.groupId||''}:${event.userId||''}:${event.subType||''}:${event.messageId||''}:${event.title||''}:${event.honorType||''}:${event.file&&event.file.name||''}:${event.duration||0}`);
};
ext.onGroupLeave = (ctx, event) => globalThis.__log.push(`leave:${event.groupId}:${event.userId}:${event.subType||''}`);
ext.onGroupKicked = (ctx, event) => push('kicked', event);
ext.onGroupUpload = (ctx, event) => push('upload', event);
ext.onGroupBan = (ctx, event) => push('ban', event);
ext.onGroupAdmin = (ctx, event) => push('admin', event);
ext.onFriendRecall = (ctx, event) => push('friendRecall', event);
ext.onMessageDeleted = (ctx, msg) => globalThis.__log.push(`deleted:${msg.rawId}:${msg.messageType}`);
ext.onGroupTitle = (ctx, event) => push('title', event);
ext.onGroupHonor = (ctx, event) => push('honor', event);
ext.onGroupEssence = (ctx, event) => push('essence', event);
ext.onFriendRequest = (ctx, event) => {
  globalThis.__log.push(`friendReq:${event.userId}:${event.flag}:${event.comment}`);
  if (event.userId === 'no') dicenext.setFriendAddRequest(ctx, event, false, '拒绝');
  if (event.userId === 'ok') dicenext.setFriendAddRequest(ctx, event, true, '想同意');
};
seal.ext.register(ext);
)JS";
        plugin.close();

        std::vector<std::string> friendCalls;
        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 1);
        const auto sealFriend = manager.evalString("typeof seal.setFriendAddRequest");
        const auto nextFriend = manager.evalString("typeof dicenext.setFriendAddRequest");
        ASSERT_TRUE(sealFriend && *sealFriend == "undefined");
        ASSERT_TRUE(nextFriend && *nextFriend == "function");
        manager.setFriendRequest([&](const std::string&, const std::string&,
                                     const std::string& flag, const std::string& reason) {
            friendCalls.push_back(flag + ":" + reason);
        });

        BotEvent leave;
        leave.type = EventType::kGroupDecrease; leave.platform = "onebot_v11";
        leave.groupId = "g"; leave.userId = "u"; leave.operatorId = "op"; leave.subType = "leave";
        manager.handleEvent(leave);

        BotEvent kick;
        kick.type = EventType::kGroupDecrease; kick.platform = "onebot_v11";
        kick.groupId = "g"; kick.userId = "bot"; kick.selfId = "bot";
        kick.operatorId = "admin"; kick.subType = "kick_me";
        manager.handleEvent(kick);

        BotEvent upload;
        upload.type = EventType::kGroupUpload; upload.platform = "onebot_v11";
        upload.groupId = "g"; upload.userId = "u";
        upload.extra = {{"file", {{"id", "f1"}, {"name", "a.zip"}, {"size", "12"}, {"busid", "0"}}}};
        manager.handleEvent(upload);

        BotEvent ban;
        ban.type = EventType::kGroupBan; ban.platform = "onebot_v11";
        ban.groupId = "g"; ban.userId = "bot"; ban.selfId = "bot";
        ban.subType = "ban"; ban.extra = {{"duration", 600}};
        manager.handleEvent(ban);

        BotEvent admin;
        admin.type = EventType::kGroupAdmin; admin.platform = "onebot_v11";
        admin.groupId = "g"; admin.userId = "bot"; admin.selfId = "bot"; admin.subType = "set";
        manager.handleEvent(admin);

        BotEvent recall;
        recall.type = EventType::kFriendRecall; recall.platform = "onebot_v11";
        recall.userId = "u"; recall.extra = {{"message_id", "99"}};
        manager.handleEvent(recall);

        BotEvent title;
        title.type = EventType::kGroupTitle; title.platform = "onebot_v11";
        title.groupId = "g"; title.userId = "u"; title.subType = "title";
        title.extra = {{"title", "骰主"}};
        manager.handleEvent(title);

        BotEvent honor;
        honor.type = EventType::kGroupHonor; honor.platform = "onebot_v11";
        honor.groupId = "g"; honor.userId = "u"; honor.subType = "honor";
        honor.extra = {{"honor_type", "talkative"}};
        manager.handleEvent(honor);

        BotEvent essence;
        essence.type = EventType::kEssence; essence.platform = "onebot_v11";
        essence.groupId = "g"; essence.userId = "u"; essence.subType = "add";
        essence.extra = {{"message_id", "7"}};
        manager.handleEvent(essence);

        BotEvent reject;
        reject.type = EventType::kFriendRequest; reject.platform = "onebot_v11";
        reject.adapterId = "a1"; reject.userId = "no"; reject.flag = "f-no"; reject.comment = "hi";
        manager.handleEvent(reject);
        ASSERT_TRUE(manager.friendRequestRejected());
        ASSERT_EQ(manager.lastFriendRequestReason(), std::string("拒绝"));
        ASSERT_EQ(friendCalls.size(), 1u);
        ASSERT_EQ(friendCalls[0], std::string("f-no:拒绝"));

        BotEvent approve;
        approve.type = EventType::kFriendRequest; approve.platform = "onebot_v11";
        approve.adapterId = "a1"; approve.userId = "ok"; approve.flag = "f-ok";
        manager.handleEvent(approve);
        ASSERT_FALSE(manager.friendRequestRejected());
        ASSERT_EQ(friendCalls.size(), 1u);

        const auto log = manager.evalString("globalThis.__log.join('|')");
        ASSERT_TRUE(log);
        ASSERT_TRUE(log->find("leave:g:u:leave") != std::string::npos);
        ASSERT_TRUE(log->find("leave:g:bot:kick_me") != std::string::npos);
        ASSERT_TRUE(log->find("kicked:g:bot:kick_me") != std::string::npos);
        ASSERT_TRUE(log->find("upload:g:u:::a.zip") != std::string::npos || log->find("a.zip") != std::string::npos);
        ASSERT_TRUE(log->find("ban:g:bot:ban") != std::string::npos);
        ASSERT_TRUE(log->find("admin:g:bot:set") != std::string::npos);
        ASSERT_TRUE(log->find("friendRecall::u") != std::string::npos);
        ASSERT_TRUE(log->find("deleted:99:private") != std::string::npos);
        ASSERT_TRUE(log->find("title:g:u:title::骰主") != std::string::npos);
        ASSERT_TRUE(log->find("honor:g:u:honor:::talkative") != std::string::npos);
        ASSERT_TRUE(log->find("essence:g:u:add:7") != std::string::npos);
        ASSERT_TRUE(log->find("friendReq:no:f-no:hi") != std::string::npos);
        ASSERT_TRUE(log->find("friendReq:ok:f-ok") != std::string::npos);
    }

    fs::remove_all(root, ec);
}
