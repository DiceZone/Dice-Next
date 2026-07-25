#pragma once
// ─── Dice!Next — AI 记忆（智能化阶段 B）：群聊滚动摘要 ────────────────────
// 让骰娘在长对话里“记得”超出近期窗口的内容。近期 N 条消息仍逐条喂给对话模型
// （阶段 A），更早、已“滑出窗口”的消息按批用大模型压缩成一份【群摘要】，存进
// chat.db 的 ai_memory(kind="summary")，每群一行。对话时把摘要作为背景注入。
//
// 触发：由消息处理钩子在后台线程调用 maybeSummarize；仅当已滑出窗口且未折叠的
// 消息累积到一批（>=rounds）才真正请求模型，天然限频、控成本。失败保留旧摘要。
//
// 配置 dice/ai.memory.short：
//   { enabled, rounds(逐条保留的近期窗口/折叠批大小), summary_model_id(空=首个启用),
//     summary_prompt(空=内置默认), max_chars(摘要字数上限) }
//
// 阶段 C 将复用 ai_memory 表存持久事实（kind="fact"，带 embedding 向量检索）。

#include "../config/config_manager.h"
#include "../storage/database.h"
#include "ai_gateway.h"
#include "ai_chat.h"
#include <sqlite_orm/sqlite_orm.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>

namespace dice::aimemory {

using json = nlohmann::json;

inline json shortConf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    if (a.is_object() && a.contains("memory") && a["memory"].is_object()) {
        json m = a["memory"];
        if (m.contains("short") && m["short"].is_object()) return m["short"];
    }
    return json::object();
}
inline bool shortEnabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && shortConf(cfg).value("enabled", false);
}
inline int keepRounds(ConfigManager& cfg) { int n = shortConf(cfg).value("rounds", 20); return n < 4 ? 4 : (n > 200 ? 200 : n); }
inline int summaryMaxChars(ConfigManager& cfg) { int n = shortConf(cfg).value("max_chars", 400); return n < 80 ? 80 : (n > 2000 ? 2000 : n); }

// ─── 阶段C：长期记忆（持久事实 + embedding 向量检索）配置 ───────────────
//   dice/ai.memory.long { enabled, embed_model_id(取其 base_url/api_key), embed_model(嵌入模型名),
//     top_k(检索条数), min_similarity(相似度阈值 0-1), max_facts(每群事实上限),
//     extract_model_id(抽取用对话模型，空=首个启用), extract_prompt(空=内置默认) }
inline json longConf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    if (a.is_object() && a.contains("memory") && a["memory"].is_object()) {
        json m = a["memory"];
        if (m.contains("long") && m["long"].is_object()) return m["long"];
    }
    return json::object();
}
inline std::string embedModelName(ConfigManager& cfg) { return longConf(cfg).value("embed_model", std::string()); }
inline bool longEnabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && longConf(cfg).value("enabled", false) && !embedModelName(cfg).empty();
}
inline int topK(ConfigManager& cfg) { int n = longConf(cfg).value("top_k", 4); return n < 1 ? 1 : (n > 20 ? 20 : n); }
inline float minSim(ConfigManager& cfg) { double s = longConf(cfg).value("min_similarity", 0.75); return (float)(s < 0 ? 0 : (s > 1 ? 1 : s)); }
inline int maxFacts(ConfigManager& cfg) { int n = longConf(cfg).value("max_facts", 300); return n < 20 ? 20 : (n > 5000 ? 5000 : n); }

// 选嵌入用连接（复用某个已配置模型的 base_url/api_key）。
inline ai::Model embedConn(ConfigManager& cfg) {
    ai::Model m; bool got = false;
    std::string want = longConf(cfg).value("embed_model_id", std::string());
    for (auto& mm : ai::models(cfg)) {
        if (!mm.enabled) continue;
        if (!got) { m = mm; got = true; }
        if (!want.empty() && mm.id == want) { m = mm; break; }
    }
    return m;
}
inline std::string defaultExtractPrompt() {
    return "你在为一个 TRPG 跑团 QQ 群维护长期记忆。从下面的对话里，抽取值得长期记住的**持久事实**："
        "人物设定与偏好、人物卡/角色信息、正在进行的剧情线索、群内约定与关系、重要事件。"
        "忽略寒暄、临时话题、掷骰结果等易过时内容。以 JSON 字符串数组输出，每条一句话、"
        "自带主语（如「希亚的调查员叫阿罗，力量60」），最多 8 条；没有可记的就输出 []。只输出 JSON 数组。";
}

inline std::string defaultSummaryPrompt() {
    return "你在维护一个 TRPG 跑团 QQ 群的对话记忆。下面给你【已有摘要】和【新增对话】，"
        "请把它们整合、改写成一份更新后的简洁摘要：保留群里的人物、正在进行的话题/剧情、"
        "重要决定与约定、人物关系与情绪；丢弃寒暄和无意义刷屏。用要点或短句，不要逐条复述原文，"
        "控制在指定字数内。只输出摘要正文，不要额外说明。";
}

// 用大模型把（已有摘要 + 新增对话）压缩为更新后的摘要。纯函数：任何失败返回空。
inline std::string summarize(ConfigManager& cfg, const std::string& prevSummary, const std::string& newDialog) {
    ai::Model m; bool got = false;
    std::string want = shortConf(cfg).value("summary_model_id", std::string());
    for (auto& mm : ai::models(cfg)) {
        if (!mm.enabled) continue;
        if (!got) { m = mm; got = true; }
        if (!want.empty() && mm.id == want) { m = mm; got = true; break; }
    }
    if (!got) return "";
    std::string sys = shortConf(cfg).value("summary_prompt", std::string());
    if (sys.empty()) sys = defaultSummaryPrompt();
    int maxChars = summaryMaxChars(cfg);
    std::string user;
    user += "【已有摘要】\n" + (prevSummary.empty() ? std::string("（暂无）") : prevSummary) + "\n\n";
    user += "【新增对话】\n" + newDialog + "\n";
    user += "请输出更新后的摘要（不超过约" + std::to_string(maxChars) + "字）：";
    ai::Result r = ai::chat(cfg, m, sys, user, maxChars * 2 + 64, 40);
    if (!r.ok || r.reply.empty()) return "";
    return dice::aichat::truncateChars(r.reply, maxChars);
}

// 阶段C：从一批对话里抽取持久事实（LLM 调用，期望 JSON 字符串数组）。任何失败返回空。
inline std::vector<std::string> extractFacts(ConfigManager& cfg, const std::string& dialog) {
    std::vector<std::string> out;
    ai::Model m; bool got = false;
    std::string want = longConf(cfg).value("extract_model_id", std::string());
    for (auto& mm : ai::models(cfg)) {
        if (!mm.enabled) continue;
        if (!got) { m = mm; got = true; }
        if (!want.empty() && mm.id == want) { m = mm; got = true; break; }
    }
    if (!got) return out;
    std::string sys = longConf(cfg).value("extract_prompt", std::string());
    if (sys.empty()) sys = defaultExtractPrompt();
    std::string user = "对话：\n" + dialog + "\n请抽取持久事实，输出 JSON 字符串数组：";
    ai::Result r = ai::chat(cfg, m, sys, user, 512, 40, 0.2);
    if (!r.ok || r.reply.empty()) return out;
    // 解析 JSON 数组（容错：截取第一个 [ 到最后一个 ]）。
    std::string s = r.reply;
    auto lb = s.find('['), rb = s.rfind(']');
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) s = s.substr(lb, rb - lb + 1);
    try {
        json a = json::parse(s);
        if (a.is_array())
            for (auto& e : a) {
                if (!e.is_string()) continue;
                std::string f = e.get<std::string>();
                if (!f.empty() && f.size() <= 400) out.push_back(f);
                if (out.size() >= 8) break;
            }
    } catch (...) {}
    return out;
}

// 读取某 scope 的当前摘要（供对话注入）。无则返回空。Store = ChatStorage*。
template <class Store>
inline std::string currentSummary(Store* cst, const std::string& scope, const std::string& scopeId) {
    if (!cst) return "";
    namespace orm = sqlite_orm;
    try {
        auto rows = cst->template get_all<AiMemoryRow>(
            orm::where(orm::c(&AiMemoryRow::scope) == scope and orm::c(&AiMemoryRow::scopeId) == scopeId
                and orm::c(&AiMemoryRow::kind) == std::string("summary")),
            orm::limit(1));
        if (!rows.empty()) return rows.front().content;
    } catch (...) {}
    return "";
}

// 阶段C：把抽取到的事实向量化后存入（去重：与已有事实余弦>0.92 视为重复跳过；超量则
// 按 hits 升序、旧的优先剪枝）。Store = ChatStorage*。
template <class Store>
inline void storeFacts(ConfigManager& cfg, Store* cst, const std::string& scope, const std::string& scopeId,
                       const std::vector<std::string>& facts) {
    if (!cst || facts.empty() || !longEnabled(cfg)) return;
    namespace orm = sqlite_orm;
    ai::EmbedResult er = ai::embed(cfg, embedConn(cfg), embedModelName(cfg), facts, 30);
    if (!er.ok) return;
    try {
        // 载入本 scope 已有事实的向量（去重用）。
        auto existing = cst->template get_all<AiMemoryRow>(
            orm::where(orm::c(&AiMemoryRow::scope) == scope and orm::c(&AiMemoryRow::scopeId) == scopeId
                and orm::c(&AiMemoryRow::kind) == std::string("fact")));
        std::vector<std::vector<float>> exVecs;
        exVecs.reserve(existing.size());
        for (auto& e : existing) exVecs.push_back(ai::vecFromJson(e.embedding));
        int64_t now = (int64_t)std::time(nullptr);
        for (size_t i = 0; i < facts.size(); ++i) {
            const auto& v = er.vectors[i];
            if (v.empty()) continue;
            bool dup = false;
            for (auto& ev : exVecs) if (ai::cosine(v, ev) > 0.92f) { dup = true; break; }
            if (dup) continue;
            AiMemoryRow nr; nr.scope = scope; nr.scopeId = scopeId; nr.kind = "fact";
            nr.content = facts[i]; nr.embedding = ai::vecToJson(v);
            nr.createdAt = now; nr.updatedAt = now;
            cst->insert(nr);
            exVecs.push_back(v);   // 让同批内也去重
        }
        // 超量剪枝：保留 hits 高/较新的，删多余。
        int cap = maxFacts(cfg);
        int total = (int)cst->template count<AiMemoryRow>(
            orm::where(orm::c(&AiMemoryRow::scope) == scope and orm::c(&AiMemoryRow::scopeId) == scopeId
                and orm::c(&AiMemoryRow::kind) == std::string("fact")));
        if (total > cap) {
            auto all = cst->template get_all<AiMemoryRow>(
                orm::where(orm::c(&AiMemoryRow::scope) == scope and orm::c(&AiMemoryRow::scopeId) == scopeId
                    and orm::c(&AiMemoryRow::kind) == std::string("fact")),
                orm::multi_order_by(orm::order_by(&AiMemoryRow::hits).asc(), orm::order_by(&AiMemoryRow::id).asc()));
            int toDel = total - cap;
            for (auto& e : all) { if (toDel-- <= 0) break; cst->remove<AiMemoryRow>(e.id); }
        }
    } catch (...) {}
}

// 阶段C：向量检索 —— 把 query 向量化，对本 scope 的事实算余弦，取 >=阈值 的 Top-K 内容
// （命中 hits+1）。返回事实文本列表。Store = ChatStorage*。
template <class Store>
inline std::vector<std::string> retrieveFacts(ConfigManager& cfg, Store* cst, const std::string& scope,
                                              const std::string& scopeId, const std::string& query) {
    std::vector<std::string> out;
    if (!cst || query.empty() || !longEnabled(cfg)) return out;
    namespace orm = sqlite_orm;
    ai::EmbedResult er = ai::embed(cfg, embedConn(cfg), embedModelName(cfg), {query}, 20);
    if (!er.ok || er.vectors.empty() || er.vectors[0].empty()) return out;
    const auto& qv = er.vectors[0];
    try {
        auto rows = cst->template get_all<AiMemoryRow>(
            orm::where(orm::c(&AiMemoryRow::scope) == scope and orm::c(&AiMemoryRow::scopeId) == scopeId
                and orm::c(&AiMemoryRow::kind) == std::string("fact")));
        std::vector<std::pair<float, size_t>> scored;
        for (size_t i = 0; i < rows.size(); ++i) {
            float s = ai::cosine(qv, ai::vecFromJson(rows[i].embedding));
            if (s >= minSim(cfg)) scored.push_back({s, i});
        }
        std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
        int k = topK(cfg);
        for (auto& sc : scored) {
            if ((int)out.size() >= k) break;
            out.push_back(rows[sc.second].content);
            try { auto row = rows[sc.second]; row.hits += 1; row.updatedAt = (int64_t)std::time(nullptr); cst->update(row); } catch (...) {}
        }
    } catch (...) {}
    return out;
}

// 若已滑出近期窗口、未折叠的消息累积到一批，则处理这一批（阻塞式：调用方应放后台线程）：
//   · shortEnabled → 用模型更新群摘要（kind="summary"）
//   · longEnabled  → 从该批抽取持久事实、向量化后存入（kind="fact"）
// 用 summary 行的 refId 做统一水位线。两者都关则直接返回。摘要模型失败则不推进水位线。
template <class Store>
inline void maybeFold(ConfigManager& cfg, Store* cst, const std::string& platform, const std::string& groupId) {
    bool sEn = shortEnabled(cfg), lEn = longEnabled(cfg);
    if ((!sEn && !lEn) || !cst || platform.empty() || groupId.empty()) return;
    namespace orm = sqlite_orm;
    try {
        const std::string scope = "group";
        const std::string scopeId = platform + ":" + groupId;
        int keep = keepRounds(cfg);
        int total = (int)cst->template count<ChatMsgRow>(
            orm::where(orm::c(&ChatMsgRow::platform) == platform and orm::c(&ChatMsgRow::groupId) == groupId));
        if (total <= keep) return;  // 尚无消息滑出近期窗口

        // 近期窗口里最旧一条的 id：早于它的才可折叠。
        auto recent = cst->template get_all<ChatMsgRow>(
            orm::where(orm::c(&ChatMsgRow::platform) == platform and orm::c(&ChatMsgRow::groupId) == groupId),
            orm::order_by(&ChatMsgRow::id).desc(), orm::limit(keep));
        if ((int)recent.size() < keep) return;
        int64_t firstVerbatimId = recent.back().id;

        // 统一水位线行（kind="summary"，即使 short 关也用它存 refId；content 可为空）。
        AiMemoryRow row; bool have = false;
        {
            auto rows = cst->template get_all<AiMemoryRow>(
                orm::where(orm::c(&AiMemoryRow::scope) == scope and orm::c(&AiMemoryRow::scopeId) == scopeId
                    and orm::c(&AiMemoryRow::kind) == std::string("summary")),
                orm::limit(1));
            if (!rows.empty()) { row = rows.front(); have = true; }
        }
        int64_t watermark = have ? row.refId : 0;

        // 可折叠 = 已滑出窗口(id<firstVerbatimId) 且未折叠(id>watermark)。
        auto fold = cst->template get_all<ChatMsgRow>(
            orm::where(orm::c(&ChatMsgRow::platform) == platform and orm::c(&ChatMsgRow::groupId) == groupId
                and orm::c(&ChatMsgRow::id) > watermark and orm::c(&ChatMsgRow::id) < firstVerbatimId),
            orm::order_by(&ChatMsgRow::id).asc(), orm::limit(300));
        if ((int)fold.size() < keep) return;  // 攒够一批再折叠，省成本

        std::string dialog; int64_t newWatermark = watermark;
        for (auto& mm : fold) {
            if (mm.recalled) continue;  // 已撤回的不进记忆
            dialog += mm.sender + "\xef\xbc\x9a" + dice::aichat::cleanForAi(cfg, mm.content) + "\n";
            if (mm.id > newWatermark) newWatermark = mm.id;
        }

        bool advance = true;
        std::string newSummary = have ? row.content : std::string();
        if (sEn) {
            std::string updated = summarize(cfg, have ? row.content : std::string(), dialog);
            if (updated.empty()) advance = false;   // 摘要失败：保留旧摘要，不推进水位线
            else newSummary = updated;
        }
        if (lEn && advance) {                        // 长期事实：只在会提交这一批时抽取
            auto facts = extractFacts(cfg, dialog);
            if (!facts.empty()) storeFacts(cfg, cst, scope, scopeId, facts);
        }
        if (!advance) return;

        int64_t now = (int64_t)std::time(nullptr);
        if (have) {
            row.content = newSummary; row.refId = newWatermark; row.updatedAt = now;
            cst->update(row);
        } else {
            AiMemoryRow nr; nr.scope = scope; nr.scopeId = scopeId; nr.kind = "summary";
            nr.content = newSummary; nr.refId = newWatermark; nr.createdAt = now; nr.updatedAt = now;
            cst->insert(nr);
        }
    } catch (...) {}
}

}  // namespace dice::aimemory
