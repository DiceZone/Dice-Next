#pragma once
// ─── Dice!Next — AI NPC 扮演（智能化阶段E）─────────────────────────────
// 让骰娘按剧本扮演 NPC：定义 NPC 的名字/人设/背景知识/触发词（可按群或全局）。群里消息
// 提到 NPC 名字或命中触发词时，就以该 NPC 的身份、口吻、知识回复（复用阶段B/C 记忆 +
// 阶段D 工具调用，只是把系统人设换成 NPC 的）。建立在 A–D 之上。
//
// 配置 dice/ai.npc：
//   { enabled, list: [ { id, name, persona, knowledge, triggers[], model_id,
//                        group("" = 全局，否则 "平台:群号"), enabled } ] }

#include "../config/config_manager.h"
#include "../storage/database.h"
#include "ai_gateway.h"
#include <sqlite_orm/sqlite_orm.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <ctime>

namespace dice::ainpc {

using json = nlohmann::json;

struct Npc {
    std::string id, name, persona, knowledge, modelId, group;
    std::vector<std::string> triggers;
    bool moodEnabled = false;   // A1：情绪/关系记忆（按 NPC 开关，默认关）
};

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("npc") && a["npc"].is_object()) ? a["npc"] : json::object();
}
inline bool enabled(ConfigManager& cfg) { return ai::enabled(cfg) && conf(cfg).value("enabled", false); }

inline Npc fromJson(const json& j) {
    Npc n;
    n.id = j.value("id", std::string());
    n.name = j.value("name", std::string());
    n.persona = j.value("persona", std::string());
    n.knowledge = j.value("knowledge", std::string());
    n.modelId = j.value("model_id", std::string());
    n.group = j.value("group", std::string());
    n.moodEnabled = j.value("mood_enabled", false);
    if (j.contains("triggers") && j["triggers"].is_array())
        for (auto& t : j["triggers"]) if (t.is_string() && !t.get<std::string>().empty()) n.triggers.push_back(t.get<std::string>());
    return n;
}

// 本群（groupKey="平台:群号"）可见且启用的 NPC 列表（含全局 group=""）。
inline std::vector<Npc> list(ConfigManager& cfg, const std::string& groupKey) {
    std::vector<Npc> v;
    json c = conf(cfg);
    if (!c.contains("list") || !c["list"].is_array()) return v;
    for (auto& j : c["list"]) {
        if (!j.is_object() || !j.value("enabled", true)) continue;
        std::string g = j.value("group", std::string());
        if (!g.empty() && g != groupKey) continue;   // 限定某群但不是本群 → 跳过
        Npc n = fromJson(j);
        if (!n.name.empty()) v.push_back(std::move(n));
    }
    return v;
}

// 在文本里匹配一个 NPC：名字出现或命中任一触发词。返回首个匹配。
inline bool match(ConfigManager& cfg, const std::string& groupKey, const std::string& text, Npc& out) {
    for (auto& n : list(cfg, groupKey)) {
        if (!n.name.empty() && text.find(n.name) != std::string::npos) { out = n; return true; }
        for (auto& tr : n.triggers)
            if (!tr.empty() && text.find(tr) != std::string::npos) { out = n; return true; }
    }
    return false;
}

// 组装 NPC 的系统提示词（完全替换骰娘默认人设，传给 aichat::generate 的 systemOverride）。
// A1：mood 非空时注入对该玩家的当前关系（好感/情绪），让 NPC 语气随关系变化。
inline std::string systemPrompt(const Npc& n, const json& mood = json(), const std::string& sender = "") {
    std::string s = "你正在一个 TRPG 跑团 QQ 群里扮演一个 NPC 角色。请严格以这个角色的身份、口吻、"
        "立场和已知信息来回复，绝不要跳出角色，不要说自己是 AI、机器人或骰娘，不要复述设定原文。"
        "回复简短、口语化，符合角色性格，一般一两句话。\n";
    s += "角色名：" + n.name + "\n";
    if (!n.persona.empty())   s += "性格/人设：" + n.persona + "\n";
    if (!n.knowledge.empty()) s += "背景知识（只有角色知道的、可据此回答）：" + n.knowledge + "\n";
    if (mood.is_object() && !mood.empty() && !sender.empty()) {
        int favor = mood.value("favor", 0);
        std::string mo = mood.value("mood", std::string());
        s += "你对「" + sender + "」的当前关系：好感 " + std::to_string(favor)
           + "（-100 厌恶 ~ 100 亲密）" + (mo.empty() ? std::string() : ("，情绪「" + mo + "」"))
           + "。请让语气与态度符合这一关系，但不要把数值直接说出来。\n";
    }
    return s;
}

// ─── A1：NPC 情绪/关系记忆 ────────────────────────────────────────
// 每个 (群, NPC, 用户) 一行 ai_memory(kind="npc_mood"，scope="npc"，
// scopeId="群key|npcId|userId")，content=JSON {favor:-100..100, mood:"词"}。
// 每次互动后后台用小模型评估好感增减（±5），跨对话持久。Store = ChatStorage*。

template <class Store>
inline json getMood(Store* cst, const std::string& groupKey, const std::string& npcId, const std::string& userId) {
    if (!cst) return json::object();
    namespace orm = sqlite_orm;
    try {
        std::string sid = groupKey + "|" + npcId + "|" + userId;
        auto rows = cst->template get_all<AiMemoryRow>(
            orm::where(orm::c(&AiMemoryRow::scope) == std::string("npc")
                and orm::c(&AiMemoryRow::scopeId) == sid
                and orm::c(&AiMemoryRow::kind) == std::string("npc_mood")),
            orm::limit(1));
        if (!rows.empty()) {
            json j = json::parse(rows.front().content, nullptr, false);
            if (j.is_object()) return j;
        }
    } catch (...) {}
    return json::object();
}

// 互动后评估好感变化并写回（阻塞式：调用方应放后台线程）。
template <class Store>
inline void updateMood(ConfigManager& cfg, Store* cst, const Npc& n, const std::string& groupKey,
                       const std::string& userId, const std::string& senderName,
                       const std::string& userText, const std::string& npcReply) {
    if (!cst || !n.moodEnabled) return;
    ai::Model m; bool got = false;
    for (auto& mm : ai::models(cfg)) {
        if (!mm.enabled) continue;
        if (!got) { m = mm; got = true; }
        if (!n.modelId.empty() && mm.id == n.modelId) { m = mm; got = true; break; }
    }
    if (!got) return;
    json cur = getMood(cst, groupKey, n.id, userId);
    int favor = cur.value("favor", 0);
    std::string sys = "你在为 TRPG NPC 维护对玩家的好感度。根据这次互动，输出 JSON："
        "{\"delta\": 整数(-5到5，本次好感变化), \"mood\": \"一两个词的当前情绪\"}。只输出 JSON，不要解释。";
    std::string user = "NPC「" + n.name + "」当前对「" + senderName + "」好感 " + std::to_string(favor)
        + "。\n玩家说：" + userText + "\nNPC 回应：" + npcReply + "\n请评估：";
    ai::Result r = ai::chat(cfg, m, sys, user, 128, 20, 0.2);
    if (!r.ok || r.reply.empty()) return;
    std::string s = r.reply;
    auto lb = s.find('{'), rb = s.rfind('}');
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return;
    json j = json::parse(s.substr(lb, rb - lb + 1), nullptr, false);
    if (!j.is_object()) return;
    int delta = j.value("delta", 0);
    if (delta > 5) delta = 5; if (delta < -5) delta = -5;
    favor += delta;
    if (favor > 100) favor = 100; if (favor < -100) favor = -100;
    json out = {{"favor", favor}, {"mood", j.value("mood", cur.value("mood", std::string()))}};
    namespace orm = sqlite_orm;
    try {
        std::string sid = groupKey + "|" + n.id + "|" + userId;
        auto rows = cst->template get_all<AiMemoryRow>(
            orm::where(orm::c(&AiMemoryRow::scope) == std::string("npc")
                and orm::c(&AiMemoryRow::scopeId) == sid
                and orm::c(&AiMemoryRow::kind) == std::string("npc_mood")),
            orm::limit(1));
        long long now = (long long)std::time(nullptr);
        if (rows.empty()) {
            AiMemoryRow nr; nr.scope = "npc"; nr.scopeId = sid; nr.kind = "npc_mood";
            nr.content = out.dump(); nr.createdAt = now; nr.updatedAt = now;
            cst->insert(nr);
        } else { auto row = rows.front(); row.content = out.dump(); row.updatedAt = now; cst->update(row); }
    } catch (...) {}
}

}  // namespace dice::ainpc
