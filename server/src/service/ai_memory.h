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
//   { enabled, rounds(逐条保留的近期窗口/折叠批大小), summary_prompt(空=内置默认),
//     max_chars(摘要字数上限) }；模型由用途分配的「记忆摘要」决定。
//
// 阶段 C 将复用 ai_memory 表存持久事实（kind="fact"，带 embedding 向量检索）。

#include "../config/config_manager.h"
#include "../storage/database.h"
#include "ai_usage.h"
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
//   dice/ai.memory.long { enabled, top_k(检索条数), min_similarity(相似度阈值 0-1),
//     max_facts(每群事实上限), extract_prompt(空=内置默认) }
//   嵌入与抽取的模型由用途分配的「嵌入」「事实抽取」决定。
inline json longConf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    if (a.is_object() && a.contains("memory") && a["memory"].is_object()) {
        json m = a["memory"];
        if (m.contains("long") && m["long"].is_object()) return m["long"];
    }
    return json::object();
}
inline bool longEnabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && longConf(cfg).value("enabled", false) && ai::hasModel(cfg, "embed");
}
inline int topK(ConfigManager& cfg) { int n = longConf(cfg).value("top_k", 4); return n < 1 ? 1 : (n > 20 ? 20 : n); }
inline float minSim(ConfigManager& cfg) { double s = longConf(cfg).value("min_similarity", 0.75); return (float)(s < 0 ? 0 : (s > 1 ? 1 : s)); }
inline int maxFacts(ConfigManager& cfg) { int n = longConf(cfg).value("max_facts", 300); return n < 20 ? 20 : (n > 5000 ? 5000 : n); }

inline std::string defaultExtractPrompt() {
    return "提取可长期使用的群事实或本群称呼。每项对应一条带行号的原话。"
        "type=fact 表示确认的事实；event 表示新发生的进展，写明主体、事件与先后；"
        "correction 仅用于发言人明确纠正原说法，before 填被纠正的旧事实，text 填完整新事实；"
        "alias 用于明确的群内称呼或黑话，term 是原词，text 是含义。玩家本人和角色分开。"
        "猜测、玩笑、尚未承诺的行动不作为事实或纠正。最多8项，只输出 JSON 数组。";
}

inline std::string defaultSummaryPrompt() {
    return "你在维护一个 TRPG 跑团 QQ 群的对话记忆。下面给你【已有摘要】和【新增对话】，"
        "请把它们整合、改写成一份更新后的简洁摘要：保留群里的人物、正在进行的话题/剧情、"
        "重要决定与约定、人物关系与情绪；分清场内剧情与场外聊天，以当事人最后确认的说法为准；丢弃寒暄和无意义刷屏。用要点或短句，不要逐条复述原文，"
        "控制在指定字数内。只输出摘要正文，不要额外说明。";
}

// 用大模型把（已有摘要 + 新增对话）压缩为更新后的摘要。纯函数：任何失败返回空。
inline std::string summarize(ConfigManager& cfg, const std::string& prevSummary, const std::string& newDialog) {
    if (!ai::hasModel(cfg, "summary")) return "";
    std::string sys = shortConf(cfg).value("summary_prompt", std::string());
    if (sys.empty()) sys = defaultSummaryPrompt();
    int maxChars = summaryMaxChars(cfg);
    std::string user;
    user += "【已有摘要】\n" + (prevSummary.empty() ? std::string("（暂无）") : prevSummary) + "\n\n";
    user += "【新增对话】\n" + newDialog + "\n";
    user += "请输出更新后的摘要（不超过约" + std::to_string(maxChars) + "字）：";
    ai::Result r = ai::chatFor(cfg, "summary", sys, user, maxChars * 2 + 64, 40);
    if (!r.ok || r.reply.empty()) return "";
    return dice::aichat::truncateChars(r.reply, maxChars);
}

struct Finding {
    std::string type, text, before, term;
    int64_t source = 0;
};

inline std::vector<Finding> parseFindings(const json& items, const std::vector<ChatMsgRow>& lines) {
    std::vector<Finding> found;
    if (!items.is_array()) return found;
    for (const auto& item : items) {
        Finding f;
        if (item.is_string()) { f.type = "fact"; f.text = item.get<std::string>(); } // custom legacy extractors
        else if (item.is_object()) {
            auto field = [&](const char* key) -> std::string {
                auto it = item.find(key);
                return it != item.end() && it->is_string() ? it->get<std::string>() : "";
            };
            f.type = field("type"); f.text = field("text"); f.before = field("before"); f.term = field("term");
            auto id = item.find("source");
            if (id == item.end() || !id->is_number_integer()) continue;
            f.source = id->get<int64_t>();
            auto line = std::find_if(lines.begin(), lines.end(), [&](const auto& l){ return !l.recalled && l.id == f.source; });
            if (line == lines.end()) continue;
            if (f.type == "alias" && (line->self || f.term.empty() || f.term.size() > 96 || line->content.find(f.term) == std::string::npos)) continue;
            if (f.type == "correction" && (line->self || f.before.empty() || f.before.size() > 600)) continue;
            if (f.type != "alias" && f.type != "fact" && f.type != "event" && f.type != "correction") continue;
        }
        if (!f.text.empty() && f.text.size() <= 600) found.push_back(std::move(f));
        if (found.size() == 8) break;
    }
    return found;
}

inline std::optional<std::vector<Finding>> extractFacts(ConfigManager& cfg, const std::string& dialog,
                                                         const std::vector<ChatMsgRow>& lines) {
    auto sys = longConf(cfg).value("extract_prompt", std::string());
    if (sys.empty()) sys = defaultExtractPrompt();
    std::string user = dialog + "\n输出数组，每项包含 type,text,source（原消息行号）；correction 另有 before，alias 另有 term。";
    auto r = ai::chatFor(cfg, "extract", sys, user, 1000, 40, 0.2);
    if (!r.ok) return {};
    auto start = r.reply.find('['), end = r.reply.rfind(']');
    if (start == std::string::npos || end == std::string::npos || end < start) return {};
    auto items = json::parse(r.reply.substr(start, end - start + 1), nullptr, false);
    if (!items.is_array()) return {};
    return parseFindings(items, lines);
}

inline json metadata(const AiMemoryRow& row) {
    auto parsed = json::parse(row.metadata, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
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

// Optimistic update protects panel edits made during the extraction/model call.
template<class Store>
bool correctFact(Store* cst, const AiMemoryRow& old, const Finding& f, const std::string& embedding, int64_t now) {
    namespace orm = sqlite_orm;
    auto meta = metadata(old);
    auto& prior = meta["previous"];
    if (!prior.is_array()) prior = json::array();
    if (prior.size() >= 20) prior.erase(prior.begin());
    prior.push_back({{"text", old.content}, {"source", old.refId}});
    meta["type"] = "correction";
    cst->update_all(orm::set(orm::c(&AiMemoryRow::content) = f.text, orm::c(&AiMemoryRow::metadata) = meta.dump(),
        orm::c(&AiMemoryRow::refId) = f.source, orm::c(&AiMemoryRow::embedding) = embedding,
        orm::c(&AiMemoryRow::updatedAt) = now),
        orm::where(orm::c(&AiMemoryRow::id) == old.id && orm::c(&AiMemoryRow::content) == old.content
            && orm::c(&AiMemoryRow::metadata) == old.metadata && orm::c(&AiMemoryRow::refId) == old.refId));
    return cst->changes() == 1;
}

template<class Store>
std::string glossary(Store* cst, const std::string& scopeId, const std::string& text) {
    if (!cst || text.empty()) return {};
    namespace orm = sqlite_orm;
    std::string context;
    for (const auto& row : cst->template get_all<AiMemoryRow>(orm::where(
        orm::c(&AiMemoryRow::scopeId) == scopeId && orm::c(&AiMemoryRow::kind) == std::string("alias")),
        orm::order_by(&AiMemoryRow::updatedAt).desc())) {
        auto m = metadata(row);
        auto term = m.value("term", std::string());
        if (!m.value("approved", false) || term.empty()) continue;
        auto word = [](char c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
        size_t at = text.find(term);
        while (at != std::string::npos) {
            auto end = at + term.size();
            if ((!word(term.front()) || !at || !word(text[at-1])) && (!word(term.back()) || end == text.size() || !word(text[end]))) break;
            at = text.find(term, at + 1);
        }
        if (at == std::string::npos) continue;
        context += "\n" + term + "：" + row.content;
        if (context.size() >= 1500) break;
    }
    return context;
}

template<class Store, class Embed, class Verify>
bool writeFindings(ConfigManager& cfg, Store* cst, const std::string& scope, const std::string& scopeId,
                   const std::vector<Finding>& findings, Embed embed, Verify verify) {
    if (!cst) return false;
    namespace orm = sqlite_orm;
    std::vector<std::string> texts;
    for (auto& f : findings) if (f.type != "alias") texts.push_back(f.text);
    ai::EmbedResult er;
    if (!texts.empty()) {
        er = embed(texts);
        if (!er.ok || er.vectors.size() != texts.size()) return false;
        for (auto& v : er.vectors) if (v.empty()) return false;
    }
    try {
        size_t next = 0;
        auto now = static_cast<int64_t>(std::time(nullptr));
        for (auto& f : findings) {
            std::string kind = f.type == "alias" ? "alias" : "fact";
            auto existing = cst->template get_all<AiMemoryRow>(orm::where(
                orm::c(&AiMemoryRow::scope) == scope && orm::c(&AiMemoryRow::scopeId) == scopeId && orm::c(&AiMemoryRow::kind) == kind));
            AiMemoryRow row; row.scope = scope; row.scopeId = scopeId; row.kind = kind;
            row.content = f.text; row.refId = f.source; row.createdAt = row.updatedAt = now;
            row.metadata = json{{"type",f.type}}.dump();
            if (kind == "alias") {
                if (existing.size() >= 200 || std::any_of(existing.begin(),existing.end(),[&](const auto& e){
                    return metadata(e).value("term",std::string()) == f.term && (e.content == f.text || e.refId == f.source);
                })) continue;
                row.metadata = json{{"term",f.term},{"approved",false}}.dump();
            } else {
                const auto& v = er.vectors[next++];
                row.embedding = ai::vecToJson(v);
                if (std::any_of(existing.begin(),existing.end(),[&](const auto& e){
                    if (e.content == f.text && (f.type == "fact" || e.refId == f.source)) return true;
                    if (f.source) {
                        auto m = metadata(e);
                        for (const auto& prior : m.value("previous", json::array()))
                            if (prior.value("source",int64_t(0)) == f.source && prior.value("text",std::string()) == f.text) return true;
                    }
                    return false;
                })) continue;
                if (f.type == "fact") {
                    const AiMemoryRow* closest = nullptr;
                    double best = 0.92;
                    for (const auto& e : existing) {
                        if (metadata(e).value("type",std::string()) == "event") continue;
                        auto score = ai::cosine(v, ai::vecFromJson(e.embedding));
                        if (score > best) { best = score; closest = &e; }
                    }
                    if (closest) {
                        auto same = verify(ChatMsgRow{}, *closest, f);
                        if (!same) return false;
                        if (*same) continue;
                    }
                }
                if (f.type == "correction") {
                    // Exact prior wording first. A close embedding alone cannot invalidate a fact.
                    auto match = std::find_if(existing.begin(), existing.end(), [&](const auto& e){ return e.content == f.before; });
                    if (match == existing.end() && !existing.empty()) {
                        auto prior = embed(std::vector<std::string>{f.before});
                        if (!prior.ok || prior.vectors.empty()) return false;
                        double best = 0.75;
                        for (auto it = existing.begin(); it != existing.end(); ++it) {
                            auto score = ai::cosine(prior.vectors.front(), ai::vecFromJson(it->embedding));
                            if (metadata(*it).value("type",std::string()) == "event") continue;
                            if (score > best) { best = score; match = it; }
                        }
                    }
                    if (match != existing.end() && f.source > match->refId && metadata(*match).value("type",std::string()) != "event") {
                        auto source = cst->template get_pointer<ChatMsgRow>(f.source);
                        if (!source || source->recalled || source->platform + ":" + source->groupId != scopeId) continue;
                        auto check = verify(*source, *match, f);
                        if (!check.has_value()) return false;
                        if (*check) {
                            if (!correctFact(cst,*match,f,row.embedding,now)) return false;
                            continue;
                        }
                    }
                    // Keep unmatched statements attributed, so retrieval can distinguish a claim from a confirmed replacement.
                    row.content = "更正陈述：" + f.text;
                    if (std::any_of(existing.begin(),existing.end(),[&](const auto& e){return e.refId == f.source && e.content == row.content;})) continue;
                }
            }
            cst->insert(row);
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
            DICE_LOG_DEBUG_M("AI 记忆", "[{}] 事实共 {} 条，超过上限 {}，删除命中最少的 {} 条。", scopeId, total, cap, toDel);
            for (auto& e : all) { if (toDel-- <= 0) break; DICE_LOG_DEBUG_M("AI 记忆", "[{}] 删除事实：{}", scopeId, e.content); cst->template remove<AiMemoryRow>(e.id); }
        }
    } catch (...) { DICE_LOG_EXCEPTION("store AI memory"); return false; }
    return true;
}

template<class Store>
bool storeFacts(ConfigManager& cfg, Store* cst, const std::string& scope, const std::string& scopeId,
                const std::vector<Finding>& findings) {
    auto verify = [&](const ChatMsgRow& source, const AiMemoryRow& old, const Finding& f) -> std::optional<bool> {
        json evidence = {{"更正人",source.sender},{"更正原话",aichat::cleanForAi(cfg,source.content)},
                         {"旧事实",old.content},{"新说法",f.text}};
        if (old.refId) {
            auto origin = cst->template get_pointer<ChatMsgRow>(old.refId);
            if (origin && !origin->recalled && origin->platform + ":" + origin->groupId == scopeId) {
                evidence["旧事实的发言人"] = origin->sender;
                evidence["旧事实的原话"] = aichat::cleanForAi(cfg,origin->content);
                evidence["同一发言人"] = origin->userId == source.userId;
            }
        }
        const std::string instruction = f.type == "fact"
            ? "新旧事实是否表达完全相同的信息？主体、地点、数值、时间、确定程度都一致才输出 yes，否则 no。"
            : "判断更正原话是否明确纠正同一主体的这条旧事实。角色猜测、剧情后续、"
            "第三者猜测他人的个人偏好或不清楚对应关系都输出 no。直接更正输出 yes。只输出 yes 或 no。";
        auto r = ai::chatFor(cfg,"extract",instruction,evidence.dump(),16,15,0);
        if (!r.ok) return {};
        const auto first = r.reply.find_first_not_of(" \r\n\t"), last = r.reply.find_last_not_of(" \r\n\t");
        return first != std::string::npos && r.reply.substr(first,last-first+1) == "yes";
    };
    return writeFindings(cfg,cst,scope,scopeId,findings,
        [&](const std::vector<std::string>& texts){return ai::embedFor(cfg,texts,30);},verify);
}

// 阶段C：向量检索 —— 把 query 向量化，对本 scope 的事实算余弦，取 >=阈值 的 Top-K 内容
// （命中 hits+1）。返回事实文本列表。Store = ChatStorage*。
template <class Store>
inline std::vector<std::string> retrieveFacts(ConfigManager& cfg, Store* cst, const std::string& scope,
                                              const std::string& scopeId, const std::string& query) {
    std::vector<std::string> out;
    if (!cst || query.empty() || !longEnabled(cfg)) return out;
    namespace orm = sqlite_orm;
    ai::EmbedResult er = ai::embedFor(cfg, {query}, 20);
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
        if (ai::debugOn()) {
            std::string hits;
            for (size_t i = 0; i < scored.size() && (int)i < k; ++i)
                hits += "\n" + std::to_string(scored[i].first).substr(0, 5) + " " + rows[scored[i].second].content;
            DICE_LOG_DEBUG_M("AI 记忆", "[{}] 检索长期记忆：共 {} 条事实，{} 条达到相似度 {}，取前 {} 条。{}", scopeId, rows.size(),
                             scored.size(), minSim(cfg), k, hits);
        }
        for (auto& sc : scored) {
            if ((int)out.size() >= k) break;
            const auto& row = rows[sc.second];
            auto m = metadata(row);
            std::string fact = row.content;
            if (m.value("type",std::string()) == "event") fact = "剧情进展：" + fact;
            if (row.refId) fact += "（群消息顺序 " + std::to_string(row.refId) + "）";
            if (m.contains("previous") && m["previous"].is_array() && !m["previous"].empty())
                fact += "；更正前曾记录为「" + m["previous"].back().value("text",std::string()) + "」，现以更正为准";
            out.push_back(std::move(fact));
            try { cst->update_all(orm::set(orm::c(&AiMemoryRow::hits) = orm::c(&AiMemoryRow::hits) + 1),
                orm::where(orm::c(&AiMemoryRow::id) == rows[sc.second].id)); } catch (...) {}
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
        static std::array<std::mutex,64> folding;
        std::unique_lock lock(folding[std::hash<std::string>{}(scopeId) % folding.size()], std::try_to_lock);
        if (!lock.owns_lock()) return;
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
        DICE_LOG_DEBUG_M("AI 记忆", "[{}] 开始整理记忆：折叠 {} 条已滑出近期窗口的消息（水位线 {} → 窗口起点 {}），摘要{}、事实抽取{}。",
                         scopeId, fold.size(), watermark, firstVerbatimId, sEn ? "开" : "关", lEn ? "开" : "关");

        std::string dialog;
        const int64_t newWatermark = fold.back().id;
        for (auto& mm : fold) {
            if (mm.recalled) continue;  // 已撤回的不进记忆
            dialog += "[" + std::to_string(mm.id) + "] " + (mm.self ? "本账号 " : "群友 ") + mm.sender + "\xef\xbc\x9a" + dice::aichat::cleanForAi(cfg, mm.content) + "\n";
        }

        bool advance = true;
        std::string newSummary = have ? row.content : std::string();
        if (sEn && !dialog.empty()) {
            std::string updated = summarize(cfg, have ? row.content : std::string(), dialog);
            if (updated.empty()) { advance = false; DICE_LOG_DEBUG_M("AI 记忆", "[{}] 摘要失败，保留旧摘要，下次再试。", scopeId); }   // 不推进水位线
            else { newSummary = updated; DICE_LOG_DEBUG_M("AI 记忆", "[{}] 摘要已更新：\n{}", scopeId, updated); }
        }
        if (lEn && advance && !dialog.empty()) {                        // 长期事实：只在会提交这一批时抽取
            auto facts = extractFacts(cfg, dialog, fold);
            if (!facts || !storeFacts(cfg, cst, scope, scopeId, *facts)) return;
        }
        if (!advance) return;

        int64_t now = (int64_t)std::time(nullptr);
        if (have) {
            // Do not overwrite a user edit/delete that occurred during the model request.
            cst->update_all(orm::set(orm::c(&AiMemoryRow::content) = newSummary,
                orm::c(&AiMemoryRow::refId) = newWatermark, orm::c(&AiMemoryRow::updatedAt) = now),
                orm::where(orm::c(&AiMemoryRow::id) == row.id and orm::c(&AiMemoryRow::content) == row.content
                    and orm::c(&AiMemoryRow::refId) == row.refId and orm::c(&AiMemoryRow::updatedAt) == row.updatedAt));
        } else {
            AiMemoryRow nr; nr.scope = scope; nr.scopeId = scopeId; nr.kind = "summary";
            nr.content = newSummary; nr.refId = newWatermark; nr.createdAt = now; nr.updatedAt = now;
            cst->insert(nr);
        }
    } catch (...) {}
}

}  // namespace dice::aimemory
