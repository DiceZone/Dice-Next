#pragma once
// ─── Dice!Next — AI 知识库 ───────────────────────────────
// 让骰娘回答「怎么用 / 有没有某功能 / 团规是什么」时有据可查，同时尽量不花大模型用量：
//   · 检索完全在本地完成（BM25 关键词计分），不调用模型改写问题或判断要不要查。
//   · FAQ 直答：骰主录入的问答条目勾选「直接回答」且问题足够相似时，原文回复，不调用模型。
//   · 只把高分的少量段落注入提示词，低于阈值不注入，总字数有上限。
//
// 三个来源：
//   help   帮助系统的全部词条（内置 / 规则包 / 插件 / 骰主自定义 helpdoc），检索时即时遍历计分，
//          不另存副本（帮助词条可能有数 MB，常驻索引会显著增加内存）。
//   docs   DiceNext 文档站切段（i18n/knowledge/docs.json，随安装包分发，见 tools/build-knowledge-docs.py）。
//   custom 骰主在「知识库」页录入的条目（data/knowledge/entries.json）。
//
// 配置 dice/ai.knowledge：
//   { enabled, sources{help,docs,custom}, top_k, min_score(0-1 覆盖度阈值), max_chars,
//     faq_direct, faq_threshold(0-1 问题相似度), semantic(自定义条目额外做向量检索) }

#include "../config/config_manager.h"
#include "ai_usage.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dice::kb {

using json = nlohmann::json;

// ─── 分词：中日韩文字取相邻二字（单字词取单字），英文数字取整词（小写）。
// 词项编码为 64 位整数，遍历大量帮助词条时不产生字符串分配。
namespace detail {
inline bool isCjk(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}
// 虚词与代词：分词时当作分隔符，避免「团规是什么」切出「规是」这类跨词的二字组合拉低匹配度。
// 不收「能、对、过、得、会」等，它们常出现在「技能、对抗、通过、获得」这类实词里。
inline bool isFunctionChar(uint32_t cp) {
    static const std::unordered_set<uint32_t> set = [] {
        std::unordered_set<uint32_t> out;
        const char* chars = "的了吗呢吧呀啊哦嘛呗是在和与及或都也就还把被让给从向这那个些哪谁啥怎么请我你他她它们您着之其而但若则于"
                            "嗎這個們麼請與還讓給從誰";
        std::string s(chars);
        for (size_t i = 0; i + 2 < s.size() + 1; i += 3) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0xE0 || i + 2 >= s.size()) break;
            out.insert(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F));
        }
        return out;
    }();
    return set.count(cp) > 0;
}
inline uint64_t bigram(uint32_t a, uint32_t b) { return (1ULL << 62) | ((uint64_t)a << 21) | b; }
inline uint64_t unigram(uint32_t a) { return (2ULL << 62) | a; }
inline uint64_t wordKey(uint64_t fnv) { return (3ULL << 62) | (fnv & ((1ULL << 62) - 1)); }
}  // namespace detail

// 对 text 逐个产出词项（回调收到编码后的词项）。
template <class F>
inline void forEachTerm(const std::string& text, F&& emit) {
    using namespace detail;
    uint32_t prev = 0; int run = 0;
    uint64_t fnv = 1469598103934665603ULL; int wlen = 0;
    auto endCjk = [&] { if (run == 1) emit(unigram(prev)); run = 0; prev = 0; };
    auto endWord = [&] { if (wlen >= 2) emit(wordKey(fnv)); fnv = 1469598103934665603ULL; wlen = 0; };
    size_t i = 0, n = text.size();
    while (i < n) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp; size_t step;
        if (c < 0x80) { cp = c; step = 1; }
        else if (c < 0xE0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F); step = 2; }
        else if (c < 0xF0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F); step = 3; }
        else if (i + 3 < n) { cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F); step = 4; }
        else { cp = 0; step = 1; }
        i += step;
        if (cp < 0x80 && (std::isalnum((int)cp) || cp == '_')) {
            endCjk();
            char lc = (char)std::tolower((int)cp);
            fnv = (fnv ^ (unsigned char)lc) * 1099511628211ULL; ++wlen;
            continue;
        }
        if (cp >= 0xFF10 && cp <= 0xFF5A) {   // 全角字母数字按半角处理
            uint32_t h = cp - 0xFEE0;
            if (std::isalnum((int)h)) { endCjk(); fnv = (fnv ^ (unsigned char)std::tolower((int)h)) * 1099511628211ULL; ++wlen; continue; }
        }
        endWord();
        if (isCjk(cp)) {
            if (isFunctionChar(cp)) { endCjk(); continue; }
            if (run > 0) emit(bigram(prev, cp));
            prev = cp; ++run;
            continue;
        }
        endCjk();
    }
    endCjk(); endWord();
}

// 疑问、语气类常用词：不参与检索（只会拉低结果的区分度）。
inline const std::unordered_set<uint64_t>& stopTerms() {
    static const std::unordered_set<uint64_t> s = [] {
        std::unordered_set<uint64_t> out;
        for (const char* w : {"怎么", "如何", "什么", "可以", "一下", "这个", "那个", "是不", "不是", "有没", "没有", "请问", "我想",
                              "想要", "能不", "不能", "怎样", "为什", "什么", "一个", "知道", "告诉", "我们", "你们", "他们", "就是",
                              "怎麼", "這個", "那個", "請問", "為什", "麼樣", "沒有", "有沒", "們的", "嗎？", "どう", "ですか", "ます",
                              "the", "how", "what", "can", "does", "do", "is", "are", "to", "of", "and", "in", "it", "you", "me", "please"})
            forEachTerm(w, [&](uint64_t t) { out.insert(t); });
        return out;
    }();
    return s;
}

// ─── 配置 ────────────────────────────────────────────────
struct Options {
    bool enabled = true, useHelp = true, useDocs = true, useCustom = true;
    int topK = 3;
    double minScore = 0.45;
    int maxChars = 1200;
    bool faqDirect = true;
    double faqThreshold = 0.6;
    bool semantic = false;
};
inline json defaultSettings() {
    return json{{"enabled", true}, {"sources", {{"help", true}, {"docs", true}, {"custom", true}}}, {"top_k", 3},
                {"min_score", 0.45}, {"max_chars", 1200}, {"faq_direct", true}, {"faq_threshold", 0.6}, {"semantic", false}};
}
inline json normalizeSettings(const json& in) {
    json d = defaultSettings();
    if (!in.is_object()) return d;
    auto clampD = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    auto clampI = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    d["enabled"] = in.value("enabled", true);
    json src = in.contains("sources") && in["sources"].is_object() ? in["sources"] : json::object();
    d["sources"] = {{"help", src.value("help", true)}, {"docs", src.value("docs", true)}, {"custom", src.value("custom", true)}};
    d["top_k"] = clampI(in.value("top_k", 3), 1, 8);
    d["min_score"] = clampD(in.value("min_score", 0.45), 0.05, 1.0);
    d["max_chars"] = clampI(in.value("max_chars", 1200), 200, 6000);
    d["faq_direct"] = in.value("faq_direct", true);
    d["faq_threshold"] = clampD(in.value("faq_threshold", 0.6), 0.3, 1.0);
    d["semantic"] = in.value("semantic", false);
    return d;
}
inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return normalizeSettings(a.is_object() && a.contains("knowledge") ? a["knowledge"] : json::object());
}
inline Options optionsFrom(const json& c) {
    Options o;
    o.enabled = c.value("enabled", false);
    o.useHelp = c["sources"].value("help", true);
    o.useDocs = c["sources"].value("docs", true);
    o.useCustom = c["sources"].value("custom", true);
    o.topK = c.value("top_k", 3);
    o.minScore = c.value("min_score", 0.45);
    o.maxChars = c.value("max_chars", 1200);
    o.faqDirect = c.value("faq_direct", true);
    o.faqThreshold = c.value("faq_threshold", 0.6);
    o.semantic = c.value("semantic", false);
    return o;
}
inline bool enabled(ConfigManager& cfg) { return ai::enabled(cfg) && conf(cfg).value("enabled", false); }

// ─── 数据 ────────────────────────────────────────────────
struct Entry {
    std::string id, title, content;
    std::vector<std::string> tags, questions;   // questions：FAQ 的问法（可多个）
    bool direct = false;                        // 命中问法时直接回复 content，不调用模型
    bool enabled = true;
    long long updatedAt = 0;
};
inline json entryToJson(const Entry& e) {
    return json{{"id", e.id}, {"title", e.title}, {"content", e.content}, {"tags", e.tags}, {"questions", e.questions},
                {"direct", e.direct}, {"enabled", e.enabled}, {"updated_at", e.updatedAt}};
}
inline Entry entryFromJson(const json& j) {
    Entry e;
    e.id = j.value("id", std::string());
    e.title = j.value("title", std::string());
    e.content = j.value("content", std::string());
    auto strs = [&](const char* k) {
        std::vector<std::string> v;
        if (j.contains(k) && j[k].is_array())
            for (auto& s : j[k]) if (s.is_string() && !s.get<std::string>().empty()) v.push_back(s.get<std::string>());
        return v;
    };
    e.tags = strs("tags");
    e.questions = strs("questions");
    e.direct = j.value("direct", false);
    e.enabled = j.value("enabled", true);
    e.updatedAt = j.value("updated_at", (long long)0);
    return e;
}

struct DocChunk { std::string id, title, url, section, text; };

// 一条检索结果。
struct Hit {
    std::string source;     // help | docs | custom
    std::string id;         // 帮助词条名 / 文档段 id / 自定义条目 id
    std::string title, url, text;
    double score = 0;       // BM25 分数（排序用）
    double coverage = 0;    // 0-1：问题里的关键词被覆盖的比例（按重要度加权），用于阈值
    double semantic = -1;   // 向量相似度（仅自定义条目、开启语义检索时）
};
struct SearchResult {
    std::vector<Hit> hits;          // 已按得分排序并按阈值过滤，最多 topK
    std::vector<Hit> belowThreshold;// 未达阈值的前几名（测试台展示用）
    size_t scanned = 0;             // 本次计分的条目/段落数
    long long elapsedMs = 0;
    std::optional<Hit> faq;         // FAQ 直答命中
    double faqSimilarity = 0;
};

// 帮助来源：遍历（不复制正文）+ 按名取正文。由 main.cpp 用 CommandRouter 注入。
struct HelpSource {
    std::function<void(const std::function<bool(const std::string& key, const std::string& content, const std::string& source)>&)> visit;
    std::function<std::string(const std::string& key)> fetch;
};

// 把长文按段落切成不超过 maxChars 的块。
inline std::vector<std::string> splitChunks(const std::string& text, size_t maxChars = 700) {
    std::vector<std::string> out;
    std::string cur;
    size_t pos = 0;
    auto push = [&] { if (!cur.empty()) out.push_back(cur); cur.clear(); };
    while (pos <= text.size()) {
        size_t end = text.find("\n\n", pos);
        std::string para = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!cur.empty() && cur.size() + para.size() + 2 > maxChars * 3) push();   // 按字节估算（中文约 3 字节/字）
        cur += (cur.empty() ? "" : "\n\n") + para;
        if (end == std::string::npos) break;
        pos = end + 2;
    }
    push();
    return out;
}

// 按字符（非字节）截断。
inline std::string truncateChars(const std::string& s, size_t maxChars) {
    size_t cnt = 0, i = 0;
    while (i < s.size() && cnt < maxChars) {
        unsigned char c = (unsigned char)s[i];
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        ++cnt;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "…";
}
inline size_t charCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// FAQ 问法匹配：@p query 是玩家的消息，@p question 是骰主写的问法。
// 主要看问法里的词有多少出现在消息里（玩家常会多说「请问…呀」），再按消息里多余的内容略微扣分。
inline double termSimilarity(const std::string& query, const std::string& question) {
    std::unordered_set<uint64_t> sa, sb;
    const auto& stop = stopTerms();
    forEachTerm(query, [&](uint64_t t) { if (!stop.count(t)) sa.insert(t); });
    forEachTerm(question, [&](uint64_t t) { if (!stop.count(t)) sb.insert(t); });
    if (sa.empty() || sb.empty()) return 0;
    size_t inter = 0;
    for (auto t : sb) if (sa.count(t)) ++inter;
    double contain = (double)inter / (double)sb.size();
    double precision = (double)inter / (double)sa.size();
    return 0.7 * contain + 0.3 * precision;
}

class KnowledgeBase {
public:
    static KnowledgeBase& instance() { static KnowledgeBase kb; return kb; }

    // ─── 加载 ───
    size_t loadDocs(const std::filesystem::path& file) {
        std::vector<DocChunk> v;
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            try {
                std::ifstream in(file, std::ios::binary);
                json j = json::parse(in);
                for (auto& c : j.value("chunks", json::array()))
                    v.push_back({c.value("id", std::string()), c.value("title", std::string()), c.value("url", std::string()),
                                 c.value("section", std::string()), c.value("text", std::string())});
            } catch (...) { DICE_LOG_WARN("知识库文档「{}」读取失败，文档来源暂不可用。", file.string()); }
        }
        std::unique_lock lk(mu_);
        docs_ = std::move(v);
        return docs_.size();
    }
    size_t loadEntries(const std::filesystem::path& file) {
        std::vector<Entry> v;
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            try {
                std::ifstream in(file, std::ios::binary);
                json j = json::parse(in);
                for (auto& e : j.value("entries", json::array())) if (e.is_object()) v.push_back(entryFromJson(e));
            } catch (...) { DICE_LOG_WARN("知识库条目文件「{}」读取失败。", file.string()); }
        }
        std::unique_lock lk(mu_);
        entriesFile_ = file;
        entries_ = std::move(v);
        return entries_.size();
    }

    // ─── 自定义条目 ───
    std::vector<Entry> entries() const { std::shared_lock lk(mu_); return entries_; }
    size_t docCount() const { std::shared_lock lk(mu_); return docs_.size(); }
    // 新增或更新（id 为空则新建）。返回保存后的条目。
    Entry upsert(Entry e) {
        std::unique_lock lk(mu_);
        if (e.id.empty()) e.id = "k" + randomId();
        e.updatedAt = (long long)std::time(nullptr);
        bool found = false;
        for (auto& x : entries_) if (x.id == e.id) { x = e; found = true; break; }
        if (!found) entries_.push_back(e);
        saveLocked();
        return e;
    }
    bool remove(const std::string& id) {
        std::unique_lock lk(mu_);
        auto before = entries_.size();
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; }), entries_.end());
        if (entries_.size() == before) return false;
        saveLocked();
        return true;
    }
    // 批量导入（追加；id 冲突时生成新 id）。
    size_t importEntries(const std::vector<Entry>& add) {
        std::unique_lock lk(mu_);
        std::unordered_set<std::string> ids;
        for (auto& e : entries_) ids.insert(e.id);
        size_t n = 0;
        for (auto e : add) {
            if (e.content.empty() && e.title.empty()) continue;
            if (e.id.empty() || ids.count(e.id)) e.id = "k" + randomId();
            e.updatedAt = (long long)std::time(nullptr);
            ids.insert(e.id);
            entries_.push_back(std::move(e));
            ++n;
        }
        if (n) saveLocked();
        return n;
    }

    // ─── 检索 ───
    // @p vectorFn 可选：传入文本列表返回向量（开启语义检索时用于自定义条目）。
    SearchResult search(const std::string& query, const Options& opt, const HelpSource* help,
                        const std::function<std::vector<std::vector<float>>(const std::vector<std::string>&)>& vectorFn = nullptr) const {
        auto t0 = std::chrono::steady_clock::now();
        SearchResult res;
        // 查询词项（去停用词、去重）。
        std::vector<uint64_t> qterms;
        {
            const auto& stop = stopTerms();
            std::unordered_set<uint64_t> seen;
            forEachTerm(query, [&](uint64_t t) { if (!stop.count(t) && seen.insert(t).second) qterms.push_back(t); });
        }
        std::shared_lock lk(mu_);
        // FAQ 直答：只看勾选了「直接回答」的自定义条目。
        if (opt.useCustom && opt.faqDirect) {
            for (auto& e : entries_) {
                if (!e.enabled || !e.direct || e.content.empty()) continue;
                double best = 0;
                for (auto& q : e.questions) best = std::max(best, termSimilarity(query, q));
                if (!e.title.empty()) best = std::max(best, termSimilarity(query, e.title));
                if (best > res.faqSimilarity) {
                    res.faqSimilarity = best;
                    if (best >= opt.faqThreshold) res.faq = Hit{"custom", e.id, e.title, "", e.content, 0, best, -1};
                }
            }
        }
        if (qterms.empty()) return res;
        std::unordered_map<uint64_t, size_t> qindex;
        for (size_t i = 0; i < qterms.size(); ++i) qindex[qterms[i]] = i;
        const size_t Q = qterms.size();

        struct Cand { int kind; size_t ref; std::string key; std::vector<int> tf; long long len; };
        std::vector<Cand> cands;
        std::vector<long long> df(Q, 0);
        long long totalLen = 0; size_t N = 0;
        std::vector<int> tf(Q, 0);
        auto score = [&](int kind, size_t ref, const std::string& key, const std::string& title, const std::string& body) {
            std::fill(tf.begin(), tf.end(), 0);
            long long len = 0;
            bool any = false;
            forEachTerm(body, [&](uint64_t t) {
                ++len;
                auto it = qindex.find(t);
                if (it != qindex.end()) { ++tf[it->second]; any = true; }
            });
            forEachTerm(title, [&](uint64_t t) {   // 标题 / 词条名命中权重更高
                auto it = qindex.find(t);
                if (it != qindex.end()) { tf[it->second] += 3; any = true; }
            });
            ++N; totalLen += len;
            if (!any) return;
            for (size_t i = 0; i < Q; ++i) if (tf[i]) ++df[i];
            cands.push_back({kind, ref, key, tf, len});
        };
        if (opt.useDocs)
            for (size_t i = 0; i < docs_.size(); ++i) score(1, i, "", docs_[i].title, docs_[i].text);
        // 自定义条目按段计分（ref 高 32 位 = 条目下标，低 32 位 = 段号）。
        std::vector<std::vector<std::string>> customChunks;
        if (opt.useCustom) {
            customChunks.resize(entries_.size());
            for (size_t i = 0; i < entries_.size(); ++i) {
                auto& e = entries_[i];
                if (!e.enabled) continue;
                customChunks[i] = splitChunks(e.content);
                std::string titleTerms = e.title;
                for (auto& q : e.questions) titleTerms += " " + q;
                for (auto& t : e.tags) titleTerms += " " + t;
                for (size_t c = 0; c < customChunks[i].size(); ++c) {
                    size_t before = cands.size();
                    score(2, (i << 32) | c, "", titleTerms, customChunks[i][c]);
                    // 开启语义检索时，没有关键词命中的段落也要参与向量比较。
                    if (cands.size() == before && opt.semantic && vectorFn) cands.push_back({2, (i << 32) | c, "", std::vector<int>(Q, 0), 1});
                }
            }
        }
        if (opt.useHelp && help && help->visit)
            help->visit([&](const std::string& key, const std::string& content, const std::string&) {
                score(0, 0, key, key, content);
                return true;
            });
        res.scanned = N;
        if (N == 0 || cands.empty()) { res.elapsedMs = elapsed(t0); return res; }

        const double avgLen = std::max(1.0, (double)totalLen / (double)N);
        std::vector<double> idf(Q);
        double idfSum = 0;
        // 在所有资料里都没出现过的词（人名、触发词、错字）不可能被任何资料覆盖，不计入覆盖度的分母。
        for (size_t i = 0; i < Q; ++i) { idf[i] = std::log(1.0 + ((double)N - df[i] + 0.5) / (df[i] + 0.5)); if (df[i] > 0) idfSum += idf[i]; }
        const double k1 = 1.2, b = 0.75;
        std::vector<Hit> all;
        all.reserve(cands.size());
        for (auto& c : cands) {
            double s = 0, cov = 0;
            for (size_t i = 0; i < Q; ++i) {
                if (!c.tf[i]) continue;
                double f = c.tf[i];
                s += idf[i] * f * (k1 + 1) / (f + k1 * (1 - b + b * (double)c.len / avgLen));
                cov += idf[i];
            }
            Hit h; h.score = s; h.coverage = idfSum > 0 ? cov / idfSum : 0;
            if (c.kind == 0) { h.source = "help"; h.id = c.key; h.title = c.key; }
            else if (c.kind == 1) {
                auto& d = docs_[c.ref]; h.source = "docs"; h.id = d.id; h.title = d.title; h.url = d.url; h.text = d.text;
                if (d.section == "开发") h.score *= 0.6;   // 开发者文档（API、插件开发）排在用法说明之后
            }
            else {
                size_t ei = (size_t)(c.ref >> 32), ci = (size_t)(c.ref & 0xFFFFFFFFULL);
                auto& e = entries_[ei];
                h.source = "custom"; h.id = e.id; h.title = e.title; h.text = customChunks[ei][ci];
            }
            all.push_back(std::move(h));
        }
        lk.unlock();   // 以下不再访问 docs_/entries_（结果已复制）
        // 语义检索（可选，仅自定义条目）：向量相似度足够高的条目提升覆盖度。
        if (opt.semantic && vectorFn) applySemantic(query, all, vectorFn);
        std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) {
            return a.score != b.score ? a.score > b.score : a.coverage > b.coverage;
        });
        // 同一页面 / 条目只取最高分的一段，避免结果挤在一处。
        std::unordered_set<std::string> seenDoc;
        for (auto& h : all) {
            std::string key = h.source + "|" + (h.source == "docs" ? h.url + "|" + h.title : h.id);
            if (!seenDoc.insert(key).second) continue;
            if (h.coverage >= opt.minScore && (int)res.hits.size() < opt.topK) res.hits.push_back(h);
            else if (res.belowThreshold.size() < 5 && h.coverage < opt.minScore) res.belowThreshold.push_back(h);
            if ((int)res.hits.size() >= opt.topK && res.belowThreshold.size() >= 5) break;
        }
        // 帮助词条正文按名取回（遍历时不复制）。
        auto fill = [&](std::vector<Hit>& v) {
            for (auto& h : v) if (h.source == "help" && h.text.empty() && help && help->fetch) h.text = help->fetch(h.id);
        };
        fill(res.hits);
        fill(res.belowThreshold);
        res.elapsedMs = elapsed(t0);
        return res;
    }

    // 把命中结果整理成注入提示词的参考资料，总长不超过 maxChars 字。
    static std::string buildContext(const std::vector<Hit>& hits, int maxChars) {
        std::string out;
        size_t used = 0;
        int n = 0;
        for (auto& h : hits) {
            if (used >= (size_t)maxChars) break;
            size_t budget = (size_t)maxChars - used;
            std::string head = "[" + std::to_string(++n) + "] " + (h.source == "help" ? "帮助词条「" + h.title + "」" : h.title) + "\n";
            std::string body = truncateChars(h.text, std::min<size_t>(budget, 600));
            out += head + body + "\n";
            used += charCount(head) + charCount(body);
        }
        return out;
    }

private:
    static long long elapsed(std::chrono::steady_clock::time_point t0) {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    static std::string randomId() {
        static std::mutex m; static std::mt19937_64 rng{std::random_device{}()};
        std::lock_guard<std::mutex> lk(m);
        static const char* hex = "0123456789abcdef";
        std::string s; auto v = rng();
        for (int i = 0; i < 10; ++i) { s += hex[v & 15]; v >>= 4; }
        return s;
    }
    void saveLocked() {
        if (entriesFile_.empty()) return;
        json arr = json::array();
        for (auto& e : entries_) arr.push_back(entryToJson(e));
        std::error_code ec;
        std::filesystem::create_directories(entriesFile_.parent_path(), ec);
        auto tmp = entriesFile_; tmp += ".tmp";
        { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << json{{"version", 1}, {"entries", arr}}.dump(2); }
        std::filesystem::rename(tmp, entriesFile_, ec);
        if (ec) DICE_LOG_WARN("知识库条目保存失败：{}", ec.message());
    }
    // 自定义条目的向量按内容缓存（进程内）；模型更换后由调用方清空。
    void applySemantic(const std::string& query, std::vector<Hit>& all,
                       const std::function<std::vector<std::vector<float>>(const std::vector<std::string>&)>& vectorFn) const {
        std::vector<std::string> need;
        std::vector<size_t> idx;
        {
            std::lock_guard<std::mutex> lk(vecMu_);
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].source == "custom" && !vecCache_.count(all[i].text)) { need.push_back(all[i].text); idx.push_back(i); }
        }
        need.push_back(query);
        auto vecs = vectorFn(need);
        if (vecs.size() != need.size()) return;
        const auto& qv = vecs.back();
        std::lock_guard<std::mutex> lk(vecMu_);
        if (vecCache_.size() > 5000) vecCache_.clear();
        for (size_t k = 0; k + 1 < vecs.size(); ++k) vecCache_[need[k]] = vecs[k];
        for (auto& h : all) {
            if (h.source != "custom") continue;
            auto it = vecCache_.find(h.text);
            if (it == vecCache_.end()) continue;
            h.semantic = ai::cosine(qv, it->second);
            // 相似度 0.5 以下视为无关，0.9 视为完全相关，线性映射到覆盖度。
            double norm = std::clamp((h.semantic - 0.5) / 0.4, 0.0, 1.0);
            if (norm > h.coverage) { h.coverage = norm; h.score = std::max(h.score, norm * 10); }
        }
    }

    mutable std::shared_mutex mu_;
    std::vector<DocChunk> docs_;
    std::vector<Entry> entries_;
    std::filesystem::path entriesFile_;
    mutable std::mutex vecMu_;
    mutable std::unordered_map<std::string, std::vector<float>> vecCache_;
};

}  // namespace dice::kb
