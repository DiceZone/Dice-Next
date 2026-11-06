#pragma once
// ─── Dice!Next — AI 用途分配 ─────────────────────────────
// 每个功能（回复、决策、润色……）称为一种「用途」。骰主在 dice/ai.usages 里为每种用途
// 指定若干模型和使用策略；调用方只说「我要做什么」，由这里决定用哪个模型，失败时换下一个。
//
// dice/ai.usages：
//   { "<用途>": { "models": [ { "id": 模型id, "weight": 1 } ], "strategy": "balance" } }
// strategy：balance 负载均衡（默认）| random 随机选择 | priority 顺序优先 | weight 分配权重
//
// 某用途没有指定模型时，沿 fallback 链借用其它用途的分配；通用模型用途最终回退到
// 「第一个启用的通用模型」（与旧版行为一致）。语音识别、嵌入不回退：没指定就是不用。

#include "ai_gateway.h"
#include <algorithm>
#include <random>
#include <set>

namespace dice::ai {

struct UsageDef {
    const char* key;
    const char* kind;       // chat | embedding
    const char* fallback;   // 未分配时借用的用途；空 = 回退到第一个启用的通用模型（chat）/ 不回退
    bool needVision;
    bool noFallback;        // true = 未分配就不可用
};

inline const std::vector<UsageDef>& usageDefs() {
    static const std::vector<UsageDef> defs = {
        {"reply",     "chat",      "",      false, false},  // 对话回复
        {"decision",  "chat",      "reply", false, false},  // 决策：待机时判断要不要插话
        {"polish",    "chat",      "reply", false, false},  // 润色
        {"translate", "chat",      "reply", false, false},  // 翻译
        {"summary",   "chat",      "reply", false, false},  // 记忆摘要
        {"extract",   "chat",      "reply", false, false},  // 事实抽取
        {"npc",       "chat",      "reply", false, false},  // NPC 扮演
        {"mood",      "chat",      "npc",   false, false},  // NPC 情绪评估
        {"vision",    "chat",      "reply", true,  false},  // 图像识别（仅支持图片的模型）
        {"speech",    "chat",      "",      false, true},   // 语音识别
        {"embed",     "embedding", "",      false, true},   // 嵌入
    };
    return defs;
}
inline const UsageDef* usageDef(const std::string& key) {
    for (auto& d : usageDefs()) if (key == d.key) return &d;
    return nullptr;
}

inline bool validStrategy(const std::string& s) {
    return s == "balance" || s == "random" || s == "priority" || s == "weight";
}

// 规范化 usages（API 保存时用）：丢弃未知用途、不存在或类别不符的模型、非法策略。
inline json normalizeUsages(const json& input, const json& modelsArr) {
    json out = json::object();
    std::map<std::string, std::string> kindOf;
    if (modelsArr.is_array())
        for (auto& m : modelsArr)
            if (m.is_object()) kindOf[m.value("id", std::string())] = m.value("kind", std::string("chat")) == "embedding" ? "embedding" : "chat";
    for (auto& d : usageDefs()) {
        json src = input.is_object() && input.contains(d.key) && input[d.key].is_object() ? input[d.key] : json::object();
        std::string strategy = src.value("strategy", std::string("balance"));
        if (!validStrategy(strategy)) strategy = "balance";
        json list = json::array();
        std::set<std::string> seen;
        if (src.contains("models") && src["models"].is_array())
            for (auto& e : src["models"]) {
                std::string id = e.is_string() ? e.get<std::string>() : (e.is_object() ? e.value("id", std::string()) : std::string());
                if (id.empty() || seen.count(id)) continue;
                auto it = kindOf.find(id);
                if (it == kindOf.end() || it->second != d.kind) continue;
                int w = e.is_object() ? e.value("weight", 1) : 1;
                if (w < 1) w = 1;
                if (w > 100) w = 100;
                list.push_back(json{{"id", id}, {"weight", w}});
                seen.insert(id);
            }
        // 嵌入向量不能混用不同模型（存下的向量与查询向量必须同源），只保留一个。
        if (std::string(d.key) == "embed") { if (list.size() > 1) list.erase(list.begin() + 1, list.end()); strategy = "priority"; }
        out[d.key] = json{{"models", list}, {"strategy", strategy}};
    }
    return out;
}

// ─── 旧配置迁移 ───────────────────────────────────────────
// 旧版每个模型自带 base_url/api_key，各功能用 *_model_id 指定模型。迁移后：
//   · 相同「Base URL + Key」的模型合并成一个提供商；
//   · memory.long 的嵌入连接 + 嵌入模型名 → 一个嵌入模型；
//   · 各功能的 model_id → 对应用途的分配；图像识别所用模型标记为支持图片。
// 返回是否有改动。已有 providers 字段时视为已迁移，不再处理。
inline bool migrateConfig(json& c) {
    if (!c.is_object() || c.contains("providers")) return false;
    json providers = json::array();
    json models = c.contains("models") && c["models"].is_array() ? c["models"] : json::array();
    std::map<std::pair<std::string, std::string>, std::string> providerOf;
    auto hostName = [](const std::string& url) {
        auto at = url.find("://");
        std::string h = at == std::string::npos ? url : url.substr(at + 3);
        auto slash = h.find('/');
        return slash == std::string::npos ? h : h.substr(0, slash);
    };
    auto providerFor = [&](const std::string& base, const std::string& key) {
        auto k = std::make_pair(base, key);
        auto it = providerOf.find(k);
        if (it != providerOf.end()) return it->second;
        std::string id = "p" + std::to_string(providers.size() + 1);
        std::string name = hostName(base);
        if (name.empty()) name = id;
        int same = 0;
        for (auto& p : providers) if (p.value("name", std::string()).rfind(name, 0) == 0) ++same;
        if (same) name += " (" + std::to_string(same + 1) + ")";
        providers.push_back(json{{"id", id}, {"name", name}, {"base_url", base}, {"api_key", key}, {"enabled", true}});
        providerOf[k] = id;
        return id;
    };
    for (auto& m : models) {
        if (!m.is_object()) continue;
        m["provider_id"] = providerFor(m.value("base_url", std::string()), m.value("api_key", std::string()));
        m.erase("base_url"); m.erase("api_key");
        if (!m.contains("kind")) m["kind"] = "chat";
        if (!m.contains("vision")) m["vision"] = false;
    }
    auto get = [&](std::initializer_list<const char*> path) -> std::string {
        const json* cur = &c;
        for (auto* p : path) {
            if (!cur->is_object() || !cur->contains(p)) return std::string();
            cur = &(*cur)[p];
        }
        return cur->is_string() ? cur->get<std::string>() : std::string();
    };
    auto modelExists = [&](const std::string& id) {
        for (auto& m : models) if (m.is_object() && m.value("id", std::string()) == id) return true;
        return false;
    };
    json usages = json::object();
    auto assign = [&](const char* usage, const std::string& id) {
        if (id.empty() || !modelExists(id)) return;
        usages[usage] = json{{"models", json::array({json{{"id", id}, {"weight", 1}}})}, {"strategy", "balance"}};
    };
    assign("reply", get({"chat", "model_id"}));
    assign("polish", get({"polish", "model_id"}));
    assign("translate", get({"translate", "model_id"}));
    assign("summary", get({"memory", "short", "summary_model_id"}));
    assign("extract", get({"memory", "long", "extract_model_id"}));
    // 图像识别：指定的模型（未指定则第一个启用模型）标记为支持图片。
    std::string visionId = get({"vision", "model_id"});
    bool visionOn = c.contains("vision") && c["vision"].is_object() && c["vision"].value("enabled", false);
    if (visionId.empty() && visionOn)
        for (auto& m : models) if (m.is_object() && m.value("enabled", true)) { visionId = m.value("id", std::string()); break; }
    for (auto& m : models) if (m.is_object() && m.value("id", std::string()) == visionId) m["vision"] = true;
    assign("vision", visionId);
    // 嵌入：连接来自 embed_model_id（未指定则第一个启用模型），模型名来自 embed_model。
    std::string embedName = get({"memory", "long", "embed_model"});
    if (!embedName.empty()) {
        std::string connId = get({"memory", "long", "embed_model_id"});
        json conn;
        for (auto& m : models) {
            if (!m.is_object() || !m.value("enabled", true)) continue;
            if (conn.is_null()) conn = m;
            if (!connId.empty() && m.value("id", std::string()) == connId) { conn = m; break; }
        }
        if (!conn.is_null()) {
            std::string eid = "emb_migrated";
            models.push_back(json{{"id", eid}, {"name", embedName}, {"provider_id", conn.value("provider_id", std::string())},
                                  {"model", embedName}, {"kind", "embedding"}, {"vision", false}, {"enabled", true},
                                  {"price_in", 0.0}, {"price_out", 0.0}, {"token_limit", 0}, {"cost_limit", 0.0},
                                  {"used_tokens", 0}, {"used_cost", 0.0}});
            assign("embed", eid);
        }
    }
    c["providers"] = providers;
    c["models"] = models;
    c["usages"] = normalizeUsages(usages, models);
    return true;
}

// 启动时调用：旧配置迁移一次并保存（保存前在 dice/ai_backup_v1 留一份原样副本）。
inline void ensureMigrated(ConfigManager& cfg) {
    std::lock_guard<std::mutex> lk(usageMutex());
    try {
        json c = conf(cfg);
        if (!c.is_object()) return;
        json before = c;
        if (!migrateConfig(c)) return;
        cfg.set<json>("dice/ai_backup_v1", before);
        cfg.set<json>("dice/ai", c);
        cfg.save();
        DICE_LOG_INFO("AI 模型设置已迁移为「提供商 / 模型 / 用途分配」格式：{} 个提供商，{} 个模型；原设置备份在 dice/ai_backup_v1。",
                      c["providers"].size(), c["models"].size());
    } catch (...) { DICE_LOG_EXCEPTION("AI 模型设置迁移"); }
}

// ─── 选模型 ───────────────────────────────────────────────
inline bool usable(const Model& m) {
    if (!m.enabled || m.baseUrl.empty() || m.model.empty()) return false;
    if (m.tokenLimit > 0 && m.usedTokens >= m.tokenLimit) return false;
    if (m.costLimit > 0 && m.usedCost >= m.costLimit) return false;
    return true;
}

struct Candidate { Model model; int weight = 1; };

inline std::mt19937& usageRng() { thread_local std::mt19937 rng{std::random_device{}()}; return rng; }
inline long long nextRoundRobin(const std::string& usage) {
    static std::mutex mu; static std::map<std::string, long long> rr;
    std::lock_guard<std::mutex> lk(mu);
    return rr[usage]++;
}

// 按策略排好尝试顺序：第一个是本次选中的，其余是失败时依次换用的备选。
inline std::vector<Model> orderByStrategy(std::vector<Candidate> cs, const std::string& strategy,
                                          const std::string& usage, std::string* why = nullptr) {
    std::vector<Model> out;
    if (cs.empty()) return out;
    if (strategy == "priority") {
        if (why) *why = "顺序优先";
    } else if (strategy == "random") {
        std::shuffle(cs.begin(), cs.end(), usageRng());
        if (why) *why = "随机选择";
    } else if (strategy == "weight") {
        // 按权重不放回抽样：权重越大越可能排在前面。
        std::vector<Candidate> rest = cs, picked;
        while (!rest.empty()) {
            int total = 0; for (auto& c : rest) total += c.weight;
            int r = std::uniform_int_distribution<int>(1, std::max(1, total))(usageRng());
            size_t i = 0;
            for (; i < rest.size(); ++i) { r -= rest[i].weight; if (r <= 0) break; }
            if (i >= rest.size()) i = rest.size() - 1;
            picked.push_back(rest[i]); rest.erase(rest.begin() + i);
        }
        cs = std::move(picked);
        if (why) *why = "分配权重";
    } else {
        // 负载均衡：先按处理中的请求数升序；相同的按轮转次序错开。
        long long rr = nextRoundRobin(usage);
        size_t n = cs.size();
        std::vector<std::pair<int, size_t>> key;
        for (size_t i = 0; i < n; ++i) key.push_back({inflight(cs[i].model.id), (i + n - (size_t)(rr % (long long)n)) % n});
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return key[a] < key[b]; });
        std::vector<Candidate> sorted;
        std::string loads;
        for (size_t i : idx) { sorted.push_back(cs[i]); loads += (loads.empty() ? "" : "，") + cs[i].model.id + "=" + std::to_string(key[i].first); }
        cs = std::move(sorted);
        if (why) *why = "负载均衡（处理中：" + loads + "）";
    }
    for (auto& c : cs) out.push_back(c.model);
    return out;
}

// 某用途的候选模型（已按策略排序）。@p overrideId 非空时把该模型放在最前（NPC 专属模型）。
inline std::vector<Model> pick(ConfigManager& cfg, const std::string& usage, const std::string& overrideId = "", bool log = true) {
    json c = conf(cfg);
    auto all = modelsOf(c);
    const UsageDef* def = usageDef(usage);
    if (!def) return {};
    auto find = [&](const std::string& id) -> const Model* {
        for (auto& m : all) if (m.id == id) return &m;
        return nullptr;
    };
    auto fits = [&](const Model& m) {
        if (m.kind != def->kind) return false;
        if (def->needVision && !m.vision) return false;
        return usable(m);
    };
    std::vector<Model> out;
    std::string why, from = usage;
    if (!overrideId.empty())
        if (auto* m = find(overrideId); m && fits(*m)) out.push_back(*m);
    // 沿 fallback 链找第一个有可用分配的用途。
    std::string cur = usage;
    std::set<std::string> visited;
    std::vector<Model> ordered;
    while (!cur.empty() && !visited.count(cur)) {
        visited.insert(cur);
        const UsageDef* d = usageDef(cur);
        json u = c.contains("usages") && c["usages"].is_object() && c["usages"].contains(cur) ? c["usages"][cur] : json::object();
        std::vector<Candidate> cs;
        if (u.contains("models") && u["models"].is_array())
            for (auto& e : u["models"]) {
                std::string id = e.is_object() ? e.value("id", std::string()) : (e.is_string() ? e.get<std::string>() : std::string());
                if (auto* m = find(id); m && fits(*m)) cs.push_back({*m, std::max(1, e.is_object() ? e.value("weight", 1) : 1)});
            }
        if (!cs.empty()) { ordered = orderByStrategy(cs, u.value("strategy", std::string("balance")), cur, &why); from = cur; break; }
        if (def->noFallback || !d) break;
        cur = d->fallback;
    }
    if (ordered.empty() && !def->noFallback && std::string(def->kind) == "chat") {
        for (auto& m : all) if (fits(m)) { ordered.push_back(m); why = "未分配模型，使用第一个启用的通用模型"; from = "默认"; break; }
    }
    for (auto& m : ordered) {
        bool dup = false;
        for (auto& o : out) if (o.id == m.id) { dup = true; break; }
        if (!dup) out.push_back(m);
    }
    if (log && debugOn()) {
        std::string ids;
        for (auto& m : out) ids += (ids.empty() ? "" : " → ") + (m.name.empty() ? m.id : m.name);
        DICE_LOG_DEBUG_M("AI 服务", "用途「{}」选模型：{}{}；尝试顺序：{}", usage,
                         from == usage ? std::string() : "借用「" + from + "」的分配，",
                         why.empty() ? std::string("无") : why, ids.empty() ? std::string("（无可用模型）") : ids);
    }
    return out;
}
inline bool hasModel(ConfigManager& cfg, const std::string& usage) { return !pick(cfg, usage, "", false).empty(); }

constexpr int kMaxAttempts = 3;

// 按用途调用 chat，失败时换下一个候选模型（最多 kMaxAttempts 个）。
inline Result chatFor(ConfigManager& cfg, const std::string& usage, const std::string& systemPrompt,
                      const std::string& userPrompt, int maxTokens = 512, int timeoutSec = 30,
                      double tempOverride = -1, const std::string& overrideId = "") {
    PurposeScope scope(usage);
    Result last; last.error = "no model assigned";
    int tries = 0;
    for (auto& m : pick(cfg, usage, overrideId)) {
        if (tries++ >= kMaxAttempts) break;
        last = chat(cfg, m, systemPrompt, userPrompt, maxTokens, timeoutSec, tempOverride);
        if (last.ok) return last;
        DICE_LOG_DEBUG_M("AI 服务", "用途「{}」模型「{}」失败（{}），换下一个候选模型。", usage, m.name.empty() ? m.id : m.name, last.error);
    }
    return last;
}

inline ChatRawResult chatRawFor(ConfigManager& cfg, const std::string& usage, const json& messages,
                                const json& tools, int maxTokens, int timeoutSec, double tempOverride = -1) {
    PurposeScope scope(usage);
    ChatRawResult last; last.error = "no model assigned";
    int tries = 0;
    for (auto& m : pick(cfg, usage)) {
        if (tries++ >= kMaxAttempts) break;
        last = chatRaw(cfg, m, messages, tools, maxTokens, timeoutSec, tempOverride);
        if (last.ok) return last;
        DICE_LOG_DEBUG_M("AI 服务", "用途「{}」模型「{}」失败（{}），换下一个候选模型。", usage, m.name.empty() ? m.id : m.name, last.error);
    }
    return last;
}

// 嵌入固定只用一个模型（见 normalizeUsages），不做失败换用。
inline EmbedResult embedFor(ConfigManager& cfg, const std::vector<std::string>& texts, int timeoutSec = 30) {
    PurposeScope scope("embed");
    auto ms = pick(cfg, "embed");
    if (ms.empty()) { EmbedResult r; r.error = "no embedding model assigned"; return r; }
    return embed(cfg, ms.front(), texts, timeoutSec);
}

inline std::string transcribeFor(ConfigManager& cfg, const std::filesystem::path& audio, std::string& error) {
    PurposeScope scope("speech");
    error = "no speech model assigned";
    int tries = 0;
    for (auto& m : pick(cfg, "speech")) {
        if (tries++ >= kMaxAttempts) break;
        std::string text = transcribe(cfg, m, audio, error);
        if (!text.empty()) return text;
        DICE_LOG_DEBUG_M("AI 服务", "语音识别模型「{}」失败（{}），换下一个候选模型。", m.name.empty() ? m.id : m.name, error);
    }
    return "";
}

}  // namespace dice::ai
