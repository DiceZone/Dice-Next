// 聊天记录默认不写 chat.db；模拟聊天窗口改用内存缓冲。

#include "test_framework.h"
#include "../src/service/ai_chat_history.h"
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

TEST(AiConversation, FindsOlderQuoteOnlyWithinVisibleChatHistory) {
    TempConfig config;
    Database db;
    ASSERT_TRUE(db.open((config.root / "test.db").string()));
    auto* store = db.getChatStorage();
    ASSERT_TRUE(store != nullptr);
    Message msg;
    msg.platform = "qq"; msg.adapterId = "bot"; msg.targetId = "group";
    auto add = [&](std::string id, std::string group, std::string account, int recalled) {
        ChatMsgRow row;
        row.platform = msg.platform; row.adapterId = account; row.groupId = group;
        row.msgId = id; row.sender = "甲"; row.content = id; row.recalled = recalled;
        return store->insert(row);
    };
    add("old", "group", "bot", 0);
    add("removed", "group", "bot", 1);
    add("elsewhere", "other", "bot", 0);
    add("other-bot", "group", "other", 0);
    auto end = add("latest", "group", "bot", 0);
    add("future", "group", "bot", 0);
    auto read = [&](const std::string& id, int rounds = 1) {
        return json::parse(aichat::conversationTurn(
            aichat::loadTurnHistory(*config.cfg, store, msg, rounds, end, id, false, "呢？"), "乙", "呢？", ""));
    };
    const auto turn = read("old");
    ASSERT_EQ(turn.at("当前发言引用的原消息").at("内容").get<std::string>(), std::string("old"));
    ASSERT_EQ(turn.at("此前对话（从早到晚）").at(0).at("内容").get<std::string>(), std::string("latest"));
    for (const auto* id : {"removed", "elsewhere", "other-bot", "future"})
        ASSERT_TRUE(read(id).at("当前发言引用的原消息").is_null());
    ASSERT_TRUE(read("old", 0).at("当前发言引用的原消息").is_null());
}

TEST(AiConversation, CorrectedCluesEventsAndReviewedTermsStayDistinct) {
    TempConfig config;
    Database db;
    ASSERT_TRUE(db.open((config.root / "test.db").string()));
    auto* store = db.getChatStorage();
    std::vector<ChatMsgRow> lines;
    for (auto text : {"钥匙在井里","上次讲错了，在井旁的石缝","我拿走钥匙了","我猜钥匙被换过","我们群叫守秘人锅盖"}) {
        ChatMsgRow r; r.platform="qq"; r.groupId="A"; r.userId="host"; r.sender="主持人"; r.content=text;
        r.id=store->insert(r); lines.push_back(r);
    }
    const auto items = json::array({
        {{"type","fact"},{"source",lines[0].id},{"text","钥匙在井里"}},
        {{"type","correction"},{"source",lines[1].id},{"before","钥匙在井里"},{"text","钥匙在井旁的石缝"}},
        {{"type","event"},{"source",lines[2].id},{"text","调查员拿走了钥匙"}},
        {{"type","guess"},{"source",lines[3].id},{"text","钥匙被换过"}},
        {{"type","alias"},{"source",lines[4].id},{"term","锅盖"},{"text","守秘人"}}
    });
    auto findings = aimemory::parseFindings(items,lines);
    ASSERT_EQ(findings.size(),size_t(4));
    int checks=0;
    auto embed = [](const std::vector<std::string>& input) {
        ai::EmbedResult r; r.ok = true; r.vectors.assign(input.size(), {1.f,0.f}); return r;
    };
    auto verify = [&](const ChatMsgRow& source,const AiMemoryRow& old,const aimemory::Finding& f) -> std::optional<bool> {
        ++checks; return source.id==lines[1].id && old.content==f.before && f.type=="correction";
    };
    ASSERT_TRUE(aimemory::writeFindings(*config.cfg,store,"group","qq:A",findings,embed,verify));
    ASSERT_EQ(checks,1);
    ASSERT_TRUE(aimemory::writeFindings(*config.cfg,store,"group","qq:A",findings,embed,verify));
    auto rows = store->get_all<AiMemoryRow>(sqlite_orm::order_by(&AiMemoryRow::id));
    ASSERT_EQ(rows.size(),size_t(3));
    ASSERT_EQ(rows[0].content,std::string("钥匙在井旁的石缝"));
    ASSERT_EQ(rows[0].refId,lines[1].id);
    ASSERT_EQ(aimemory::metadata(rows[0]).at("previous")[0].at("text").get<std::string>(),std::string("钥匙在井里"));
    ASSERT_EQ(aimemory::metadata(rows[1]).at("type").get<std::string>(),std::string("event"));
    ASSERT_TRUE(aimemory::glossary(store,"qq:A","锅盖在吗").empty());
    auto m=aimemory::metadata(rows[2]); m["approved"]=true; rows[2].metadata=m.dump(); store->update(rows[2]);
    ASSERT_TRUE(aimemory::glossary(store,"qq:A","锅盖在吗").find("守秘人")!=std::string::npos);
    ASSERT_TRUE(aimemory::glossary(store,"qq:B","锅盖在吗").empty());
}

TEST(AiConversation, TargetAndRecallAreValidatedIndependently) {
    auto targeted=aichat::parseFocus(R"({"target":2,"memory_query":"  钥匙原来的位置  "})",2);
    ASSERT_EQ(targeted.target,size_t(2));
    ASSERT_EQ(targeted.recall.value(),std::string("钥匙原来的位置"));
    auto room=aichat::parseFocus(R"({"target":0,"memory_query":" "})",2);
    ASSERT_EQ(room.target,size_t(0));
    ASSERT_TRUE(room.recall.value().empty());
    ASSERT_EQ(aichat::parseFocus(R"({"target":200})",2).target,size_t(1));
    ASSERT_FALSE(aichat::parseFocus("invalid",2).recall.has_value());
}

TEST(AiConversation, OpensPreReviewMemoryDatabaseWithoutLosingStoredFacts) {
    TempConfig config;
    Database db;
    const auto path = (config.root / "test.db").string();
    ASSERT_TRUE(db.open(path));
    AiMemoryRow old; old.kind="fact"; old.scope="group"; old.scopeId="qq:A";
    old.content="旧版保存的线索"; old.embedding="[1,0]"; old.hits=7;
    auto id=db.getChatStorage()->insert(old);
    db.close();
    sqlite3* raw=nullptr;
    ASSERT_EQ(sqlite3_open((config.root / "chat.db").string().c_str(), &raw), SQLITE_OK);
    auto rc=sqlite3_exec(raw,"ALTER TABLE ai_memory DROP COLUMN metadata",nullptr,nullptr,nullptr);
    sqlite3_close(raw);
    ASSERT_EQ(rc,SQLITE_OK);
    ASSERT_TRUE(db.open(path));
    auto saved=db.getChatStorage()->get<AiMemoryRow>(id);
    ASSERT_EQ(saved.content,old.content);
    ASSERT_EQ(saved.hits,7);
    ASSERT_EQ(saved.embedding,old.embedding);
    ASSERT_TRUE(aimemory::metadata(saved).empty());
}
