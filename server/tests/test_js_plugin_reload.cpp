#include "test_framework.h"
#include "../src/core/mod/js_plugin_manager.h"
#include "../src/adapter/adapter_interface.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dice;
namespace fs = std::filesystem;

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
)JS";
        plugin.close();

        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 1);

        Message msg;
        msg.id = "raw-42";
        msg.platform = "onebot";
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
                         {"segments", json::array({json{{"type", "text"}, {"data", "x"}}})}};

        const auto received = manager.handleMessageReceived(msg);
        ASSERT_EQ(received.reply,
                  std::string("recv:raw-42:guild-7:channel-9:1:RAW-CONTENT:true"));

        const auto nonCommand = manager.handleNonCommand(msg);
        ASSERT_EQ(nonCommand.reply, std::string("not-command"));

        const auto command = manager.handle(msg, "probe abc");
        ASSERT_EQ(command.reply,
                  std::string("solve:abc\ncommand-hook:probe:RAW-CONTENT:true:true"));

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
    }

    fs::remove_all(root, ec);
}
