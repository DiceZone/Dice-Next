#include "test_framework.h"
#include "../src/core/mod/lua_plugin_manager.h"

#include <chrono>
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

}  // namespace

TEST(LuaPluginCompat, LegacySiblingLoadLuaAndLoadTimeHttpBothWork) {
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
    return target and target.value or "missing-modern"
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

    writeText(pluginDir / "script" / "modern.lua", "return { value = \"modern-ok\" }\n");
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
                ? std::string(R"JSON({"data":{"image":"https://img.example/daily.png"}})JSON")
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
        ASSERT_EQ(modern.reply, std::string("modern-ok"));

        const auto escape = manager.dispatch(
            "越界", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(escape.matched);
        ASSERT_EQ(escape.reply, std::string("blocked"));

        const auto news = manager.dispatch(
            "60s", "user-42", "group-1", "Tester", "", false, 0, "onebot");
        ASSERT_TRUE(news.matched);
        ASSERT_EQ(news.reply, std::string("新闻:https://img.example/daily.png"));
    }
}

TEST(LuaPluginCompat, LegacyTaskCallRegistersAndRuns) {
    TempWorkspace workspace("dice_next_lua_task_");
    const fs::path pluginDir = workspace.root() / "data" / "plugin";
    writeText(pluginDir / "task.lua", R"LUA(
task_call = { news = "run_news" }
msg_order = {}
function run_news()
    eventMsg("task fired", "g", "u")
end
)LUA");

    LuaPluginManager manager;
    std::string fired;
    manager.setEventMsg([&](const std::string& text, const std::string&, const std::string&) {
        fired = text;
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    ASSERT_TRUE(manager.hasTask("news"));
    ASSERT_EQ(manager.taskNames().size(), static_cast<size_t>(1));
    std::string error;
    ASSERT_TRUE(manager.runTask("news", &error));
    ASSERT_EQ(fired, std::string("task fired"));
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
    std::error_code ec;
    fs::copy_file(sourceEntry, pluginDir / "ResourceSearchEngine.lua",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(static_cast<bool>(ec));
    fs::copy(sourceData, pluginDir / "QQBot",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    LuaPluginManager manager;
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 1);
    const auto result = manager.dispatch(
        ".专长 星质形 ASTRAL_AQUAN", "10001", "20002", "Tester", "", false, 0, "onebot");
    ASSERT_TRUE(result.matched);
    ASSERT_TRUE(!result.reply.empty());
    ASSERT_TRUE(result.reply.find("星质形态") != std::string::npos);
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
    std::error_code ec;
    fs::copy(sourceDir, pluginDir / utf8Path(u8"求签"),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(static_cast<bool>(ec));
    fs::copy_file(sourceEntry, pluginDir / utf8Path(u8"求签3.0.lua"),
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(static_cast<bool>(ec));
    fs::copy_file(sourceNews, pluginDir / "DailyNews-fixed.lua",
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    LuaPluginManager manager;
    manager.setHttpFetch([](const std::string&, const std::string&,
                            const std::string&, const std::string&, int& status) {
        status = 200;
        return std::string(R"JSON({"data":{"image":"https://img.example/actual-daily.png"}})JSON");
    });
    ASSERT_TRUE(manager.init());
    ASSERT_EQ(manager.loadDir((workspace.root() / "data" / "mod").string()), 2);

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
    ASSERT_TRUE(news.reply.find("https://img.example/actual-daily.png") != std::string::npos);
}
