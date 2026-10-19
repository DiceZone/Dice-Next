// 聊天记录默认不写 chat.db；模拟聊天窗口改用内存缓冲。

#include "test_framework.h"
#include "../src/service/chat_persistence.h"
#include "../src/service/group_chat_buffer.h"

#include <chrono>
#include <filesystem>

using namespace dice;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct TempConfig {
    fs::path root;
    std::unique_ptr<ConfigManager> cfg;
    TempConfig() {
        root = fs::temp_directory_path() / ("dnx-chat-persist-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        cfg = std::make_unique<ConfigManager>(root.string());
        cfg->load();
    }
    ~TempConfig() { cfg.reset(); std::error_code ec; fs::remove_all(root, ec); }
};

struct FakeRouter {
    bool groupAi = true;
    bool whitelisted = true;
    bool aiEnabledForGroup(const std::string&, const std::string&, const std::string& = {}) const { return groupAi; }
    bool aiWhitelistOk(const std::string&, const std::string&, bool) const { return whitelisted; }
};

std::string firstMsgIds(const json& lines) {
    std::string out;
    for (const auto& line : lines) out += line.value("msgId", std::string()) + ",";
    return out;
}

} // namespace

TEST(ChatPersistence, OffByDefaultAndScopedOverridesApply) {
    TempConfig t;
    FakeRouter router;
    ASSERT_FALSE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "napcat", "1001"));
    t.cfg->set<json>("dice/scoped_overrides", json{{"account", {{"napcat", {{"dice", {{"chat_persist", true}}}}}}}});
    ASSERT_TRUE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "napcat", "1001"));
    ASSERT_FALSE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "other", "1001"));
    t.cfg->set<bool>("dice/chat_persist", true);
    ASSERT_TRUE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "other", "1001"));
}

TEST(ChatPersistence, GroupsUsingAiKeepHistory) {
    TempConfig t;
    FakeRouter router;
    t.cfg->set<json>("dice/ai", json{{"enabled", true}, {"chat", {{"enabled", true}}}});
    ASSERT_TRUE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "napcat", "1001"));
    ASSERT_FALSE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "napcat", "private:10001"));
    router.groupAi = false;
    ASSERT_FALSE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "napcat", "1001"));
    router.groupAi = true; router.whitelisted = false;
    ASSERT_FALSE(chat_persistence::enabledFor(*t.cfg, router, "onebot_v11", "napcat", "1001"));
}

TEST(GroupChatBuffer, HistoryIsOrderedDeduplicatedAndRecallable) {
    auto& log = GroupChatLog::instance();
    const std::string key = "test:chat-buffer-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    log.addHistory(key, "希亚", "10001", "first", false, "m1", 100);
    log.addHistory(key, "希亚", "10001", "third", false, "m3", 300);
    log.addHistory(key, "希亚", "10001", "second", false, "m2", 200);
    log.addHistory(key, "希亚", "10001", "second again", false, "m2", 200);
    auto lines = log.recent(key, 10);
    ASSERT_EQ(lines.size(), size_t(3));
    ASSERT_EQ(firstMsgIds(lines), std::string("m1,m2,m3,"));
    log.add(key, "希亚", "10001", "live", false, "m4");
    log.markRecalled(key, "m2");
    lines = log.recent(key, 10);
    ASSERT_EQ(firstMsgIds(lines), std::string("m1,m2,m3,m4,"));
    ASSERT_TRUE(lines[1].value("recalled", false));
    ASSERT_FALSE(lines[0].value("recalled", true));
}
