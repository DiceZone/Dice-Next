#include "test_framework.h"
#include "../src/service/knowledge_base.h"

#include <chrono>
#include <filesystem>
#include <map>

using namespace dice;

namespace {
kb::HelpSource fakeHelp(const std::map<std::string, std::string>& entries, int* visits = nullptr) {
    kb::HelpSource hs;
    hs.visit = [&entries, visits](const std::function<bool(const std::string&, const std::string&, const std::string&)>& fn) {
        if (visits) ++*visits;
        for (auto& [k, v] : entries) if (!fn(k, v, "builtin")) break;
    };
    hs.fetch = [&entries](const std::string& key) { auto it = entries.find(key); return it == entries.end() ? std::string() : it->second; };
    return hs;
}
std::filesystem::path tempDir(const char* tag) {
    return std::filesystem::temp_directory_path() / (std::string("dnx-kb-") + tag + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}
struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } };
}  // namespace

TEST(KnowledgeBase, TermsCoverCjkBigramsAndAsciiWords) {
    std::vector<uint64_t> t;
    kb::forEachTerm("暗骰 .rh D100", [&](uint64_t x) { t.push_back(x); });
    ASSERT_EQ(t.size(), (size_t)3);   // 暗骰 / rh / d100
    std::vector<uint64_t> u;
    kb::forEachTerm("RH", [&](uint64_t x) { u.push_back(x); });
    ASSERT_EQ(u[0], t[1]);            // 不分大小写
    std::vector<uint64_t> single;
    kb::forEachTerm("骰", [&](uint64_t x) { single.push_back(x); });
    ASSERT_EQ(single.size(), (size_t)1);   // 单字取单字
}

TEST(KnowledgeBase, HelpEntriesAreRankedWithoutCopyingAndFetchedByName) {
    std::map<std::string, std::string> help = {
        {"rh", "暗骰：.rh 表达式，结果私聊发给你，群里只提示有人暗骰。"},
        {"jrrp", "今日人品：.jrrp 查看今天的人品值。"},
        {"st", "录入人物卡属性：.st 力量60 敏捷50"}};
    int visits = 0;
    auto hs = fakeHelp(help, &visits);
    kb::Options opt; opt.useDocs = false; opt.useCustom = false; opt.minScore = 0.3;
    auto r = kb::KnowledgeBase::instance().search("怎么暗骰？", opt, &hs);
    ASSERT_EQ(visits, 1);
    ASSERT_TRUE(!r.hits.empty());
    ASSERT_EQ(r.hits[0].id, std::string("rh"));
    ASSERT_TRUE(r.hits[0].text.find(".rh") != std::string::npos);   // 正文按名取回
    ASSERT_EQ(r.scanned, (size_t)3);
}

TEST(KnowledgeBase, WordsFoundNowhereDoNotLowerRelevance) {
    std::map<std::string, std::string> help = {{"rh", "暗骰：.rh 表达式，结果私聊发给你。"}, {"jrrp", "今日人品：.jrrp"}};
    auto hs = fakeHelp(help);
    kb::Options opt; opt.useDocs = false; opt.useCustom = false; opt.minScore = 0.45;
    auto r = kb::KnowledgeBase::instance().search("希亚喵喵 怎么暗骰", opt, &hs);   // 人名在资料里不存在
    ASSERT_TRUE(!r.hits.empty());
    ASSERT_EQ(r.hits[0].id, std::string("rh"));
}

TEST(KnowledgeBase, UnrelatedQuestionsStayBelowTheThreshold) {
    std::map<std::string, std::string> help = {{"rh", "暗骰：.rh 表达式，结果私聊发给你。"}};
    auto hs = fakeHelp(help);
    kb::Options opt; opt.useDocs = false; opt.useCustom = false; opt.minScore = 0.45;
    auto r = kb::KnowledgeBase::instance().search("今天晚饭吃火锅还是烧烤", opt, &hs);
    ASSERT_TRUE(r.hits.empty());
}

TEST(KnowledgeBase, CustomEntriesPersistAndFaqAnswersDirectly) {
    auto dir = tempDir("faq");
    Cleanup c{dir};
    auto& base = kb::KnowledgeBase::instance();
    base.loadEntries(dir / "entries.json");
    kb::Entry e;
    e.title = "跑团时间";
    e.content = "本群每周六晚上八点开团，提前半小时集合。";
    e.questions = {"什么时候开团", "几点开团"};
    e.direct = true;
    auto saved = base.upsert(e);
    ASSERT_FALSE(saved.id.empty());
    kb::Entry rules;
    rules.title = "团规";
    rules.content = "禁止在团中刷屏，掷骰请用 .r，跑团期间请保持角色扮演。";
    base.upsert(rules);
    // 重新读取文件，确认已保存。
    ASSERT_EQ(base.loadEntries(dir / "entries.json"), (size_t)2);
    kb::Options opt; opt.useHelp = false; opt.useDocs = false;
    auto r = base.search("请问几点开团呀", opt, nullptr);
    ASSERT_TRUE(r.faq.has_value());
    ASSERT_EQ(r.faq->text, e.content);
    // 随口提到同一个词不算问这个问题。
    ASSERT_FALSE(base.search("我们昨天开团玩得很开心今天继续冒险", opt, nullptr).faq.has_value());
    // 关闭直答时只作为参考资料返回。
    opt.faqDirect = false;
    auto r2 = base.search("团规是什么", opt, nullptr);
    ASSERT_FALSE(r2.faq.has_value());
    ASSERT_TRUE(!r2.hits.empty());
    ASSERT_EQ(r2.hits[0].title, std::string("团规"));
    ASSERT_TRUE(base.remove(saved.id));
    ASSERT_EQ(base.entries().size(), (size_t)1);
    base.loadEntries(dir / "none.json");   // 还原为空，避免影响其它用例
}

TEST(KnowledgeBase, ContextRespectsTheCharacterBudget) {
    std::vector<kb::Hit> hits(3);
    for (auto& h : hits) { h.source = "docs"; h.title = "标题"; h.text = std::string(900, 'a'); }
    auto text = kb::KnowledgeBase::buildContext(hits, 500);
    ASSERT_TRUE(kb::charCount(text) <= 540);   // 预算 + 标题行与省略号
    ASSERT_TRUE(text.find("[1]") != std::string::npos);
}

TEST(KnowledgeBase, SettingsAreClamped) {
    auto s = kb::normalizeSettings({{"top_k", 99}, {"min_score", -1}, {"max_chars", 5}, {"faq_threshold", 2}});
    ASSERT_EQ(s["top_k"].get<int>(), 8);
    ASSERT_TRUE(s["min_score"].get<double>() > 0);
    ASSERT_EQ(s["max_chars"].get<int>(), 200);
    ASSERT_TRUE(s["faq_threshold"].get<double>() <= 1.0);
    ASSERT_TRUE(s["sources"]["help"].get<bool>());
}

TEST(KnowledgeBase, BundledDocsLoadAndAnswerUsageQuestions) {
    // 从仓库里的 server/i18n/knowledge/docs.json 读取（测试工作目录可能不同，向上找）。
    std::filesystem::path p = std::filesystem::current_path();
    std::filesystem::path docs;
    for (int i = 0; i < 6 && docs.empty(); ++i, p = p.parent_path())
        for (auto cand : {p / "i18n" / "knowledge" / "docs.json", p / "server" / "i18n" / "knowledge" / "docs.json"})
            if (std::filesystem::exists(cand)) { docs = cand; break; }
    if (docs.empty()) return;   // 打包后的测试环境没有仓库文件
    auto& base = kb::KnowledgeBase::instance();
    ASSERT_TRUE(base.loadDocs(docs) > 100);
    kb::Options opt; opt.useHelp = false; opt.useCustom = false;
    auto r = base.search("骰娘怎么设置用途分配和负载均衡", opt, nullptr);
    ASSERT_TRUE(!r.hits.empty());
    ASSERT_EQ(r.hits[0].url, std::string("/manage/ai"));
    base.loadDocs(docs.parent_path() / "missing.json");
}
