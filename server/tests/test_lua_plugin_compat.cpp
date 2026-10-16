#include "test_framework.h"
#include "test_watchdog.h"
#include "legacy_corpus_guard.h"
#include "../src/core/mod/lua_plugin_manager.h"
#include "../src/common/legacy_conf_value.h"
#include "../src/common/legacy_mod_limit.h"
#include "../src/common/session_deck.h"
#include "../src/core/mod/legacy_log_context.h"
#include "../src/common/utils.h"

#include <chrono>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <functional>
#include <nlohmann/json.hpp>

using namespace dice;
namespace fs = std::filesystem;

namespace {

fs::path utf8Path(const std::u8string& value) {
    return fs::path(value);
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

class TempWorkspace {
public:
    explicit TempWorkspace(const std::string& prefix)
        : previous_(fs::current_path()) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / (prefix + std::to_string(nonce));
        std::error_code ec;
        fs::create_directories(root_ / "data" / "mod", ec);
        fs::create_directories(root_ / "data" / "plugin" / utf8Path(u8"求签"), ec);
        fs::create_directories(root_ / "data" / "plugin" / "script", ec);
        fs::create_directories(root_ / "data" / "logs" / "app", ec);
        fs::current_path(root_);
    }

    ~TempWorkspace() {
        std::error_code ec;
        fs::current_path(previous_, ec);
        fs::remove_all(root_, ec);
    }

    fs::path root() const { return root_; }

private:
    fs::path previous_;
    fs::path root_;
};

#ifdef _WIN32
class ScopedCLocale {
public:
    explicit ScopedCLocale(const char* locale) {
        if (const char* current = std::setlocale(LC_ALL, nullptr)) previous_ = current;
        active_ = std::setlocale(LC_ALL, locale) != nullptr;
    }
    ~ScopedCLocale() {
        if (!previous_.empty()) std::setlocale(LC_ALL, previous_.c_str());
    }
    bool active() const { return active_; }
private:
    std::string previous_;
    bool active_ = false;
};
#endif

}  // namespace

TEST(LuaPluginCompat, TypedMigratedConfigKeepsFalseDecimalEmptyTextAndTable) {
    TempWorkspace workspace("dice_typed_migration_");
    writeText("data/plugin/typed.lua", R"LUA(
msg_order = { [".typed"] = "run" }
function run(msg)
  local flag = getUserConf(msg.fromQQ, 'flag')
  local decimal = getUserConf(msg.fromQQ, 'decimal')
  local digits = getUserConf(msg.fromQQ, 'digits')
  local empty = getUserConf(msg.fromQQ, 'empty')
  local obj = getUserConf(msg.fromQQ, 'obj')
  return type(flag)..':'..tostring(flag)..'|'..tostring(decimal)..'|'..type(digits)..':'..digits..'|'..type(empty)..':'..empty..'|'..tostring(obj.value)
end
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.confSet("u:10001", "flag", "false"); manager.confSet("u:10001", "decimal", "1.75");
    manager.confSet("u:10001", "digits", "@dice:legacy-json:\"01\"");
    manager.confSet("u:10001", "empty", "@dice:legacy-json:\"\"");
    manager.confSet("u:10001", "obj", "@dice:legacy-json:{\"value\":7}");
    ASSERT_TRUE(manager.loadDir("data/mod") > 0);
    const auto result = manager.dispatch(".typed", "10001", "20001", "Tester", "", false, 0, "onebot_v11");
    ASSERT_TRUE(result.matched); ASSERT_EQ(result.reply, "boolean:false|1.75|string:01|string:|7");
}

TEST(LuaPluginCompat, UnicodeSelfDataReadsWritesAndUppercaseJsonSuffix) {
    TempWorkspace workspace("dice_unicode_selfdata_");
    fs::create_directories("data/self_data");
    writeText(fs::path("data/self_data") / utf8Path(u8"中文 存档🌙.json"), R"({"value":7})");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); std::string error;
    ASSERT_TRUE(manager.eval(R"LUA(
      local data = getSelfData('中文 存档🌙.JSON')
      assert(data.value == 7)
      data.value = 9
      assert(getSelfData('中文 存档🌙.json').value == 9)
    )LUA", &error));
    std::ifstream input(fs::path("data/self_data") / utf8Path(u8"中文 存档🌙.json"));
    nlohmann::json saved; input >> saved; ASSERT_EQ(saved["value"],9);
}

TEST(LuaPluginCompat, MigratedSetsKeepMethodsTypesAndPersistNestedChanges) {
    TempWorkspace workspace("dice_typed_set_");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.confSet("u:10001", "flags", legacy_conf::encode(legacy_conf::setValue(nlohmann::json::array({"one",7}))));
    manager.confSet("g:20001", "object", legacy_conf::encode(nlohmann::json{{"a/b~",legacy_conf::setValue(nlohmann::json::array())},{"other",true}}));
    std::string error;
    ASSERT_TRUE(manager.eval(R"LUA(
      local flags = getUserConf(10001,'flags')
      assert(flags['in'](flags,'one') and flags[7])
      assert(flags:add('two') and flags:remove(7))
      local reread = getUserConf(10001,'flags')
      assert(reread['in'](reread,'two') and not reread[7])
      assert(#reread == 2 and #reread:totable() == 2)
      local nested = getGroupConf(20001,'object')['a/b~']
      assert(nested:add('new'))
      assert(getGroupConf(20001,'object')['a/b~']['new'])
      setUserConf(10001,'new-set',Set.new({'x',5}))
      assert(getUserConf(10001,'new-set')['in'](getUserConf(10001,'new-set'),'x'))
    )LUA", &error));
    nlohmann::json saved; ASSERT_TRUE(legacy_conf::decode(manager.confGet("g:20001","object"),saved));
    ASSERT_TRUE(saved["other"].get<bool>()); ASSERT_TRUE(legacy_conf::isSet(saved["a/b~"]));
}

TEST(LuaPluginCompat, CardSetMutationsUseTheTypedCardBridge) {
    TempWorkspace workspace("dice_card_set_");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    nlohmann::json flags=legacy_conf::setValue(nlohmann::json::array({"one"}));
    manager.setPlayerCardBridge(
      [&](const auto&,const auto&,bool,const auto& key,nlohmann::json& out) { if(key!="flags")return false; out=flags; return true; },
      [&](const auto&,const auto&,bool,const auto& key,const nlohmann::json& value) { if(key!="flags")return false; flags=value; return true; }, {}, {});
    std::string error;
    ASSERT_TRUE(manager.eval(R"LUA(
      local flags = getPlayerCardAttr(10001,20001,'flags')
      assert(flags:add('two'))
      assert(getPlayerCardAttr(10001,20001,'flags')['two'])
    )LUA", &error));
    ASSERT_TRUE(legacy_conf::isSet(flags)); ASSERT_EQ(flags["values"].size(),size_t(2));
}

TEST(LuaPluginCompat, PluginLogPrintSeverityAndDelayedOriginsReachStructuredLogs) {
    TempWorkspace workspace("dice_lua_logs_");
    writeText("data/plugin/LogOrigin.lua", R"LUA(
msg_order = { [".logorigin"] = "run" }
function run(msg)
  log('lua-own-info'); print('lua-print-marker', 42)
  logWarn('lua-own-warn'); logError('lua-own-error'); logDebug('lua-own-debug')
  sleepTime(1)
  print('lua-delayed-marker')
  return 'delayed reply'
end
)LUA");
    const auto previous = std::string(logLevelName(logRecordingLevel()));
    configureLogger(nlohmann::json{{"server", {{"log_level", "debug"}}}});
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    std::vector<std::function<void()>> callbacks;
    manager.setScheduler([&](double, std::function<void()> cb) { callbacks.push_back(std::move(cb)); });
    std::string returned, source;
    manager.setAsyncReply([&](const auto&, const auto&, const auto&, const auto& text) { returned = text; source = activeProcessingContext.sources[0].name; });
    manager.loadDir("data/mod");
    auto result = manager.dispatch(".logorigin", "42", "1001", "用户", "", false, 0, "onebot_v11");
    ASSERT_TRUE(result.matched); ASSERT_EQ(result.sources.size(), size_t(1));
    ASSERT_EQ(callbacks.size(), size_t(1)); auto callback = std::move(callbacks[0]); callbacks.clear(); callback();
    bool printed = false, warning = false, error = false, debug = false, delayed = false;
    for (const auto& record : recentLogRecords(1000)) {
        const auto text = record["message"].get<std::string>();
        if (text.find("lua-print-marker") != std::string::npos) printed = text.find("42") != std::string::npos;
        if (text.find("lua-own-warn") != std::string::npos) warning = record["level"] == "warn";
        if (text.find("lua-own-error") != std::string::npos) error = record["level"] == "error";
        if (text.find("lua-own-debug") != std::string::npos) debug = record["level"] == "debug";
        if (text.find("lua-delayed-marker") != std::string::npos) delayed = record["details"]["processing"]["sources"][0]["name"] == "LogOrigin";
    }
    configureLogger(nlohmann::json{{"server", {{"log_level", previous}}}});
    ASSERT_TRUE(printed && warning && error && debug && delayed);
    ASSERT_EQ(returned, std::string("delayed reply")); ASSERT_EQ(source, std::string("LogOrigin"));
}

TEST(LuaPluginCompat, ExecutionTimeoutCannotBeCaughtOrRemovedAndRuntimeRecovers) {
    TestWatchdog watchdog;
    TempWorkspace workspace("dice_next_lua_deadline_");
    LuaPluginManager manager(std::chrono::milliseconds(100), 8 * 1024 * 1024);
    ASSERT_TRUE(manager.init());
    for (const std::string code : {
        "while true do end",
        "while true do pcall(function() while true do end end) end",
        "pcall(debug.sethook); while true do end",
        "local co=coroutine.create(function() while true do end end); while true do coroutine.resume(co) end",
        "local co=coroutine.wrap(function() while true do end end); while true do pcall(co) end"
    }) {
        std::string error;
        ASSERT_FALSE(manager.eval(code, &error));
        ASSERT_TRUE(error.find("超时") != std::string::npos);
        ASSERT_TRUE(manager.eval("assert(1 + 1 == 2)"));
    }
    std::string error;
    ASSERT_FALSE(manager.eval("huge = string.rep('x', 32 * 1024 * 1024)", &error));
    ASSERT_TRUE(manager.eval("assert(2 + 2 == 4)"));
    ASSERT_FALSE(manager.eval("os.exit(0)", &error));
    ASSERT_TRUE(error.find("os.exit") != std::string::npos);
}

TEST(LuaPluginCompat, NativePipeCapturesOutputAndExitStatus) {
    TestWatchdog watchdog;
    TempWorkspace workspace("dice_next_lua_pipe_");
    // A healthy shell needs headroom for Windows process startup. Keep the
    // separate cancellation/finalizer test at 100 ms; don't weaken its budget.
    LuaPluginManager manager(std::chrono::seconds(2));
    ASSERT_TRUE(manager.init());
    std::string error;
    const bool healthyPipe = manager.eval("local f, err=io.popen('echo probe'); assert(f, err); assert(f:read('*a'):find('probe')); local ok, kind, status=f:close(); assert(ok and kind=='exit' and status==0)", &error);
    if (!healthyPipe) std::cerr << "  native pipe probe: " << error << '\n';
    ASSERT_TRUE(healthyPipe);
#ifdef _WIN32
    const std::string failedCommand = "exit /b 7";
#else
    const std::string failedCommand = "exit 7";
#endif
    const bool failedPipe = manager.eval("local f, err=io.popen('" + failedCommand + "'); assert(f, err); local ok, kind, status=f:close(); assert(ok==nil and kind=='exit' and status==7)", &error);
    if (!failedPipe) std::cerr << "  native pipe exit status: " << error << '\n';
    ASSERT_TRUE(failedPipe);
}

TEST(LuaPluginCompat, NativeCommandsAndFinalizersCannotKeepTheRuntimeBusyForever) {
    TestWatchdog watchdog;
    TempWorkspace workspace("dice_next_lua_native_");
    LuaPluginManager manager(std::chrono::milliseconds(100));
    ASSERT_TRUE(manager.init());
#ifdef _WIN32
    const std::string slow = "ping -n 15 127.0.0.1 >nul";
#else
    const std::string slow = "sleep 10";
#endif
    std::string error;
    ASSERT_FALSE(manager.eval("os.execute('" + slow + "')", &error));
    ASSERT_TRUE(error.find("超时") != std::string::npos);
    ASSERT_FALSE(manager.eval("io.popen('" + slow + "')", &error));
    ASSERT_TRUE(error.find("超时") != std::string::npos);
    ASSERT_TRUE(manager.eval("holder=setmetatable({}, {__gc=function() while true do pcall(function() while true do end end) end end})"));
    manager.freeRuntime();
    ASSERT_TRUE(manager.init());
    ASSERT_TRUE(manager.eval("assert(4 + 4 == 8)"));
}

TEST(LuaPluginCompat, TimedOutPluginIsSuspendedButManagementAndHealthyRepliesRecover) {
    TestWatchdog watchdog;
    TempWorkspace workspace("dice_next_lua_circuit_");
    writeText(workspace.root() / "data/plugin/loop.lua", R"LUA(
msg_order = { loop = 'loop', healthy = 'healthy' }
function loop(msg) while true do end end
function healthy(msg) return 'healthy' end
)LUA");
    LuaPluginManager manager(std::chrono::milliseconds(100));
    ASSERT_TRUE(manager.init());
    manager.loadDir("data/mod");
    ASSERT_TRUE(manager.dispatch("loop", "u", "g", "tester", "", false).matched);
    ASSERT_FALSE(manager.dispatch("healthy", "u", "g", "tester", "", false).matched);
    const auto mods = manager.mods();
    ASSERT_TRUE(!mods.empty());
    ASSERT_TRUE(!mods.front().compatibilityWarnings.empty());
    writeText(workspace.root() / "data/plugin/loop.lua", R"LUA(
msg_order = { healthy = 'healthy' }
function healthy(msg) return 'healthy' end
)LUA");
    manager.reload();
    ASSERT_EQ(manager.dispatch("healthy", "u", "g", "tester", "", false).reply, std::string("healthy"));
}

TEST(LuaPluginCompat, UnsupportedLegacyActionsAreVisibleAndNeverRunPartialFallbacks) {
    TempWorkspace workspace("dice_next_lua_diagnostics_");
    const auto mod = workspace.root() / "data" / "mod" / "unsupported";
    fs::create_directories(mod / "reply"); fs::create_directories(mod / "event");
    writeText(mod / "descriptor.json", R"({"title":"兼容诊断"})");
    writeText(mod / "reply" / "unsupported.toml", R"TOML(
[reply.javascript]
keyword.match = "legacyjs"
echo.js = "old.js"
echo.lua = "must not run a partial fallback"
[reply.unsupported_condition]
keyword.match = "unsafe"
limit.unknown = true
echo = "must not run without its condition"
)TOML");
    writeText(mod / "event" / "mixed.lua", R"LUA(
event = { mixed = { trigger = { cycle = 10, hook = "WordCensored" }, action = { lua = "never" } } }
)LUA");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto mods = manager.mods();
    ASSERT_EQ(mods.size(), size_t(1));
    ASSERT_EQ(mods[0].events, 0);
    ASSERT_EQ(mods[0].compatibilityWarnings.size(), size_t(3));
    ASSERT_FALSE(manager.dispatch("legacyjs", "u", "g", "n", "", false).matched);
    ASSERT_FALSE(manager.dispatch("unsafe", "u", "g", "n", "", false).matched);
}

TEST(LuaPluginCompat, TomlLiteralStringsKeepHashesCommasAndRegexBackslashes) {
    TempWorkspace workspace("dice_next_lua_literal_toml_");
    const auto mod = workspace.root() / "data" / "mod" / "literal";
    fs::create_directories(mod / "reply");
    writeText(mod / "descriptor.json", R"({"title":"literal"})");
    writeText(mod / "reply" / "literal.toml", R"TOML(
[reply.literal]
keyword.match = ['hash#word', 'comma,word'] # real comment
echo = 'literal#content, with \backslash'
[reply.regex]
keyword.regex = '^num\d+$'
echo = 'regex'
)TOML");
    writeText(mod / "reply" / "invalid.toml", R"TOML(
[reply.unsafe]
keyword.match = "invalid"
limit.cd = invalid
echo = "must not silently disable cooldown"
)TOML");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    ASSERT_EQ(manager.dispatch("hash#word", "u", "g", "n", "", false).reply, "literal#content, with \\backslash");
    ASSERT_EQ(manager.dispatch("comma,word", "u", "g", "n", "", false).reply, "literal#content, with \\backslash");
    ASSERT_EQ(manager.dispatch("num123", "u", "g", "n", "", false).reply, "regex");
    ASSERT_FALSE(manager.dispatch("invalid", "u", "g", "n", "", false).matched);
    ASSERT_EQ(manager.mods()[0].compatibilityWarnings.size(), size_t(1));
}

TEST(LuaPluginCompat, LegacySiblingLoadLuaAndTopLevelHttpRefreshPerInvocation) {
    TempWorkspace workspace("dice_next_lua_compat_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";

    writeText(pluginDir / utf8Path(u8"求签3.0.lua"), R"LUA(
msg_order = {}
index_deck = { ["月老"] = "月老灵签" }

function draw_today(msg)
    local deck_key = string.match(msg.fromMsg, "^[%s]*([^%s]*)[%s]*(.-)$", #("求签") + 1) or "月老"
    local deck_name = index_deck[deck_key]
    if not deck_name then return "unknown:" .. deck_key end
    local target = loadLua("求签/" .. deck_name)
    if not target then return "missing:" .. deck_name end
    return target.getDraw(msg)
end

function draw_modern(msg)
    local target = loadLua("modern")
    return target and (target.value .. ":" .. target.count) or "missing-modern"
end

function draw_escape(msg)
    local target = loadLua("../secret")
    return target and "unsafe" or "blocked"
end

msg_order["求签"] = "draw_today"
msg_order["现代"] = "draw_modern"
msg_order["越界"] = "draw_escape"
)LUA");

    writeText(pluginDir / utf8Path(u8"求签") / utf8Path(u8"月老灵签.lua"), R"LUA(
return {
    getDraw = function(msg)
        return "月老签:" .. msg.fromQQ
    end
}
)LUA");

    writeText(pluginDir / "script" / "modern.lua", R"LUA(
helper_counter = (helper_counter or 0) + 1
return { value = "modern-ok", count = helper_counter }
)LUA");
    writeText(workspace.root() / "data" / "secret.lua", "return { value = \"must-not-load\" }\n");

    writeText(pluginDir / "DailyNews-fixed.lua", R"LUA(
msg_order = {}
local ok, info = http.get("http://excerpt.example/toolman/getMiniNews")
local js = require "json"
local decoded = js.decode(info)

function dailynews(msg)
    if not ok or not decoded then return "news-failed" end
    return "新闻:" .. decoded.data.image
end

msg_order["60s"] = "dailynews"
)LUA");

    int fetchCalls = 0;
    {
        LuaPluginManager manager;
        manager.setHttpFetch([&](const std::string& method, const std::string& url,
                                 const std::string&, const std::string&, int& status) {
            ++fetchCalls;
            status = 200;
            return method == "GET" && url == "http://excerpt.example/toolman/getMiniNews"
                ? std::string(R"JSON({"data":{"image":"https://img.example/daily-)JSON") +
                      std::to_string(fetchCalls) + ".png\"}}"
                : std::string();
        });

        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 2);
        ASSERT_EQ(fetchCalls, 1);

        const auto draw = manager.dispatch(
            "求签 月老", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(draw.matched);
        ASSERT_EQ(draw.reply, std::string("月老签:user-42"));

        const auto modern = manager.dispatch(
            "现代", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(modern.matched);
        ASSERT_EQ(modern.reply, std::string("modern-ok:1"));
        const auto modernAgain = manager.dispatch(
            "现代", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(modernAgain.matched);
        ASSERT_EQ(modernAgain.reply, std::string("modern-ok:1"));

        const auto escape = manager.dispatch(
            "越界", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(escape.matched);
        ASSERT_EQ(escape.reply, std::string("blocked"));

        const auto news = manager.dispatch(
            "60s", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(news.matched);
        ASSERT_EQ(news.reply, std::string("新闻:https://img.example/daily-2.png"));
        ASSERT_EQ(fetchCalls, 2);

        const auto nextNews = manager.dispatch(
            "60s", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(nextNews.matched);
        ASSERT_EQ(nextNews.reply, std::string("新闻:https://img.example/daily-3.png"));
        ASSERT_EQ(fetchCalls, 3);
    }
}

TEST(LuaPluginCompat, LegacyTaskCallRegistersAndRuns) {
    TempWorkspace workspace("dice_next_lua_task_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    writeText(pluginDir / "task.lua", R"LUA(
task_call = { news = "run_news" }
msg_order = {}
local ok, payload = http.get("https://news.example/task")
function run_news()
    eventMsg("task fired:" .. (ok and payload or "failed"), "g", "u")
end
)LUA");

    LuaPluginManager manager;
    int fetchCalls = 0;
    std::string fired;
    manager.setHttpFetch([&](const std::string&, const std::string&,
                             const std::string&, const std::string&, int& status) {
        status = 200;
        return std::to_string(++fetchCalls);
    });
    manager.setEventMsg([&](const std::string& text, const std::string&, const std::string&) {
        fired = text;
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    ASSERT_EQ(fetchCalls, 1);
    ASSERT_TRUE(manager.hasTask("news"));
    ASSERT_EQ(manager.taskNames().size(), static_cast<size_t>(1));
    std::string error;
    ASSERT_TRUE(manager.runTask("news", &error));
    ASSERT_EQ(fired, std::string("task fired:2"));
    ASSERT_EQ(fetchCalls, 2);
    ASSERT_TRUE(manager.runTask("news", &error));
    ASSERT_EQ(fired, std::string("task fired:3"));
    ASSERT_EQ(fetchCalls, 3);
}

TEST(LuaPluginCompat, LegacyTomlReplyLoadsLuaActionAndHonorsRuleScope) {
    TempWorkspace workspace("dice_next_lua_toml_reply_");
    const fs::path mod = workspace.root() / "data" / "mod" / "BRP-TRPG";
    std::error_code ec;
    fs::create_directories(mod / "reply", ec);
    fs::create_directories(mod / "script" / "BRP", ec);
    writeText(mod / "descriptor.json", R"JSON({"title":"BRP","ver":"1.0","helpdoc":{"team":"team help"}})JSON");
    writeText(mod / "reply" / "sn.toml", R"TOML(
[reply.BRP_sn]
type = "Game"
rule = "BRP"
keyword.prefix = ".sn"
limit.grp_id.nor = 0
echo.lua = "BRP.sn"

[reply.BRP_team]
type = "Game"
rule = "BRP"
keyword.prefix = ".team"
echo.lua = "BRP.team"

[reply.team_help]
type = "Order"
keyword.prefix = ".fallback"
echo = "{help:team}"

[reply.aaa_generic]
type = "Order"
keyword.prefix = ".shared"
echo = "fallback"

[reply.zzz_specific]
type = "Game"
rule = "BRP"
keyword.prefix = ".shared"
echo = "specific"
)TOML");
    writeText(mod / "script" / "BRP" / "sn.lua", R"LUA(
return "sn:" .. msg.suffix
)LUA");
    writeText(mod / "script" / "BRP" / "team.lua", R"LUA(
local before = #msg.game.pls
msg.game.pls:add("p3")
return "{help:team}:" .. before .. ":" .. #msg.game.pls
)LUA");

    LuaPluginManager manager;
    bool brpActive = false;
    manager.setRuleGate([&](const std::string&, const std::string&, const std::string& user,
                            const std::string& rule, const std::string&) {
        return brpActive && user == "u" && rule == "BRP";
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    ASSERT_EQ(manager.mods().front().replies, 1);

    auto inactive = manager.dispatch(".sn auto", "u", "g", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(!inactive.matched);
    ASSERT_EQ(manager.dispatch(".shared", "u", "g", "Tester", "", false).reply, std::string("fallback"));
    brpActive = true;
    ASSERT_EQ(manager.dispatch(".shared", "u", "g", "Tester", "", false).reply, std::string("specific"));
    // 宿主先提取命令首词再做 ownership probe；参数只在真正 dispatch 时参与。
    ASSERT_TRUE(manager.hasCommandTrigger(".sn", "u", "g", "Tester", "", false, 0, "onebot"));
    auto active = manager.dispatch(".sn auto", "u", "g", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(active.matched);
    ASSERT_EQ(active.reply, std::string("sn:auto"));

    manager.confSet("game:g", "__name", "test");
    manager.confSet("game:g", "__pls", R"JSON(["u","p2"])JSON");
    auto team = manager.dispatch(".team show", "u", "g", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(team.matched);
    ASSERT_EQ(team.reply, std::string("team help:2:3"));
    const auto players = nlohmann::json::parse(manager.confGet("game:g", "__pls"), nullptr, false);
    ASSERT_TRUE(players.is_array());
    ASSERT_TRUE(std::find(players.begin(), players.end(), "p3") != players.end());

    auto help = manager.dispatch(".fallback", "u", "g", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(help.matched);
    ASSERT_EQ(help.reply, std::string("team help"));
}

TEST(LuaPluginCompat, LegacyReplySupportsCaseVariantsRegexAndVariableLimits) {
    TempWorkspace workspace("dice_next_lua_reply_limits_");
    const fs::path mod = workspace.root() / "data" / "mod" / "compat";
    std::error_code ec;
    fs::create_directories(mod / "reply", ec);
    writeText(mod / "descriptor.json", R"JSON({"title":"compat"})JSON");
    writeText(mod / "reply" / "rules.lua", R"LUA(
msg_reply.upper = {
  keyword = { Prefix = ".upper" },
  limit = { grp_var = { enabled = true } },
  echo = "upper:{suffix}"
}
msg_reply.regex = {
  keyword = { Regex = [[^hello\s+(.+)$]] },
  echo = "regex:{suffix}"
}
msg_reply.unblocked = {
  keyword = { Match = "ask" },
  limit = { user_var = { blocked = false } },
  echo = "allowed"
}
)LUA");

    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    ASSERT_FALSE(manager.hasCommandTrigger(".upper", "u", "g", "Tester", "", false));

    std::string error;
    ASSERT_TRUE(manager.eval("setGroupConf('g', 'enabled', true)", &error));
    ASSERT_EQ(manager.confGet("g:g", "enabled"), std::string("true"));
    ASSERT_TRUE(manager.hasCommandTrigger(".upper", "u", "g", "Tester", "", false));
    auto upper = manager.dispatch(".upper value", "u", "g", "Tester", "", false);
    ASSERT_TRUE(upper.matched);
    ASSERT_EQ(upper.reply, std::string("upper:value"));

    auto regex = manager.dispatch("hello world", "u", "g", "Tester", "", false);
    ASSERT_TRUE(regex.matched);
    ASSERT_EQ(regex.reply, std::string("regex:world"));

    auto allowed = manager.dispatch("ask", "u", "g", "Tester", "", false);
    ASSERT_TRUE(allowed.matched);
    ASSERT_EQ(allowed.reply, std::string("allowed"));
    ASSERT_TRUE(manager.eval("setUserConf('u', 'blocked', true)", &error));
    ASSERT_EQ(manager.confGet("u:u", "blocked"), std::string("true"));
    ASSERT_FALSE(manager.dispatch("ask", "u", "g", "Tester", "", false).matched);
}

TEST(LuaPluginCompat, LegacyTypedConditionsAndWholeMessageRegex) {
    TempWorkspace workspace("dice_next_lua_typed_conditions_");
    const fs::path mod = workspace.root() / "data" / "mod" / "typed";
    fs::create_directories(mod / "reply");
    writeText(mod / "reply" / "rules.lua", R"LUA(
msg_reply.truth = {
    keyword = { Match = "truth" }, limit = { user_var = { puzzle = true } }, echo = "truthy"
}
msg_reply.range = {
    keyword = { Match = "range" },
    limit = { user_var = { score = { at_least = 2 }, trust = { at_most = 3 } } }, echo = "in-range"
}
msg_reply.private = {
    keyword = { Match = "private" }, limit = { grp_var = { missing = true } }, echo = "private-ok"
}
msg_reply.regex = { keyword = { Regex = [[a{2,4}]] }, echo = "regex-ok" }
msg_reply.long = { keyword = { Regex = [[z+]] }, echo = "long-ok" }
msg_reply.game = {
    keyword = { Match = "gameflag" },
    echo = function(msg)
        msg.game.active = false
        if msg.game.active == false then return "bool-ok" end
        return "bad"
    end
}
)LUA");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    auto run = [&](const std::string& text, int trust = 0) {
        return manager.dispatch(text, "u", "g", "Tester", "", false, trust);
    };
    ASSERT_FALSE(run("truth").matched);
    manager.confSet("u:u", "puzzle", "flower puzzle text");
    ASSERT_EQ(run("truth").reply, std::string("truthy"));
    manager.confSet("u:u", "puzzle", "0");
    ASSERT_TRUE(run("truth").matched);
    manager.confSet("u:u", "puzzle", "false");
    ASSERT_FALSE(run("truth").matched);
    manager.confSet("u:u", "score", "1");
    ASSERT_FALSE(run("range").matched);
    manager.confSet("u:u", "score", "2");
    ASSERT_EQ(run("range", 3).reply, std::string("in-range"));
    ASSERT_FALSE(run("range", 4).matched);
    ASSERT_FALSE(manager.hasCommandTrigger("range", "u", "g", "Tester", "", false, 4));
    ASSERT_FALSE(run("private").matched);
    ASSERT_EQ(manager.dispatch("private", "u", "", "Tester", "", true).reply, std::string("private-ok"));
    ASSERT_EQ(run("aaa").reply, std::string("regex-ok"));
    ASSERT_EQ(run("AAA").reply, std::string("regex-ok"));
    ASSERT_FALSE(run("xaaa").matched);
    ASSERT_FALSE(run("aaaaa").matched);
    ASSERT_TRUE(run(std::string(400, 'z')).matched);
    ASSERT_FALSE(run(std::string(401, 'z')).matched);
    manager.confSet("game:g", "__name", "test");
    ASSERT_EQ(run("gameflag").reply, std::string("bool-ok"));
}

TEST(LuaPluginCompat, WholeRegexBacktracksForAlternativesLazyQuantifiersAndChineseLimits) {
    TempWorkspace workspace("dice_lua_regex_full_");
    const fs::path mod = workspace.root() / "data" / "mod" / "regex";
    fs::create_directories(mod / "reply");
    writeText(mod / "reply" / "rules.lua", R"LUA(
msg_reply.alt = { keyword = { Regex = [[(a|ab)]] }, echo = "alt:{suffix}" }
msg_reply.lazy = { keyword = { Regex = [[(z+?)]] }, echo = "lazy:{suffix}" }
msg_reply.chinese = { keyword = { Regex = [[^白(丽|麗).{0,6}]] }, echo = "chinese-ok" }
msg_reply.spaces = { keyword = { Regex = [[( a )]] }, echo = "spaces:{suffix}" }
)LUA");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    auto run = [&](const std::string& text) { return manager.dispatch(text, "u", "g", "Tester", "", false); };
    ASSERT_EQ(run("ab").reply, "alt:ab");
    ASSERT_FALSE(run("abc").matched);
    ASSERT_EQ(run("zzzz").reply, "lazy:zzzz");
    ASSERT_FALSE(run("zzzz!").matched);
    ASSERT_EQ(run("白丽").reply, "chinese-ok");
    ASSERT_EQ(run("白麗一二三四五六").reply, "chinese-ok");
    ASSERT_FALSE(run("白麗一二三四五六七").matched);
    ASSERT_EQ(run(" a ").reply, "spaces: a ");
}

TEST(LuaPluginCompat, TomlVariableConditionsFailClosedWhenUnsupported) {
    TempWorkspace workspace("dice_next_lua_toml_limits_");
    const auto mod = workspace.root() / "data" / "mod" / "limits";
    fs::create_directories(mod / "reply");
    writeText(mod / "reply" / "limits.toml", R"TOML(
[reply.score]
keyword.Match = "score"
limit.user_var.score.at_least = 2
limit.user_var.trust.at_most = 3
echo = "score-ok"
[reply.existing]
keyword.match = "existing"
limit.user_var.puzzle = true
echo = "exists"
[reply.private]
keyword.match = "group-only"
limit.grp_id.nor = 0
echo = "group"
[reply.restricted]
keyword.match = "restricted"
limit.self_var.secret = true
echo = "must-not-run"
)TOML");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    auto run = [&](const std::string& text, int trust = 0) {
        return manager.dispatch(text, "u", "g", "Tester", "", false, trust);
    };
    ASSERT_FALSE(run("score").matched);
    manager.confSet("u:u", "score", "2");
    ASSERT_EQ(run("score", 3).reply, std::string("score-ok"));
    ASSERT_FALSE(run("score", 4).matched);
    ASSERT_FALSE(run("existing").matched);
    manager.confSet("u:u", "puzzle", "flower");
    ASSERT_EQ(run("existing").reply, std::string("exists"));
    ASSERT_TRUE(run("group-only").matched);
    ASSERT_FALSE(manager.dispatch("group-only", "u", "", "Tester", "", true).matched);
    ASSERT_FALSE(run("restricted").matched);
}

TEST(LuaPluginCompat, DirectoryScriptGlobalsArePerInvocationButNestedScriptsShareContext) {
    TempWorkspace workspace("dice_script_env_");
    fs::create_directories("data/mod/test/reply"); fs::create_directories("data/mod/test/script");
    writeText("data/mod/test/reply/main.lua", R"LUA(
msg_reply.first={keyword={Match="first"},echo={lua="entry"}}
msg_reply.other={keyword={Match="other"},echo={lua="other"}}
msg_reply.inline={keyword={Match="inline"},echo={lua="scratch=(scratch or 0)+1;return tostring(scratch)"}}
)LUA");
    writeText("data/mod/test/script/entry.lua", R"LUA(
scratch=(scratch or 0)+1
loadLua('helper')
assert(helperRan==1 and helperFunction()=='nested')
return tostring(scratch)..':'..msg.uid
)LUA");
    writeText("data/mod/test/script/helper.lua", "helperRan=(helperRan or 0)+1;function helperFunction()return 'nested'end");
    writeText("data/mod/test/script/other.lua", "return tostring(scratch==nil and helperRan==nil and helperFunction==nil)");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"),1);
    auto run=[&](const std::string& text,const std::string& uid="1") {
        return manager.dispatch(text,uid,"2","N","",false).reply;
    };
    ASSERT_EQ(run("first"),std::string("1:1"));
    ASSERT_EQ(run("first","3"),std::string("1:3"));
    ASSERT_EQ(run("other"),std::string("true"));
    ASSERT_EQ(run("inline"),std::string("1"));
    ASSERT_EQ(run("inline"),std::string("1"));
    ASSERT_TRUE(manager.eval("assert(scratch==nil and helperRan==nil and helperFunction==nil)"));
}

TEST(LuaPluginCompat, DirectoryScriptEventsAndSuspendedRepliesKeepSeparateContexts) {
    TempWorkspace workspace("dice_script_async_env_");
    fs::create_directories("data/mod/test/reply"); fs::create_directories("data/mod/test/script");
    fs::create_directories("data/mod/test/event");
    writeText("data/mod/test/reply/main.lua", "msg_reply.wait={keyword={Match='wait'},echo={lua='wait'}}");
    writeText("data/mod/test/script/wait.lua", R"LUA(
scratch=(scratch or 0)+1; who=msg.uid
sleepTime(10)
return tostring(scratch)..':'..who..':'..msg.uid
)LUA");
    writeText("data/mod/test/event/main.lua", "event.add={trigger={hook='FriendAdd'},action={lua='hook'}}");
    writeText("data/mod/test/script/hook.lua", R"LUA(
scratch=(scratch or 0)+1
event.privateCounter=scratch
event.sameContext=(event==msg)
)LUA");
    LuaPluginManager manager; std::vector<std::function<void()>> callbacks; std::vector<std::string> replies;
    manager.setScheduler([&](double,auto callback){callbacks.push_back(std::move(callback));});
    manager.setAsyncReply([&](const auto&,const auto&,const auto&,const auto& text){replies.push_back(text);});
    ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"),1);
    ASSERT_TRUE(manager.dispatch("wait","1","2","N","",false).pending);
    ASSERT_TRUE(manager.dispatch("wait","3","2","N","",false).pending);
    for (int i=0;i<2;++i) {
        nlohmann::json context={{"uid","5"},{"gid","2"}}; manager.dispatchHook("FriendAdd",context);
        ASSERT_EQ(context["privateCounter"],1); ASSERT_EQ(context["sameContext"],true);
    }
    ASSERT_EQ(callbacks.size(),size_t(2));
    callbacks[1](); callbacks[0]();
    ASSERT_EQ(replies.size(),size_t(2));
    ASSERT_EQ(replies[0],std::string("1:3:3")); ASSERT_EQ(replies[1],std::string("1:1:1"));
}

TEST(LuaPluginCompat, ReentrantEventMsgDispatchRestoresOuterMessageForTemplateRendering) {
    TempWorkspace workspace("dice_reentrant_message_");
    fs::create_directories("data/mod/test/reply"); fs::create_directories("data/mod/test/script");
    writeText("data/mod/test/reply/main.lua", R"LUA(
msg_reply.file={keyword={Match='outer-file'},echo={lua='outer'}}
msg_reply.func={keyword={Match='outer-func'},echo=function()
 msg.value=msg.nick;eventMsg('inner','8','9');return '{nick}:{value}:{uid}'
end}
msg_reply.inner={keyword={Match='inner'},echo=function()
 msg.value='inner-value';return '{nick}:{value}:{uid}'
end}
)LUA");
    writeText("data/mod/test/script/outer.lua", "msg.value=msg.nick;eventMsg('inner','8','9');return '{nick}:{value}:{uid}'");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"),1);
    std::vector<std::string> innerReplies;
    manager.setEventMsg([&](const auto& text,const auto& gid,const auto& uid) {
        innerReplies.push_back(manager.dispatch(text,uid,gid,"Inner","",false).reply);
    });
    ASSERT_TRUE(manager.eval("msg={marker='host'}"));
    ASSERT_EQ(manager.dispatch("outer-file","1","2","Outer1","",false).reply,std::string("Outer1:Outer1:1"));
    ASSERT_EQ(manager.dispatch("outer-func","3","2","Outer3","",false).reply,std::string("Outer3:Outer3:3"));
    ASSERT_EQ(innerReplies.size(),size_t(2));
    for(const auto& reply:innerReplies) ASSERT_EQ(reply,std::string("Inner:inner-value:9"));
    ASSERT_TRUE(manager.eval("assert(msg.marker=='host')"));
}

TEST(LuaPluginCompat, SpeechCaseAliasesChoicesAndOverridesMatchLegacyDice) {
    TempWorkspace workspace("dice_speech_compat_");
    fs::create_directories("data/mod/first/speech");
    fs::create_directories("data/mod/second/speech");
    writeText("data/mod/first/descriptor.json", R"JSON({"speech":{
      "Reply_Spells_List":"spell:{INV}","Array":["a:{nick}","b:{nick}"],
      "Empty":[],"EmptyText":"","Invalid":["not-active",{}],"Alias":"&REPLY_SPELLS_LIST"
    }})JSON");
    writeText("data/mod/first/speech/words.yaml", R"YAML(
yamlChoices: ["y:{NICK}", "z:{nick}"]
CHINESE词条: "中文:{Nick}"
loopA: "&LOOPB"
loopB: "{LOOPA}"
yamlEmpty: []
yamlInvalid: [ok, {nested: not-a-text}]
)YAML");
    writeText("data/mod/second/descriptor.json", R"JSON({"speech":{"reply_SPELLS_list":"overridden:{inv}"}})JSON");
    writeText("data/mod/second/speech/words.yaml", "Reply_Spells_List: 'yaml-override:{inv}'\n");
    writeText("data/mod_order.json", R"(["first","second"])");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir("data/mod"),2);
    const std::map<std::string,std::string> vars{{"inv","Torch"},{"nick","Tester"}};
    ASSERT_EQ(manager.formatTemplate("{reply_spells_LIST}",vars),std::string("yaml-override:Torch"));
    ASSERT_EQ(manager.formatTemplate("{ALIAS}",vars),std::string("yaml-override:Torch"));
    ASSERT_EQ(manager.formatTemplate("{chinese词条}",vars),std::string("中文:Tester"));
    ASSERT_EQ(manager.formatTemplate("{Empty}{EmptyText}{Invalid}{yamlEmpty}{yamlInvalid}{LoopA}",vars),std::string());
    ASSERT_EQ(manager.formatTemplate("{NICK}",{{"NICK","Exact"},{"nick","fallback"}}),std::string("Exact"));
    std::set<std::string> jsonChoices, yamlChoices;
    for (int i=0;i<256;++i) {
        jsonChoices.insert(manager.formatTemplate("{ARRAY}",vars));
        yamlChoices.insert(manager.formatTemplate("{YAMLCHOICES}",vars));
    }
    ASSERT_TRUE((jsonChoices == std::set<std::string>{"a:Tester","b:Tester"}));
    ASSERT_TRUE((yamlChoices == std::set<std::string>{"y:Tester","z:Tester"}));
    ASSERT_TRUE(manager.setModEnabled("second",false));
    ASSERT_EQ(manager.reload(),2);
    ASSERT_EQ(manager.formatTemplate("{alias}",vars),std::string("spell:Torch"));
}

TEST(LuaPluginCompat, LegacyNamedScriptsIgnoreCaseAndRespectGlobalOverrideOrder) {
    TempWorkspace workspace("dice_script_names_");
    fs::create_directories("data/mod/first/reply"); fs::create_directories("data/mod/first/script/Tools");
    fs::create_directories("data/mod/second/script/tools");
    writeText("data/mod/first/reply/rules.lua", "msg_reply.test={keyword={Match='named'},echo={lua='entry'}}");
    writeText("data/mod/first/script/entry.lua", "return loadLua('TOOLS.PING')");
    writeText("data/mod/first/script/Tools/Ping.lua", "return 'first'");
    writeText("data/mod/second/script/tools/pING.lua", "return 'second'");
    writeText("data/mod_order.json", R"(["first","second"])");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"),2);
    ASSERT_EQ(manager.dispatch("named","1","2","N","",false).reply,std::string("second"));
    ASSERT_EQ(manager.resolveScript("tOoLs.PiNg.LUA","data/mod/first").filename().string(),std::string("pING.lua"));
    ASSERT_TRUE(manager.resolveScript("../escape","data/mod/first").empty());
    ASSERT_TRUE(manager.resolveScript("C:/outside","data/mod/first").empty());
    ASSERT_TRUE(manager.setModEnabled("second",false)); ASSERT_EQ(manager.reload(),2);
    ASSERT_EQ(manager.dispatch("named","1","2","N","",false).reply,std::string("first"));
}

TEST(LuaPluginCompat, LegacyInitScriptsExposePackageNamesAndKeepExplicitInitAliases) {
    TempWorkspace workspace("dice_script_packages_");
    fs::create_directories("data/mod/test/script/ToolBox/Nested");
    writeText("data/mod/test/script/ToolBox/init.lua", "return 'package'");
    writeText("data/mod/test/script/ToolBox/Nested/init.lua", "return 'nested'");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"),1);
    const auto package = manager.resolveScript("TOOLBOX","data/mod/test");
    ASSERT_TRUE(!package.empty());
    ASSERT_EQ(manager.resolveScript("toolbox.init","data/mod/test"),package);
    ASSERT_EQ(manager.resolveScript("ToOlBoX/NeStEd","data/mod/test").filename().string(),std::string("init.lua"));
    ASSERT_TRUE(manager.eval("assert(loadLua('toolbox')=='package' and loadLua('toolbox.nested')=='nested')"));
}

TEST(LuaPluginCompat, LegacyHelpNamesAliasesAndListOverridesUseOneGlobalNamespace) {
    TempWorkspace workspace("dice_help_names_");
    fs::create_directories("data/mod/first"); fs::create_directories("data/mod/second");
    writeText("data/mod/first/descriptor.json", R"JSON({"helpdoc":{
      "Topic":"first:{nick}","Alias":"&tOpIc","Chain":"&ALIAS",
      "loop1":"&loop2","loop2":"&loop1"
    }})JSON");
    writeText("data/mod/second/descriptor.json", R"JSON({"helpdoc":{"tOPIC":"second:{NICK}","External":"&cHaIn"}})JSON");
    writeText("data/mod_order.json", R"(["first","second"])");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"),2);
    const std::map<std::string,std::string> vars{{"nick","Tester"}};
    ASSERT_EQ(manager.formatTemplate("{HELP:topic}",vars),std::string("second:Tester"));
    ASSERT_EQ(manager.formatTemplate("{help:CHAIN}",vars),std::string("second:Tester"));
    ASSERT_EQ(manager.formatTemplate("{help:External}",vars),std::string("second:Tester"));
    auto entries = manager.helpEntries();
    size_t topics = 0; std::string alias, external, loop;
    for(const auto& entry:entries) {
        if(legacy_mod::lower(entry.topic)=="topic") { ++topics; ASSERT_EQ(entry.mod,std::string("second")); }
        if(entry.topic=="Alias") alias=entry.text;
        if(entry.topic=="External") external=entry.text;
        if(entry.topic=="loop1") loop=entry.text;
    }
    ASSERT_EQ(topics,size_t(1)); ASSERT_EQ(alias,std::string("second:{NICK}"));
    ASSERT_EQ(external,std::string("second:{NICK}")); ASSERT_TRUE(loop.empty());
    ASSERT_TRUE(manager.setModEnabled("second",false)); ASSERT_EQ(manager.reload(),2);
    ASSERT_EQ(manager.formatTemplate("{help:alias}",vars),std::string("first:Tester"));
}

TEST(LuaPluginCompat, LegacyHelpTemplatesResolveTheirOwnNamespaceWithBoundedExpansion) {
    TempWorkspace workspace("dice_help_templates_");
    fs::create_directories("data/mod/test");
    writeText("data/mod/test/descriptor.json", R"JSON({
      "helpdoc":{"Topic":"old","TOPIC":"new","Index":"{NICK}/{topic}/{fromSpeech}/{Self}",
        "Nick":"must-not-replace-user","Alias":"&TOPIC","ContextAlias":"&NICK",
        "loop":"{loop}","many":"{many}{many}{many}{many}{many}{many}{many}{many}{many}{many}"},
      "speech":{"fromSpeech":"{alias}/{sample:fixed|fixed}","TOPIC":"speech-not-help"}
    })JSON");
    LuaPluginManager manager; manager.setSelfName("Maid"); ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir("data/mod"),1);
    const std::map<std::string,std::string> vars{{"nick","Tester"}};
    ASSERT_EQ(manager.formatTemplate("{help:INDEX}",vars),std::string("Tester/new/new/fixed/Maid"));
    ASSERT_EQ(manager.formatHelpTemplate("{INDEX}",vars),std::string("Tester/new/new/fixed/Maid"));
    ASSERT_EQ(manager.formatHelpTemplate("&contextAlias",vars),std::string("Tester"));
    ASSERT_EQ(manager.formatHelpTopic("contextALIAS",vars),std::string("Tester"));
    ASSERT_EQ(manager.formatHelpTopic("Index",vars),std::string("Tester/new/new/fixed/Maid"));
    ASSERT_EQ(manager.formatTemplate("{topic}",vars),std::string("speech-not-help"));
    ASSERT_EQ(manager.formatTemplate("{SeLf}",vars),std::string("Maid"));
    TestWatchdog watchdog(std::chrono::seconds(10));
    ASSERT_TRUE(manager.formatHelpTemplate("{loop}",vars).empty());
    ASSERT_TRUE(manager.formatHelpTemplate("{many}",vars).empty());
    ASSERT_TRUE(manager.formatHelpTemplate(std::string(4*1024*1024+1,'x'),vars).empty());
    ASSERT_EQ(manager.formatHelpTemplate("ready:{nick}",vars),std::string("ready:Tester"));
}

TEST(LuaPluginCompat, MixedDescriptorOrderAndReplyOverrideSurviveReload) {
    TempWorkspace workspace("dice_next_lua_mod_order_");
    const fs::path root = workspace.root() / "data" / "mod";
    for (const auto* name : {"first", "second"}) {
        fs::create_directories(root / name / "reply");
        writeText(root / name / "reply" / "rules.lua",
            "msg_reply.same = { keyword = { Match = 'choose' }, echo = '" + std::string(name) + "' }\n");
    }
    writeText(root / "first" / "descriptor.json", R"JSON({"speech":{"choice":"first"}})JSON");
    writeText(root / "second.json", R"JSON({"speech":{"choice":"second"}})JSON");
    writeText(root.parent_path() / "mod_order.json", R"JSON(["first","second"])JSON");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir(root.string()), 2);
    ASSERT_EQ(manager.mods()[0].name, std::string("first"));
    ASSERT_EQ(manager.mods()[1].name, std::string("second"));
    ASSERT_EQ(manager.dispatch("choose", "u", "g", "Tester", "", false).reply, std::string("second"));
    ASSERT_EQ(manager.formatTemplate("{choice}", {}), std::string("second"));
    ASSERT_TRUE(manager.setModEnabled("second", false));
    ASSERT_EQ(manager.reload(), 2);
    ASSERT_EQ(manager.dispatch("choose", "u", "g", "Tester", "", false).reply, std::string("first"));
    ASSERT_EQ(manager.formatTemplate("{choice}", {}), std::string("first"));
}

TEST(LuaPluginCompat, LegacyCycleEventRunsImmediatelyRepeatsAndStopsAfterReload) {
    TempWorkspace workspace("dice_next_lua_cycle_event_");
    const fs::path mod = workspace.root() / "data" / "mod" / "alarm";
    std::error_code ec;
    fs::create_directories(mod / "event", ec);
    fs::create_directories(mod / "script", ec);
    writeText(mod / "descriptor.json", R"JSON({"title":"Alarm"})JSON");
    writeText(mod / "event" / "cycle.lua", R"LUA(
event.alarm_main_cycle = {
  trigger = { cycle = { second = 60 } },
  action = { lua = "main_cycle" }
}
)LUA");
    writeText(mod / "script" / "main_cycle.lua", R"LUA(
eventMsg("tick", "g", "u")
)LUA");

    LuaPluginManager manager;
    std::vector<std::function<void()>> callbacks;
    std::vector<double> delays;
    int ticks = 0;
    manager.setScheduler([&](double seconds, std::function<void()> callback) {
        delays.push_back(seconds); callbacks.push_back(std::move(callback));
    });
    manager.setEventMsg([&](const std::string& text, const std::string&, const std::string&) {
        if (text == "tick") ++ticks;
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    ASSERT_EQ(manager.mods().front().events, 1);
    ASSERT_EQ(ticks, 1);
    ASSERT_EQ(delays.front(), 60.0);
    ASSERT_EQ(callbacks.size(), static_cast<size_t>(1));

    auto firstGeneration = callbacks.front();
    firstGeneration();
    ASSERT_EQ(ticks, 2);
    ASSERT_EQ(callbacks.size(), static_cast<size_t>(2));

    ASSERT_EQ(manager.reload(), 1);
    ASSERT_EQ(ticks, 3);
    firstGeneration();
    ASSERT_EQ(ticks, 3);
}

TEST(LuaPluginCompat, SleepTimeYieldsAndRepliesWithoutBlockingDispatch) {
    TempWorkspace workspace("dice_next_lua_sleep_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    writeText(pluginDir / "timer.lua", R"LUA(
msg_order = { [".clock"] = "clock" }
function clock(msg)
    sleepTime(250)
    return "{nick}:done"
end
)LUA");

    LuaPluginManager manager;
    std::function<void()> continuation;
    double scheduledSeconds = -1.0;
    std::string asyncReply, asyncPlatform, asyncGroup, asyncUser;
    manager.setScheduler([&](double seconds, std::function<void()> callback) {
        scheduledSeconds = seconds;
        continuation = std::move(callback);
    });
    manager.setAsyncReply([&](const std::string& platform, const std::string& group,
                              const std::string& user, const std::string& text) {
        asyncPlatform = platform; asyncGroup = group; asyncUser = user; asyncReply = text;
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    manager.setGroupGate([](const std::string&, const std::string& group,
                            const std::string&) { return group != "blocked"; });
    ASSERT_TRUE(manager.hasCommandTrigger(
        ".clock", "u1", "g1", "Tester", "", false, 0, "onebot_v11"));
    ASSERT_FALSE(manager.hasCommandTrigger(
        ".clock 1", "u1", "g1", "Tester", "", false, 0, "onebot_v11"));
    ASSERT_FALSE(manager.hasCommandTrigger(
        ".clock", "u1", "blocked", "Tester", "", false, 0, "onebot_v11"));
    manager.setGroupGate([](const std::string&, const std::string& group,
                            const std::string&, const std::string& adapterId) {
        return group != "blocked" && adapterId != "disabled-account";
    });
    ASSERT_FALSE(manager.hasCommandTrigger(
        ".clock", "u1", "g1", "Tester", "", false, 0, "onebot_v11", "disabled-account"));
    ASSERT_FALSE(manager.dispatch(
        ".clock 1", "u1", "g1", "Tester", "", false, 0, "onebot_v11", "disabled-account").matched);
    ASSERT_FALSE(static_cast<bool>(continuation));
    const auto result = manager.dispatch(
        ".clock 1", "u1", "g1", "Tester", "", false, 0, "onebot_v11");
    ASSERT_TRUE(result.matched);
    ASSERT_TRUE(result.reply.empty());
    ASSERT_TRUE(static_cast<bool>(continuation));
    ASSERT_TRUE(scheduledSeconds >= 0.249 && scheduledSeconds <= 0.251);
    continuation();
    ASSERT_EQ(asyncReply, std::string("Tester:done"));
    ASSERT_EQ(asyncPlatform, std::string("onebot_v11"));
    ASSERT_EQ(asyncGroup, std::string("g1"));
    ASSERT_EQ(asyncUser, std::string("u1"));
}

TEST(LuaPluginCompat, MalformedLegacyTextCannotEscapeAsInvalidUtf8OrThrowAcrossHostBoundary) {
    TempWorkspace workspace("dice_next_lua_encoding_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    std::string source = "msg_order = { [\"bad\"] = \"bad\" }\nfunction bad(msg) sendMsg(\"";
    source.push_back(static_cast<char>(0xBA));
    source.push_back(static_cast<char>(0xC3));  // GBK: 好
    source += "\", msg.fromGroup, msg.fromQQ); return \"";
    source.push_back(static_cast<char>(0xBA));
    source.push_back(static_cast<char>(0xC3));
    source += "\" end\n";
    writeText(pluginDir / "gbk.lua", source);

    LuaPluginManager manager;
    manager.setSender([](const std::string&, const std::string&, const std::string&) {
        throw std::runtime_error("adapter test failure");
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto result = manager.dispatch("bad", "u", "g", "n", "", false, 0, "onebot");
    ASSERT_TRUE(result.matched);
    ASSERT_TRUE(!result.reply.empty());
    const std::string encoded = nlohmann::json(result.reply).dump();
    ASSERT_TRUE(!encoded.empty());
#ifdef _WIN32
    ASSERT_EQ(result.reply, std::string("好"));
#endif
}
TEST(LuaPluginCompat, MixedUtf8AndCp936ReplyPreservesBothParts) {
    TempWorkspace workspace("dice_next_lua_mixed_encoding_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    writeText(pluginDir / "mixed.lua", R"LUA(
msg_order = { mixed = "mixed" }
function mixed(msg)
    return "前缀:" .. string.char(0xBA, 0xC3)
end
)LUA");

    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto result = manager.dispatch("mixed", "u", "g", "n", "", false, 0, "onebot");
    ASSERT_TRUE(result.matched);
#ifdef _WIN32
    ASSERT_EQ(result.reply, std::string("前缀:好"));
#else
    ASSERT_EQ(result.reply, std::string("前缀:��"));
#endif
    ASSERT_TRUE(!nlohmann::json(result.reply).dump().empty());
}
TEST(LuaPluginCompat, InvalidLegacyAuditPathIsRemappedWithoutCrashing) {
    TempWorkspace workspace("dice_next_lua_resource_path_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    std::error_code ec;
    fs::create_directories(pluginDir / "QQBot" / "feat", ec);
    ASSERT_FALSE(static_cast<bool>(ec));
    writeText(pluginDir / "QQBot" / "feat" / "resource.txt", "detail-ok\n");
    writeText(pluginDir / "resource.lua", R"LUA(
msg_order = { [".detail"] = "detail" }
function detail(msg)
    -- The invalid CP936 prefix reproduces the path that used to make the JSON
    -- audit writer throw through the Lua C boundary. The plugin/ tail is the
    -- stale absolute-cache layout used by ResourceSearchEngine.lua.
    local stale = string.char(0xBA, 0xC3) .. "D:/old/Dice/plugin/QQBot/feat/resource.txt"
    local file = io.open(stale, "r")
    if file == nil then return "missing" end
    local value = file:read("*l")
    file:close()
    return value
end
)LUA");

    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto result = manager.dispatch(
        ".detail", "u", "g", "n", "", false, 0, "onebot");
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, std::string("detail-ok"));

    std::ifstream audit(workspace.root() / "data" / "audit" / "lua_audit.jsonl");
    std::string line;
    int records = 0;
    while (std::getline(audit, line)) {
        ASSERT_TRUE(nlohmann::json::parse(line).is_object());
        ++records;
    }
    ASSERT_TRUE(records >= 2);
}

TEST(LuaPluginCompat, LegacyCp936PluginFilenameOpensUnderUtf8Locale) {
#ifndef _WIN32
    return;  // CP936 and the Windows CRT UTF-8 locale are Windows compatibility behavior.
#else
    TempWorkspace workspace("dice_next_lua_cp936_path_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    std::error_code ec;
    fs::create_directories(pluginDir / "QQBot" / "spell", ec);
    ASSERT_FALSE(static_cast<bool>(ec));
    writeText(pluginDir / "QQBot" / "spell" / utf8Path(u8"万法大全.txt"),
              "cp936-path-ok\n");
    writeText(pluginDir / "outside.txt", "must-not-open\n");
    writeText(pluginDir / "cp936_path.lua", R"LUA(
msg_order = { [".spell"] = "spell", [".escape"] = "escape" }
function spell(msg)
    -- ResourceSearchEngine reads these bytes from its old CP936 QQBot/index.
    local filename = string.char(0xCD, 0xF2, 0xB7, 0xA8, 0xB4, 0xF3, 0xC8, 0xAB)
    local stale = "D:/old/Dice/plugin/QQBot/spell/" .. filename .. ".txt"
    local file = io.open(stale, "r")
    if file == nil then return "missing" end
    local value = file:read("*l")
    file:close()
    return value
end
function escape(msg)
    local file = io.open("D:/old/Dice/plugin/QQBot/../outside.txt", "r")
    if file == nil then return "blocked" end
    file:close()
    return "opened"
end
)LUA");

    ScopedCLocale utf8Locale(".utf8");
    ASSERT_TRUE(utf8Locale.active());

    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto result = manager.dispatch(
        ".spell", "u", "g", "n", "", false, 0, "onebot");
    ASSERT_TRUE(result.matched);
    ASSERT_EQ(result.reply, std::string("cp936-path-ok"));
    const auto escape = manager.dispatch(
        ".escape", "u", "g", "n", "", false, 0, "onebot");
    ASSERT_TRUE(escape.matched);
    ASSERT_EQ(escape.reply, std::string("blocked"));
#endif
}

TEST(LuaPluginCompat, FullTomlMultilineQuotedTablesInlineDeckAndEmptyReplies) {
    TempWorkspace workspace("dice_mod_full_toml_");
    const auto mod = workspace.root() / "data/mod/full";
    fs::create_directories(mod / "reply/nested");
    writeText(mod / "reply/nested/rules.toml", R"TOML(
[reply."quoted.entry"]
keyword = { Match = ["multi", "also"] }
echo = '''first #, literal
second {nick} {sample:yes|{sample:yes|yes}}'''
[reply.empty]
keyword.Match = "empty"
echo = ""
[reply.deck]
keyword.Match = "deck"
echo = ["::0::never", "::3::picked {nick} {sample:ok|ok}"]
[reply.inline]
keyword.Match = "inline"
echo.lua = "return 'inline-result'"
)TOML");
    LuaPluginManager manager;
    ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    auto run = [&](const std::string& text) { return manager.dispatch(text, "u", "g", "Tester", "", false); };
    ASSERT_EQ(run("multi").reply, std::string("first #, literal\nsecond Tester yes"));
    ASSERT_EQ(run("MULTI").reply, run("multi").reply);
    ASSERT_EQ(run("also").reply, run("multi").reply);
    ASSERT_TRUE(run("empty").matched); ASSERT_TRUE(run("empty").reply.empty());
    for (int n = 0; n < 20; ++n) ASSERT_EQ(run("deck").reply, std::string("picked Tester ok"));
    ASSERT_EQ(run("inline").reply, std::string("inline-result"));
    ASSERT_TRUE(manager.mods()[0].compatibilityWarnings.empty());
}

TEST(LuaPluginCompat, LimitsKeepTypedValuesIdsSelfVariablesAndDicePolicies) {
    TempWorkspace workspace("dice_mod_typed_limits_");
    const auto mod = workspace.root() / "data/mod/limits";
    fs::create_directories(mod / "reply"); fs::create_directories("data/self_data");
    writeText(mod / "reply/rules.lua", R"LUA(
msg_reply.typed = { keyword={Match="typed"}, limit={user_var={text="false", zero=true, score={more=1,at_most=3}}}, echo="typed-ok" }
msg_reply.ids = { keyword={Match="ids"}, limit={user_id={nor={"bad"}},grp_id={"g"}}, echo="ids-ok" }
msg_reply.self = { keyword={Match="self"}, limit={self_var={health={at_least=3},["service.ready"]=true}}, echo="self-ok" }
msg_reply.bots = { keyword={Match="bots"}, limit={dicemaid="only"}, echo="bot-ok" }
msg_reply.people = { keyword={Match="people"}, limit={dicemaid="off"}, echo="human-ok" }
msg_reply.unknown = { keyword={Match="unknown"}, limit={user_var={score={magical=1}}}, echo="never" }
msg_reply.raw = { keyword={Match="raw"},limit="prob:100;user_id:!bad;grp_id:g;user_var:score=2+&text=false;lock:shared@user",echo="raw-ok" }
)LUA");
    writeText("data/self_data/service.json", R"({"ready":true})");
    LuaPluginManager manager; manager.setBotId("self");
    manager.setDiceLookup([](const auto& uid, const auto&, const auto&) { return uid == "bot"; });
    ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    ASSERT_TRUE(manager.eval("assert(require('Set')==Set and require('Actor')==Actor and require('GameTable')==GameTable and require('SelfData')==getSelfData)"));
    manager.confSet("u:u", "text", legacy_conf::encode("false"));
    manager.confSet("u:u", "zero", "0"); manager.confSet("u:u", "score", "2");
    manager.confSet("u:self", "health", "3");
    auto run = [&](const std::string& text, const std::string& uid = "u", const std::string& gid = "g") {
        return manager.dispatch(text, uid, gid, "Tester", "", gid.empty(), 0, "onebot", "adapter");
    };
    ASSERT_EQ(run("typed").reply, std::string("typed-ok"));
    ASSERT_EQ(run("ids").reply, std::string("ids-ok")); ASSERT_FALSE(run("ids", "bad").matched);
    ASSERT_FALSE(run("ids", "u", "other").matched); ASSERT_FALSE(run("ids", "u", "").matched);
    ASSERT_EQ(run("self").reply, std::string("self-ok"));
    manager.confSet("u:self", "health", "2"); ASSERT_FALSE(run("self").matched);
    ASSERT_FALSE(run("bots").matched); ASSERT_TRUE(run("bots", "bot").matched);
    ASSERT_TRUE(run("people").matched); ASSERT_FALSE(run("people", "bot").matched);
    ASSERT_FALSE(run("unknown").matched);
    // String-form false means a boolean truth check, so the authored string "false" is truthy.
    ASSERT_FALSE(run("raw").matched);
    manager.confSet("u:u", "text", "false"); ASSERT_TRUE(run("raw").matched);
    ASSERT_TRUE(!manager.mods()[0].compatibilityWarnings.empty());
}

TEST(LuaPluginCompat, SharedCooldownAndDailyLimitsAreAtomicPersistentAndScoped) {
    TempWorkspace workspace("dice_mod_counters_");
    const auto mod = workspace.root() / "data/mod/counters"; fs::create_directories(mod / "reply");
    writeText(mod / "reply/rules.toml", R"TOML(
[reply.limited]
keyword.Match = "limited"
limit = "cd:10&@echo=cooling;today:2&@echo=daily"
echo = "ok"
[reply.first]
keyword.Match = "first"
limit.cd.global.shared = 20
echo = "first"
[reply.second]
keyword.Match = "second"
limit.cd = { global={shared=20}, echo="shared" }
echo = "second"
)TOML");
    std::time_t now = 1704067200;
    {
        LuaPluginManager manager; manager.setClock([&] { return now; });
        ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
        auto run = [&](const std::string& text, const std::string& user = "u", const std::string& group = "g") {
            return manager.dispatch(text, user, group, "N", "", false, 0, "p", "a");
        };
        ASSERT_EQ(run("limited").reply, std::string("ok"));
        ASSERT_EQ(run("limited", "other").reply, std::string("cooling")); // scalar CD is shared by chat
        ASSERT_EQ(run("limited", "u", "other").reply, std::string("ok"));
        now += 11; ASSERT_EQ(run("limited").reply, std::string("ok"));
        now += 11; ASSERT_EQ(run("limited").reply, std::string("daily"));
        ASSERT_EQ(run("first").reply, std::string("first"));
        ASSERT_EQ(run("second", "v", "different").reply, std::string("shared"));
    }
    LuaPluginManager reloaded; reloaded.setClock([&] { return now; });
    ASSERT_TRUE(reloaded.init()); ASSERT_EQ(reloaded.loadDir("data/mod"), 1);
    ASSERT_EQ(reloaded.dispatch("limited", "u", "g", "N", "", false, 0, "p", "a").reply, std::string("daily"));
    now += 86400;
    ASSERT_EQ(reloaded.dispatch("limited", "u", "g", "N", "", false, 0, "p", "a").reply, std::string("ok"));
}

TEST(LuaPluginCompat, LocksQueueWithoutBlockingAndSurviveCoroutineSleepOnlyUntilReload) {
    TempWorkspace workspace("dice_mod_locks_");
    const auto mod = workspace.root() / "data/mod/locks"; fs::create_directories(mod / "reply");
    writeText(mod / "reply/rules.lua", R"LUA(
msg_reply.wait = {keyword={Match="wait"},limit={lock={chat="shared"}},echo=function(m)
 setUserConf(m.uid,"started",getUserConf(m.uid,"started",0)+1)
 sleepTime(10)
 return "done:"..m.uid
end}
)LUA");
    std::vector<std::function<void()>> callbacks;
    std::vector<std::string> replies;
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.setScheduler([&](double, auto callback) { callbacks.push_back(std::move(callback)); });
    manager.setAsyncReply([&](const auto&, const auto&, const auto&, const auto& text) { replies.push_back(text); });
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    const auto first = manager.dispatch("wait", "u", "g", "N", "", false);
    ASSERT_TRUE(first.pending); ASSERT_EQ(manager.confGet("u:u", "started"), std::string("1"));
    ASSERT_TRUE(manager.dispatch("wait", "v", "g", "N", "", false).pending);
    ASSERT_FALSE(manager.confHas("u:v", "started"));
    auto fire = [&] { auto callback = std::move(callbacks.front()); callbacks.erase(callbacks.begin()); callback(); };
    fire(); ASSERT_EQ(replies.size(), size_t(1)); ASSERT_EQ(replies[0], std::string("done:u"));
    fire(); ASSERT_EQ(manager.confGet("u:v", "started"), std::string("1"));
    ASSERT_EQ(callbacks.size(), size_t(1));
    ASSERT_EQ(manager.reload(), 1); fire(); ASSERT_EQ(replies.size(), size_t(1));
}

TEST(LuaPluginCompat, NestedModsResourcesAndGlobalScriptsRespectDisabledParents) {
    TempWorkspace workspace("dice_mod_nested_");
    const auto parent = workspace.root() / "data/mod/parent";
    const auto child = parent / "child"; const auto later = workspace.root() / "data/mod/zlate";
    fs::create_directories(parent / "script/ns"); fs::create_directories(child / "reply/deep");
    fs::create_directories(later / "script/ns");
    writeText(parent / "descriptor.json", R"({"title":"Parent"})");
    writeText(child / "descriptor.json", R"({"title":"Child"})");
    writeText(child / "reply/deep/rule.lua", R"(msg_reply.child={keyword={Match="child"},echo=function()return loadLua("ns.shared")end})");
    writeText(parent / "script/ns/shared.lua", "return 'parent'");
    writeText(later / "script/ns/shared.lua", "return 'last'");
    writeText(child / "resource.lua", "error('never auto-execute a resource')");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 3);
    ASSERT_EQ(manager.dispatch("child", "u", "g", "N", "", false).reply, std::string("last"));
    ASSERT_TRUE(manager.setModEnabled("zlate", false)); ASSERT_EQ(manager.reload(), 3);
    ASSERT_EQ(manager.dispatch("child", "u", "g", "N", "", false).reply, std::string("parent"));
    ASSERT_TRUE(manager.setModEnabled("parent", false)); ASSERT_EQ(manager.reload(), 3);
    ASSERT_FALSE(manager.dispatch("child", "u", "g", "N", "", false).matched);
    ASSERT_FALSE(manager.setModEnabled("parent.child", true));
    ASSERT_FALSE(manager.deleteMod("../")); ASSERT_FALSE(manager.setModEnabled("missing", false));
    ASSERT_TRUE(manager.setModEnabled("parent", true)); ASSERT_EQ(manager.reload(), 3);
    ASSERT_TRUE(manager.dispatch("child", "u", "g", "N", "", false).matched);
    ASSERT_TRUE(manager.deleteMod("parent.child")); ASSERT_EQ(manager.reload(), 2);
    ASSERT_TRUE(fs::is_directory(parent));
    writeText("data/plugin/parent.lua", "msg_order={dup='run'};function run() return 'single' end");
    ASSERT_EQ(manager.reload(), 3);
    ASSERT_EQ(manager.dispatch("dup", "u", "g", "N", "", false).reply, std::string("single"));
    ASSERT_TRUE(manager.deleteMod("parent@plugin")); ASSERT_TRUE(fs::is_directory(parent));
    ASSERT_FALSE(fs::exists("data/plugin/parent.lua"));
}

TEST(LuaPluginCompat, TriggerPriorityIsGlobalMatchThenLongestPrefixThenSearchThenRegex) {
    TempWorkspace workspace("dice_mod_priority_");
    const auto mod = workspace.root() / "data/mod/priority"; fs::create_directories(mod / "reply");
    writeText(mod / "reply/rules.toml", R"TOML(
[reply.a]
keyword.Regex = ".*"
echo = "regex"
[reply.b]
keyword.Search = "abc"
echo = "search"
[reply.c]
keyword.Prefix = "a"
echo = "short"
[reply.d]
keyword.Prefix = ["ab", "abc"]
echo = "long:{suffix}"
[reply.e]
keyword.Match = "abcdef"
echo = "exact"
[reply.f]
keyword.Match = "duplicate"
echo = "earlier"
[reply.z]
keyword.Match = "duplicate"
echo = "later"
)TOML");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    auto run = [&](const std::string& text) { return manager.dispatch(text, "u", "g", "N", "", false).reply; };
    ASSERT_EQ(run("abcdef"), std::string("exact")); ASSERT_EQ(run("abc xyz"), std::string("long:xyz"));
    ASSERT_EQ(run("abZZ"), std::string("long:ZZ")); ASSERT_EQ(run("xabc"), std::string("search"));
    ASSERT_EQ(run("Z"), std::string("regex"));
    ASSERT_EQ(run("DUPLICATE"), std::string("later"));
}

TEST(LuaPluginCompat, RuleManualsInheritWithoutCollisionsAndTapesRemainGroupGated) {
    TempWorkspace workspace("dice_mod_rulebook_");
    const auto mod = workspace.root() / "data/mod/books";
    fs::create_directories(mod / "rulebook/nested"); fs::create_directories(mod / "script");
    writeText(mod / "rulebook/root.yaml", "rule: COC\nmanual:\n  Parent: root\n  Same: root-same\n");
    writeText(mod / "rulebook/nested/child.yaml", "rule: COC-7\nmanual:\n  Child: child\n  Same: child-same\ntape:\n  scene:\n    lua: scene\n  unsafe:\n    js: never\n");
    writeText(mod / "rulebook/other.yaml", "rule: DND\nmanual:\n  Same: dnd-same\n");
    writeText(mod / "script/scene.lua", "event.blocked = true; event.msg_reply = event:format('scene:{nick}')");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    ASSERT_EQ(manager.ruleManual("Same", "COC-7").value_or(""), std::string("child-same"));
    ASSERT_EQ(manager.ruleManual("Parent", "COC-7").value_or(""), std::string("root"));
    ASSERT_EQ(manager.ruleManual("Child", "COC").value_or(""), std::string("child"));
    ASSERT_EQ(manager.ruleManual("DND:Same", "COC").value_or(""), std::string("dnd-same"));
    ASSERT_TRUE(manager.ruleManual("Same").value_or("").find("dnd-same") != std::string::npos);
    ASSERT_EQ(manager.formatTemplate("{help:COC-7:Same}", {}), std::string("child-same"));
    nlohmann::json context = {{"uid","u"},{"gid","g"},{"nick","N"}};
    auto tape = manager.dispatchTape("COC-7", "scene", context);
    ASSERT_TRUE(tape.matched); ASSERT_EQ(tape.reply, std::string("scene:N"));
    manager.setGroupGate([](const auto&, const auto&, const auto&, const auto&) { return false; });
    context = {{"uid","u"},{"gid","g"},{"nick","N"}};
    ASSERT_FALSE(manager.dispatchTape("COC-7", "scene", context).matched);
    ASSERT_FALSE(manager.dispatchTape("COC-7", "unsafe", context).matched);
}

TEST(LuaPluginCompat, ClockStartupAndDayHooksUseServerTimeAndYieldSafely) {
    TempWorkspace workspace("dice_mod_clock_");
    struct TimezoneGuard { int previous = utils::timezoneOffsetMinutes(); ~TimezoneGuard() { utils::setTimezoneOffset(previous); } } timezone;
    utils::setTimezoneOffset(0);
    const auto mod = workspace.root() / "data/mod/clock"; fs::create_directories(mod / "event");
    writeText(mod / "event/events.lua", R"LUA(
event.start = {trigger={hook="StartUp"},action={lua=function()setUserConf("u","start",getUserConf("u","start",0)+1)end}}
event.endday = {trigger={hook="DayEnd"},action={lua=function()setUserConf("u","oldyear",event.year)end}}
event.newday = {trigger={hook="DayNew"},action={lua=function()setUserConf("u","newyear",event.year)end}}
event.clock = {trigger={clock={{hour=0,minute=0},{hour=12,minute=30}}},action={lua=function()
 setUserConf("u","clock",event.hour); sleepTime(5); setUserConf("u","resumed",true)
end}}
)LUA");
    std::time_t now = 1704067199; // 2023-12-31 23:59:59 UTC
    struct Timer { double seconds; std::function<void()> callback; };
    std::vector<Timer> timers;
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); manager.setClock([&] { return now; });
    manager.setScheduler([&](double seconds, auto callback) { timers.push_back({seconds, std::move(callback)}); });
    ASSERT_EQ(manager.loadDir("data/mod"), 1); ASSERT_EQ(manager.mods()[0].events, 4);
    manager.startEventHooks(); manager.startEventHooks();
    auto fireShortest = [&] {
        auto it = std::min_element(timers.begin(), timers.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
        auto callback = std::move(it->callback); timers.erase(it); callback();
    };
    fireShortest(); ASSERT_EQ(manager.confGet("u:u", "start"), std::string("1"));
    ++now; fireShortest();
    ASSERT_EQ(manager.confGet("u:u", "oldyear"), std::string("2023"));
    ASSERT_EQ(manager.confGet("u:u", "newyear"), std::string("2024"));
    ASSERT_EQ(manager.confGet("u:u", "clock"), std::string("0"));
    ASSERT_FALSE(manager.confHas("u:u", "resumed")); fireShortest();
    ASSERT_EQ(manager.confGet("u:u", "resumed"), std::string("true"));
    auto stale = timers.front().callback;
    ASSERT_EQ(manager.reload(), 1); const auto count = timers.size(); stale(); ASSERT_EQ(timers.size(), count);
    ASSERT_EQ(manager.confGet("u:u", "start"), std::string("1")); // reload is not a host startup
}

TEST(LuaPluginCompat, UnicodeNestedResourcesAndLuaActionFunctionsKeepTheirCallingConvention) {
    TempWorkspace workspace("dice_mod_unicode_");
    const auto mod = workspace.root() / "data/mod" / utf8Path(u8"父") / utf8Path(u8"子");
    fs::create_directories(mod / "reply"); fs::create_directories(mod / "script" / utf8Path(u8"命名"));
    writeText(mod / "reply/rule.lua", R"LUA(
msg_reply.action={keyword={mAtCh="中文"},echo={lua=function(...)
 assert(select('#',...)==0);return loadLua("命名.助手")
end}}
)LUA");
    writeText(mod / "script" / utf8Path(u8"命名") / utf8Path(u8"助手.lua"), "return 'unicode-ok'");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    ASSERT_EQ(manager.dispatch("中文", "u", "g", "N", "", false).reply, std::string("unicode-ok"));
}

TEST(LuaPluginCompat, SchedulerFailureDoesNotLeakTheLegacyLock) {
    TempWorkspace workspace("dice_mod_scheduler_failure_");
    const auto mod = workspace.root() / "data/mod/failure"; fs::create_directories(mod / "reply");
    writeText(mod / "reply/rule.lua", R"LUA(
msg_reply.wait={keyword={Match="wait"},limit={lock={chat="shared"}},echo=function()sleepTime(10);return 'never'end}
msg_reply.ok={keyword={Match="ok"},limit={lock={chat="shared"}},echo="ok"}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.setScheduler([](double, auto) { throw std::runtime_error("synthetic scheduler stopped"); });
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    const auto failed = manager.dispatch("wait", "u", "g", "N", "", false);
    ASSERT_TRUE(failed.failed); ASSERT_FALSE(failed.pending);
    ASSERT_EQ(manager.dispatch("ok", "u", "g", "N", "", false).reply, std::string("ok"));
}

TEST(LuaPluginCompat, HookFailuresAreVisibleAndCannotUndoAnotherHooksRejection) {
    TempWorkspace workspace("dice_mod_hook_failure_");
    const auto mod = workspace.root() / "data/mod/hooks"; fs::create_directories(mod / "event");
    writeText(mod / "event/events.lua", R"LUA(
event.bad={trigger={hook="GroupRequest"},action={lua=function()error("synthetic hook failure")end}}
event.first={trigger={hook="FriendRequest"},action={lua=function()event.blocked=true;event.approval=false end}}
event.last={trigger={hook="FriendRequest"},action={lua=function()event.blocked=true;event.approval=true end}}
event.log={trigger={hook="LogEnd"},action={lua=function()event.blocked=true end}}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    nlohmann::json context = nlohmann::json::object();
    ASSERT_FALSE(manager.dispatchHook("GroupRequest", context)); ASSERT_TRUE(context["@dice_event_failed"] == true);
    context = nlohmann::json::object(); ASSERT_TRUE(manager.dispatchHook("FriendRequest", context)); ASSERT_TRUE(context["approval"] == false);
    ASSERT_TRUE(manager.hasHook("LogEnd")); context = nlohmann::json::object(); ASSERT_TRUE(manager.dispatchHook("LogEnd", context));
}

TEST(LuaPluginCompat, EventAndDelayedRepliesKeepTheTriggeringAdapter) {
    TempWorkspace workspace("dice_mod_account_");
    const auto mod = workspace.root() / "data/mod/account";
    fs::create_directories(mod / "reply"); fs::create_directories(mod / "event");
    writeText(mod / "reply/rule.lua", R"LUA(msg_reply.wait={keyword={Match="wait"},echo=function(m)
 sendMsg("early",m.gid,m.uid);sleepTime(10);sendMsg("late",m.gid,m.uid);return "done"
end})LUA");
    writeText(mod / "event/rule.lua", R"LUA(event.add={trigger={hook="FriendAdd"},action={lua=function()sendMsg("event",event.gid,event.uid)end}})LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    std::vector<std::function<void()>> callbacks;
    std::vector<std::string> sent;
    manager.setScheduler([&](double, auto callback) { callbacks.push_back(std::move(callback)); });
    auto sender = [&](const auto& platform, const auto& adapter, const auto& text, const auto&, const auto&) { sent.push_back(platform + ":" + adapter + ":" + text); };
    manager.setScopedSender(sender); manager.setScopedAsyncReply(sender);
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    ASSERT_TRUE(manager.dispatch("wait", "u", "g", "N", "", false, 0, "p", "second").pending);
    ASSERT_EQ(sent[0], std::string("p:second:early")); callbacks.front()();
    ASSERT_EQ(sent[1], std::string("p:second:late")); ASSERT_EQ(sent[2], std::string("p:second:done"));
    nlohmann::json context = {{"platform","p"},{"adapterId","second"},{"uid","u"},{"gid","g"}};
    manager.dispatchHook("FriendAdd", context); ASSERT_EQ(sent[3], std::string("p:second:event"));
}

TEST(LuaPluginCompat, DelayedCallbacksAfterDestructionAreHarmless) {
    TempWorkspace workspace("dice_mod_lifetime_");
    const auto mod = workspace.root() / "data/mod/lifetime";
    fs::create_directories(mod / "event");
    writeText(mod / "event/timer.lua", "event.timer={trigger={cycle=10},action={lua=function()setUserConf('u','tick',1)end}} ");
    std::vector<std::function<void()>> callbacks;
    {
        LuaPluginManager manager; ASSERT_TRUE(manager.init());
        manager.setScheduler([&](double, auto callback) { callbacks.push_back(std::move(callback)); });
        ASSERT_EQ(manager.loadDir("data/mod"), 1); ASSERT_EQ(callbacks.size(), size_t(1));
    }
    const auto count = callbacks.size(); for (const auto& callback : callbacks) callback();
    ASSERT_EQ(callbacks.size(), count);
}

TEST(LuaPluginCompat, EventDefaultsParticipateInHostGatesAndKeepTheirAccount) {
    TempWorkspace workspace("dice_mod_event_defaults_");
    const auto mod = workspace.root() / "data/mod/defaults";
    fs::create_directories(mod / "event");
    writeText(mod / "event/events.lua", R"LUA(
event.target={uid="u",gid="g",platform="p",adapterId="account",trigger={cycle=60},
 action={lua=function()sendMsg("target",event.gid,event.uid)end}}
event.notice={uid="default-user",gid="default-group",platform="default-platform",adapterId="default-account",
 trigger={hook="FriendAdd"},action={lua=function()sendMsg("notice",event.gid,event.uid)end}}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    std::vector<std::function<void()>> callbacks;
    manager.setScheduler([&](double, auto callback) { callbacks.push_back(std::move(callback)); });
    bool enabled = false; std::vector<std::string> gates, sent;
    manager.setGroupGate([&](const auto& p, const auto& g, const auto&, const auto& a) {
        gates.push_back(p + ":" + a + ":" + g); return enabled;
    });
    manager.setScopedSender([&](const auto& p, const auto& a, const auto& text, const auto& g, const auto& u) {
        sent.push_back(p + ":" + a + ":" + g + ":" + u + ":" + text);
    });
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    ASSERT_TRUE(sent.empty()); ASSERT_EQ(gates.back(), std::string("p:account:g"));
    enabled = true; auto callback = std::move(callbacks.front()); callbacks.erase(callbacks.begin()); callback();
    ASSERT_EQ(sent.size(), size_t(1)); ASSERT_EQ(sent[0], std::string("p:account:g:u:target"));
    nlohmann::json context = {{"uid","native-user"},{"gid","native-group"},{"platform","native-p"},{"adapterId","native-a"}};
    manager.dispatchHook("FriendAdd", context);
    ASSERT_EQ(gates.back(), std::string("native-p:native-a:native-group"));
    ASSERT_EQ(sent.back(), std::string("native-p:native-a:native-group:native-user:notice"));
}

TEST(LuaPluginCompat, TextTapesCannotReuseAReplyToBypassTheGroupGate) {
    TempWorkspace workspace("dice_mod_text_tape_");
    const auto mod = workspace.root() / "data/mod/text"; fs::create_directories(mod / "rulebook");
    writeText(mod / "rulebook/book.yaml", "rule: TEST\ntape:\n  scene:\n    text: hello\n");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); ASSERT_EQ(manager.loadDir("data/mod"), 1);
    nlohmann::json context = {{"uid","u"},{"gid","g"}};
    auto result = manager.dispatchTape("TEST", "scene", context);
    ASSERT_TRUE(result.matched); ASSERT_EQ(result.reply, std::string("hello"));
    manager.setGroupGate([](const auto&, const auto&, const auto&, const auto&) { return false; });
    result = manager.dispatchTape("TEST", "scene", context);
    ASSERT_FALSE(result.matched); ASSERT_TRUE(result.reply.empty());
}

TEST(LuaPluginCompat, AccountSelfIdentityAndSelfLimitsFollowImmediateEventAndDelayedContexts) {
    TempWorkspace workspace("dice_mod_self_identity_");
    const auto mod = workspace.root() / "data/mod/identity";
    fs::create_directories(mod / "reply"); fs::create_directories(mod / "event");
    writeText(mod / "reply/identity.lua", R"LUA(
msg_reply.who={keyword={match="who"},echo=function()return getDiceQQ()end}
msg_reply.wait={keyword={match="wait"},echo=function()
 local before=getDiceQQ(); sleepTime(1); return before.."/"..getDiceQQ()
end}
msg_reply.a={keyword={match="gate"},limit={self_var={token={equal="A"}}},echo="account-A"}
msg_reply.b={keyword={match="gate"},limit={self_var={token={equal="B"}}},echo="account-B"}
msg_reply.absent={keyword={match="absent"},limit={self_var={missing=false}},echo="known-account"}
)LUA");
    writeText(mod / "event/event.lua", R"LUA(
event.identity={trigger={hook="FriendAdd"},action={lua=function()setUserConf("observer","event-self",getDiceQQ())end}}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init()); manager.setBotId("global");
    std::map<std::string, std::string> identities = {{"a","bot-a"},{"b","bot-b"}};
    manager.setSelfIdLookup([&](const std::string& p, const std::string& a) {
        return p == "p" && identities.count(a) ? identities.at(a) : std::string();
    });
    std::vector<std::function<void()>> callbacks; std::vector<std::string> replies;
    manager.setScheduler([&](double, auto callback) { callbacks.push_back(std::move(callback)); });
    manager.setScopedAsyncReply([&](const auto&, const auto& a, const auto& text, const auto&, const auto&) { replies.push_back(a + ":" + text); });
    manager.confSet("u:global", "token", "A"); manager.confSet("u:bot-a", "token", "A"); manager.confSet("u:bot-b", "token", "B");
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    auto run = [&](const std::string& text, const std::string& account) { return manager.dispatch(text,"u","g","N","",false,0,"p",account); };
    ASSERT_EQ(run("who","a").reply, std::string("bot-a")); ASSERT_EQ(run("who","b").reply, std::string("bot-b"));
    ASSERT_EQ(run("gate","a").reply, std::string("account-A")); ASSERT_EQ(run("gate","b").reply, std::string("account-B"));
    ASSERT_FALSE(run("gate","unknown").matched); ASSERT_FALSE(run("absent","unknown").matched);
    ASSERT_EQ(run("absent","a").reply, std::string("known-account"));
    ASSERT_TRUE(run("wait","a").pending); ASSERT_TRUE(run("wait","b").pending);
    ASSERT_EQ(callbacks.size(), size_t(2));
    ASSERT_EQ(manager.botId(), std::string("global"));
    while (!callbacks.empty()) { auto callback = std::move(callbacks.back()); callbacks.pop_back(); callback(); }
    ASSERT_EQ(replies[0], std::string("b:bot-b/bot-b")); ASSERT_EQ(replies[1], std::string("a:bot-a/bot-a"));
    identities["a"].clear(); ASSERT_EQ(run("who","a").reply, std::string()); ASSERT_FALSE(run("gate","a").matched);
    nlohmann::json context = {{"uid","u"},{"platform","p"},{"adapterId","b"}};
    manager.dispatchHook("FriendAdd", context);
    ASSERT_EQ(manager.confGet("u:observer","event-self"), std::string("bot-b"));
    ASSERT_EQ(manager.botId(), std::string("global"));
}

TEST(LuaPluginCompat, ThreeArgumentDeckCallsKeepTheirChatAndSourceAccount) {
    TempWorkspace workspace("dice_mod_scoped_deck_");
    const auto mod = workspace.root() / "data/mod/cards"; fs::create_directories(mod / "reply");
    writeText(mod / "reply/card.lua", R"LUA(
msg_reply.card={keyword={match="cards"},echo=function(msg)
 return drawDeck(msg.gid,msg.uid,"X").."|"..drawDeck("X").."|"..drawDeck(0,22,"X")
end}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.setScopedDeckDraw([](const auto& p, const auto& a, const auto& g, const auto& u, const auto& name) {
        return p+":"+a+":"+g+":"+u+":"+name;
    });
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    ASSERT_EQ(manager.dispatch("cards","11","33","N","",false,0,"p","a").reply,
        std::string("p:a:33:11:X|p:a:::X|p:a::22:X"));
    ASSERT_EQ(manager.dispatch("cards","11","","N","",true,0,"p","b").reply,
        std::string("p:b::11:X|p:b:::X|p:b::22:X"));
}

TEST(LuaPluginCompat, LogEndOldFilenameAndContextFieldsReachLua) {
    TempWorkspace workspace("dice_mod_log_fields_");
    const auto mod = workspace.root() / "data/mod/log"; fs::create_directories(mod / "event");
    writeText(mod / "event/log.lua", R"LUA(
event.log={trigger={hook="LogEnd"},action={lua=function()
 assert(event.log_file=="export.txt");assert(event.log_name=="Name");assert(event.log_count==3)
 assert(event.fromUser=="u");assert(event.fromGroup=="g");assert(getDiceQQ()=="bot-b")
 event.blocked=true;setUserConf("u","last-log",event.log_file)
end}}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.setSelfIdLookup([](const auto&, const auto& a) { return a == "b" ? std::string("bot-b") : std::string(); });
    ASSERT_EQ(manager.loadDir("data/mod"), 1);
    Message source; source.senderId="u"; source.targetId="g"; source.platform="p"; source.adapterId="b";
    auto context=legacy_log::context(source,1,"Name",fs::path("data/logs/export.txt"),3,123);
    ASSERT_TRUE(manager.dispatchHook("LogEnd", context));
    ASSERT_EQ(manager.confGet("u:u","last-log"), std::string("export.txt"));
}

TEST(LuaPluginCompat, SessionDeckStorageKeepsSpentCardsAcrossReloadAndDatabaseReopen) {
    TempWorkspace workspace("dice_mod_deck_persistence_");
    {
        LuaPluginManager manager; ASSERT_TRUE(manager.init());
        auto decks=nlohmann::json::parse(R"({"X":{"meta":["spent","left"],"idxs":[1,0],"size":1}})");
        ASSERT_TRUE(manager.confSetBatch({{"game:2000","__session","shared"},{"game:session:shared","__decks",decks.dump()}}));
        std::mt19937_64 rng(1); ASSERT_EQ(session_deck::draw(decks["X"],rng).text,std::string("left"));
        ASSERT_TRUE(manager.confSetBatch({{"game:session:shared","__decks",decks.dump()}}));
        manager.reload();
        ASSERT_EQ(nlohmann::json::parse(manager.confGet("game:session:shared","__decks"))["X"]["size"],0);
    }
    LuaPluginManager reopened; ASSERT_TRUE(reopened.init());
    auto decks=nlohmann::json::parse(reopened.confGet("game:session:shared","__decks"));
    ASSERT_EQ(decks["X"]["size"],0); ASSERT_EQ(reopened.confGet("game:2000","__session"),std::string("shared"));
    std::mt19937_64 rng(1); ASSERT_TRUE(session_deck::draw(decks["X"],rng).empty);
}

TEST(LuaPluginCompat, ScopedDeckHostExceptionsCannotEscapeTheLuaBoundary) {
    TempWorkspace workspace("dice_mod_deck_host_failure_");
    const auto mod=workspace.root()/"data/mod/deck"; fs::create_directories(mod/"reply");
    writeText(mod/"reply/cards.lua",R"LUA(
msg_reply.card={keyword={match="card"},echo=function()
 local card,error=drawDeck(22,11,"X"); assert(card==nil);assert(error~=nil);return "caught"
end}
)LUA");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    manager.setScopedDeckDraw([](const auto&,const auto&,const auto&,const auto&,const auto&) -> std::string {
        throw std::runtime_error("synthetic deck failure");
    });
    ASSERT_EQ(manager.loadDir("data/mod"),1);
    ASSERT_EQ(manager.dispatch("card","11","22","N","",false,0,"p","a").reply,std::string("caught"));
    ASSERT_TRUE(manager.eval("assert(type(getDiceQQ())=='string')"));
}

TEST(LuaPluginCompat, AuditedCorpusFixtureRestrictsHostAccess) {
    TempWorkspace workspace("dice_corpus_guard_");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    std::string error;
    ASSERT_TRUE(test::constrainAuditedCorpus(manager, workspace.root(), &error));
    ASSERT_TRUE(manager.eval(R"LUA(
      assert(not pcall(os.execute, 'echo forbidden'))
      assert(not pcall(io.popen, 'echo forbidden'))
      assert(not pcall(require, 'socket'))
      assert(not pcall(package.loadlib, 'outside.dll', '*'))
      assert(debug.getregistry == nil and debug.getupvalue == nil)
      assert(io.open('../outside.txt', 'w') == nil)
      assert(io.open('C:/Windows/win.ini', 'r') == nil)
      assert(io.open('//server/share/file', 'r') == nil)
      assert(io.open('data/file.txt:stream', 'w') == nil)
      assert(io.open('data/NUL', 'w') == nil)
      assert(os.remove('../outside.txt') == nil)
      assert(os.rename('data/a', '../outside.txt') == nil)
      assert(not mkDirs('../outside-dir'))
      local file = assert(io.open('./data/allowed.txt', 'w'))
      file:write('fixture'); file:close()
      file = assert(io.open('data/allowed.txt', 'r'))
      assert(file:read('*a') == 'fixture'); file:close()
      assert(require('json').decode('{"n":7}').n == 7)
      assert(assert(load('return 7'))() == 7)
      assert(load(string.dump(function() return 7 end)) == nil)
    )LUA", &error));
}

TEST(LuaPluginCompat, ExternalLegacyResourceCorpusWhenProvided) {
    const char* entryValue = std::getenv("DICENEXT_LEGACY_RESOURCE_PLUGIN");
    if (!entryValue || !*entryValue) return;  // CI uses the self-contained fixture above.

    const fs::path sourceEntry(entryValue);
    const fs::path sourcePluginRoot = sourceEntry.parent_path();
    const fs::path sourceData = sourcePluginRoot / "QQBot";
    ASSERT_TRUE(fs::is_regular_file(sourceEntry));
    ASSERT_TRUE(fs::is_directory(sourceData));

    TempWorkspace workspace("dice_next_lua_resource_corpus_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    test::copyAuditedCorpus(sourceEntry, pluginDir / "ResourceSearchEngine.lua");
    test::copyAuditedCorpus(sourceData, pluginDir / "QQBot");

#ifdef _WIN32
    // main.cpp runs the CRT in UTF-8 mode. Switch only after std::filesystem has
    // copied the externally supplied Chinese path; the behavior under test is
    // Lua io.open on CP936 filenames, not narrow source-path construction.
    ScopedCLocale utf8Locale(".utf8");
    ASSERT_TRUE(utf8Locale.active());
#endif

    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    std::string error;
    ASSERT_TRUE(test::constrainAuditedCorpus(manager, workspace.root(), &error));
    TestWatchdog watchdog(std::chrono::seconds(90));
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto result = manager.dispatch(
        ".专长 星质形 ASTRAL_AQUAN", "10001", "20002", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(result.matched);
    ASSERT_TRUE(!result.reply.empty());
    ASSERT_TRUE(result.reply.find("星质形态") != std::string::npos);

    const auto reload = manager.dispatch(
        ".法术 reload", "10001", "20002", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(reload.matched);
    ASSERT_TRUE(reload.reply.find("重载缓存数据") != std::string::npos);

    const auto featReload = manager.dispatch(
        ".专长 reload", "10001", "20002", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(featReload.matched);
    ASSERT_TRUE(featReload.reply.find("重载缓存数据") != std::string::npos);

    const auto spell = manager.dispatch(
        ".法术 武器吸收 ABSORB_WEAPON", "10001", "20002", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(spell.matched);
    ASSERT_TRUE(!spell.reply.empty());
    ASSERT_TRUE(spell.reply.find("武器吸收") != std::string::npos);
}

TEST(LuaPluginCompat, ExternalLegacyFortuneCorpusWhenProvided) {
    const char* corpusValue = std::getenv("DICENEXT_LEGACY_FORTUNE_PLUGIN");
    if (!corpusValue || !*corpusValue) return;  // CI uses the self-contained fixture above.

    const fs::path sourceDir(corpusValue);
    const fs::path sourcePluginRoot = sourceDir.parent_path();
    const fs::path sourceEntry = sourcePluginRoot / utf8Path(u8"求签3.0.lua");
    const fs::path sourceNews = sourcePluginRoot / "DailyNews-fixed.lua";
    ASSERT_TRUE(fs::is_directory(sourceDir));
    ASSERT_TRUE(fs::is_regular_file(sourceEntry));
    ASSERT_TRUE(fs::is_regular_file(sourceNews));

    TempWorkspace workspace("dice_next_lua_corpus_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    test::copyAuditedCorpus(sourceDir, pluginDir / utf8Path(u8"求签"));
    test::copyAuditedCorpus(sourceEntry, pluginDir / utf8Path(u8"求签3.0.lua"));
    test::copyAuditedCorpus(sourceNews, pluginDir / "DailyNews-fixed.lua");

    LuaPluginManager manager;
    std::vector<std::string> scheduledMessages;
    manager.setBotId("dice-bot");
    manager.setEventMsg([&](const std::string& text, const std::string&,
                            const std::string&) { scheduledMessages.push_back(text); });
    int dailyFetchCalls = 0;
    manager.setHttpFetch([&](const std::string&, const std::string&,
                             const std::string&, const std::string&, int& status) {
        status = 200;
        return std::string("{\"data\":{\"image\":\"https://img.example/actual-daily-") +
            std::to_string(++dailyFetchCalls) + ".png\"}}";
    });
    ASSERT_TRUE(manager.init());
    std::string error;
    ASSERT_TRUE(test::constrainAuditedCorpus(manager, workspace.root(), &error));
    TestWatchdog watchdog(std::chrono::seconds(30));
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 2);
    ASSERT_EQ(dailyFetchCalls, 1);

    const std::string commands[] = {
        "求签", "求签 月老", "求签 浅草寺", "求签 原神",
        "求签 关帝", "求签 吕祖", "求签 先天",
    };
    for (const auto& command : commands) {
        const auto result = manager.dispatch(
            command, "10001", "20002", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(result.matched);
        ASSERT_TRUE(!result.reply.empty());
        ASSERT_TRUE(result.reply.find("找不到") == std::string::npos);
    }

    const auto news = manager.dispatch(
        "60s", "10001", "20002", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(news.matched);
    ASSERT_TRUE(news.reply.find("https://img.example/actual-daily-2.png") != std::string::npos);
    ASSERT_EQ(dailyFetchCalls, 2);
    ASSERT_TRUE(manager.hasTask("news"));
    std::string taskError;
    ASSERT_TRUE(manager.runTask("news", &taskError));
    ASSERT_EQ(scheduledMessages.size(), static_cast<size_t>(2));
    ASSERT_TRUE(scheduledMessages[1].find(
        "https://img.example/actual-daily-3.png") != std::string::npos);
    ASSERT_EQ(dailyFetchCalls, 3);
}

TEST(LuaPluginCompat, ExternalOfficialInventoryCorpusWhenProvided) {
    const char* root = std::getenv("DICENEXT_LEGACY_OFFICIAL_MOD_ROOT");
    if (!root || !*root) return;
    TempWorkspace workspace("dice_inventory_corpus_");
    test::copyAuditedCorpus(fs::path(root) / "PC_Inventory", workspace.root() / "data/mod/PC_Inventory");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    std::string error;
    ASSERT_TRUE(test::constrainAuditedCorpus(manager, workspace.root(), &error));
    std::time_t now = 200000;
    manager.setClock([&] { return now; });
    ASSERT_EQ(manager.loadDir("data/mod"),1);
    auto run = [&](const std::string& command) {
        now += 11; // Respect, rather than remove, the original per-user cooldown.
        auto result = manager.dispatch(command,"10001","20002","Tester","",false,0,"onebot","account-a");
        if (!result.matched || result.reply.empty()) throw std::runtime_error("empty corpus reply: " + command);
        return result.reply;
    };
    ASSERT_TRUE(run("放入背包 绳索").find("绳索") != std::string::npos);
    ASSERT_TRUE(run("查看背包").find("绳索") != std::string::npos);
    ASSERT_TRUE(run("放入背包 火把").find("火把") != std::string::npos);
    ASSERT_TRUE(run("移出背包 绳索").find("移出") != std::string::npos);
    const auto rest = run("查看背包");
    ASSERT_TRUE(rest.find("火把") != std::string::npos);
    ASSERT_TRUE(rest.find("绳索") == std::string::npos);
    ASSERT_TRUE(manager.eval("local inv=getPlayerCardAttr('10001','20002','_Inventory'); assert(#inv==1 and inv[1].name=='火把')", &error));
}

TEST(LuaPluginCompat, ExternalOfficialSlotsCorpusWhenProvided) {
    const char* root = std::getenv("DICENEXT_LEGACY_OFFICIAL_MOD_ROOT");
    if (!root || !*root) return;
    TempWorkspace workspace("dice_slots_corpus_");
    test::copyAuditedCorpus(fs::path(root) / "PC_Slots", workspace.root() / "data/mod/PC_Slots");
    LuaPluginManager manager; ASSERT_TRUE(manager.init());
    std::string error;
    ASSERT_TRUE(test::constrainAuditedCorpus(manager, workspace.root(), &error));
    std::time_t now = 200000;
    manager.setClock([&] { return now; });
    ASSERT_EQ(manager.loadDir("data/mod"),1);
    auto run = [&](const std::string& command) {
        now += 11;
        auto result = manager.dispatch(command,"10001","20002","Tester","",false,0,"onebot","account-a");
        if (!result.matched || result.reply.empty()) throw std::runtime_error("empty corpus reply: " + command);
        return result.reply;
    };
    ASSERT_TRUE(run("物品+绳索*2+火把*3").find("0->2") != std::string::npos);
    ASSERT_TRUE(run("物品-绳索").find("2->1") != std::string::npos);
    ASSERT_TRUE(run("查看物品").find("火把*3") != std::string::npos);
    ASSERT_TRUE(run("状态+中毒^2").find("中毒^2") != std::string::npos);
    ASSERT_TRUE(run("状态-中毒").find("中毒") != std::string::npos);
    ASSERT_TRUE(run("更新状态 中毒=轻度中毒").find("轻度中毒") != std::string::npos);
    ASSERT_TRUE(run("查看状态").find("轻度中毒") != std::string::npos);
    ASSERT_TRUE(run("法术+光亮术+治疗术").find("治疗术") != std::string::npos);
    run("法术-光亮术");
    const auto spells = run("查看法术");
    ASSERT_TRUE(spells.find("治疗术") != std::string::npos);
    ASSERT_TRUE(spells.find("光亮术") == std::string::npos);
    ASSERT_TRUE(run("专长+警觉+灵巧").find("灵巧") != std::string::npos);
    run("专长-警觉");
    ASSERT_TRUE(run("查看专长").find("灵巧") != std::string::npos);
    ASSERT_TRUE(run("新建笔记 调查=抵达钟楼").find("抵达钟楼") != std::string::npos);
    ASSERT_TRUE(run("更新笔记 调查=发现密室").find("发现密室") != std::string::npos);
    const auto journal = run("查看笔记 调查");
    ASSERT_TRUE(journal.find("抵达钟楼") != std::string::npos);
    ASSERT_TRUE(journal.find("发现密室") != std::string::npos);
    ASSERT_TRUE(run("查看笔记").find("发现密室") != std::string::npos); // Nested loadLua.
    run("删除笔记 调查");
    ASSERT_TRUE(run("查看笔记").find("空空") != std::string::npos);
    run("清空物品"); run("清空状态"); run("清空法术"); run("清空专长");
    ASSERT_TRUE(manager.eval(R"LUA(
      for _, name in ipairs({'_Inventory','_Status','_Spells','_Feats','_Journal'}) do
        assert(next(getPlayerCardAttr('10001','20002',name)) == nil, name)
      end
    )LUA", &error));
}
