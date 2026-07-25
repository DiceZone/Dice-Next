#pragma once
// ─── Dice!Next — AI 回复润色（C#68 阶段二）──────────────────────
// 掷骰/检定类回复在发送前，按「人设 + 掷骰上下文」用大模型润色。同步调用（会给这条回复
// 加延迟，是文档里说明的取舍）；失败/超时/未配置→原样返回，绝不影响正常掷骰。
//
// 配置 dice/ai.polish：
//   { enabled, model_id (空=用首个启用模型), mode ("text"=仅润色措辞 / "rp"=按情境加 rp),
//     persona (人设/额外指令) }

#include "../config/config_manager.h"
#include "ai_gateway.h"
#include <nlohmann/json.hpp>
#include <string>

namespace dice::aipolish {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("polish") && a["polish"].is_object()) ? a["polish"] : json::object();
}
inline bool enabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && conf(cfg).value("enabled", false);
}
// C#78：某回复类别是否在润色覆盖范围内（roll/deck/fun/custom/plugin）。默认只覆盖 roll，
// 兼容旧配置（无 cov 字段时 roll 覆盖、其余不覆盖）。
inline bool covers(ConfigManager& cfg, const std::string& cat) {
    if (cat.empty()) return false;
    json c = conf(cfg);
    if (c.contains("cov") && c["cov"].is_object()) return c["cov"].value(cat, false);
    return cat == "roll";
}

// 选润色模型：polish.model_id 指定且启用则用它，否则用首个启用模型。
inline bool pickModel(ConfigManager& cfg, ai::Model& out) {
    std::string want = conf(cfg).value("model_id", std::string());
    ai::Model first; bool haveFirst = false;
    for (auto& m : ai::models(cfg)) {
        if (!m.enabled) continue;
        if (!haveFirst) { first = m; haveFirst = true; }
        if (!want.empty() && m.id == want) { out = m; return true; }
    }
    if (want.empty() && haveFirst) { out = first; return true; }
    return false;
}

// C#78/C#81：内置提示词（前端可查看/覆盖）。数字/结构保留是硬约束——即使被改，
// preservesNumbers 校验仍会兜底回退原文。
inline std::string defaultPromptText() {
    return "你是 TRPG 跑团骰娘。请只调整原回复的语气/措辞让它更自然生动，**保持它的结构与全部信息**。"
        "【硬性要求】必须原样保留原回复里的：角色名/昵称、骰子表达式（如 D100、3d6、b/p）、等号、"
        "所有数字、大成功/大失败/成功/失败/困难/极难等结果与等级、@提及、[图片] 等方括号代码。"
        "这些一个都不能改、不能删、不能编造新数字。示例：原文「<希亚人物卡版> 掷骰: D100=73」润色后"
        "应仍然包含「<希亚人物卡版>」和「D100=73」这样的结构，只在其前后调整语气。只输出润色后的文本，不要解释。";
}
inline std::string defaultPromptRp() {
    return "你是 TRPG 跑团骰娘。在**保留原回复完整结构**的前提下，按玩家消息和人设，为这次掷骰补充"
        "一两句符合情境的 rp 描述（可放在原结构前后）。【硬性要求】必须原样保留原回复里的：角色名/昵称、"
        "骰子表达式（如 D100、3d6）、等号、所有数字、成功/失败/等级、@提及、[图片] 等方括号代码，"
        "一个都不能改、不能删、不能编造新数字。只输出最终要发到群里的文本，不要解释。";
}

// 主入口：润色一条掷骰回复。@p userMsg = 玩家原始消息（含掷骰指令/事件），
// @p replyText = 骰子生成的回复。返回润色后的文本；任何失败都返回 replyText 原文。
inline std::string polish(ConfigManager& cfg, const std::string& userMsg, const std::string& replyText) {
    if (replyText.empty()) return replyText;
    ai::Model m;
    if (!pickModel(cfg, m)) return replyText;

    DICE_LOG_INFO("[AI polish] start model={} mode={} reply_len={} user_msg_len={}", m.id, conf(cfg).value("mode","text"), replyText.size(), userMsg.size());
    json c = conf(cfg);
    std::string mode = c.value("mode", std::string("text"));
    std::string persona = c.value("persona", std::string());

    // C#81：系统提示词优先用配置里的 prompt（前端可编辑）；留空则用内置默认。
    std::string sys = c.value("prompt", std::string());
    if (sys.empty()) sys = (mode == "rp") ? defaultPromptRp() : defaultPromptText();
    if (!persona.empty()) sys += "\n人设/风格：" + persona;

    std::string user = "玩家消息：" + userMsg + "\n骰子回复：" + replyText;

    // 润色输出长度按原文放宽一些；低温度求忠实；超时 20s。
    int maxTok = (int)(replyText.size()) + 256;
    if (maxTok > 1024) maxTok = 1024;
    ai::Result r = ai::chat(cfg, m, sys, user, maxTok, 20, /*tempOverride=*/0.3);
    if (!r.ok || r.reply.empty()) { if (!r.ok) DICE_LOG_WARN("[AI polish] failed model={} error={} latency={}ms", m.id, r.error, r.latencyMs); return replyText; }
    std::string out = r.reply;
    auto b = out.find_first_not_of(" \t\r\n");
    auto e = out.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return replyText;
    out = out.substr(b, e - b + 1);
    if (out.size() >= 2 && ((out.front() == '"' && out.back() == '"') ||
                            (out.front() == '\'' && out.back() == '\''))) out = out.substr(1, out.size() - 2);
    if (out.empty()) return replyText;
    // C#78 关键：润色后若丢失/改动了原文的任何数字 → 判为破坏了骰点结果，直接发原文。
    if (!ai::preservesNumbers(replyText, out)) { DICE_LOG_WARN("[AI polish] numbers changed, falling back to original"); return replyText; }
    DICE_LOG_INFO("[AI polish] ok model={} tokens={} latency={}ms in_len={} out_len={}", m.id, r.totalTokens, r.latencyMs, replyText.size(), out.size());
    return out;
}  // polish
}  // namespace dice::aipolish