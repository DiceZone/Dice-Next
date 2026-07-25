#pragma once
// ─── Dice!Next — AI 对话回复（智能化阶段 A）────────────────────────
// 让骰娘能根据群里的上下文自然聊天。触发（参考 aiplugin4 Received）：被 @骰娘 / 命中触发
// 关键词 / 概率待机。用「人设 + 近期对话上下文（chat.db 最近 N 轮）」请求大模型生成回复。
// 强限频（每群冷却）避免刷屏；最大字数截断；失败/未配置/未触发一律不回复。
//
// 配置 dice/ai.chat：
//   { enabled, model_id(空=首个启用), persona, prompt(系统提示词，空=内置默认),
//     at_bot(被@时回复), keywords[](含则回复), standby_prob(0-100 非触发也按概率回),
//     context_rounds(喂给AI的上下文轮数), max_chars(回复上限), cooldown_sec(每群冷却),
//     reply_at(是否@发送者) }

#include "../config/config_manager.h"
#include "ai_gateway.h"
#include "ai_tools.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <random>
#include <ctime>

namespace dice::aichat {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("chat") && a["chat"].is_object()) ? a["chat"] : json::object();
}
inline bool enabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && conf(cfg).value("enabled", false);
}
inline int contextRounds(ConfigManager& cfg) { int n = conf(cfg).value("context_rounds", 10); return (n < 0) ? 0 : (n > 40 ? 40 : n); }
inline bool replyAt(ConfigManager& cfg) { return conf(cfg).value("reply_at", false); }

// 去掉 [CQ:..]/[img,..]/[图片:..] 等方括号码 → 供上下文纯文本（图片/表情用简短占位）。
inline std::string stripCodes(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') {
            size_t e = s.find(']', i);
            if (e != std::string::npos) {
                std::string inner = s.substr(i + 1, e - i - 1);
                if (inner.rfind("CQ:image", 0) == 0 || inner.rfind("img,", 0) == 0 ||
                    inner.rfind("\xe5\x9b\xbe", 0) == 0) o += "[\xe5\x9b\xbe\xe7\x89\x87]";  // [图片]
                else if (inner.rfind("CQ:face", 0) == 0) o += "[\xe8\xa1\xa8\xe6\x83\x85]";   // [表情]
                else if (inner.rfind("CQ:record", 0) == 0) o += "[\xe8\xaf\xad\xe9\x9f\xb3]"; // [语音]
                else if (inner.rfind("CQ:video", 0) == 0) o += "[\xe8\xa7\x86\xe9\xa2\x91]";  // [视频]
                else if (inner.rfind("CQ:at", 0) == 0) o += "@";
                // C#85：其它非标准码（reply/file/json/xml/forward 等）一律过滤丢弃
                i = e + 1; continue;
            }
        }
        o += s[i++];
    }
    return o;
}

// C#85：用户自定义过滤词（dice/ai.chat.filters，字符串数组）。喂给 AI 前把这些内容
// 从文本里删除（逐字面量出现全删）。用于滤掉水印、签名、特定占位符、刷屏词等。
inline std::vector<std::string> filters(ConfigManager& cfg) {
    std::vector<std::string> v;
    json c = conf(cfg);
    if (c.contains("filters") && c["filters"].is_array())
        for (auto& f : c["filters"])
            if (f.is_string() && !f.get<std::string>().empty()) v.push_back(f.get<std::string>());
    return v;
}
inline std::string applyFilters(ConfigManager& cfg, std::string s) {
    for (auto& f : filters(cfg)) {
        size_t p = 0;
        while ((p = s.find(f, p)) != std::string::npos) s.erase(p, f.size());
    }
    return s;
}
// stripCodes（结构性 CQ 码）+ 用户自定义过滤 → 供 AI 使用的干净文本。所有喂给 AI 的
// 消息文本（当前消息 / 上下文 / 记忆折叠）都应经过它。
inline std::string cleanForAi(ConfigManager& cfg, const std::string& raw) {
    return applyFilters(cfg, stripCodes(raw));
}

inline std::string defaultPrompt() {
    return "你是一个 TRPG 跑团 QQ 群里的骰子机器人（骰娘）。像群里一个普通、稳重的熟人那样，"
        "用简短、自然、平实的中文参与聊天。要求：不要复述别人已经说过的话，不要加「骰娘：」之类前缀，"
        "不要长篇大论，一般一两句话就够；语气自然大方，不要卖萌、不要浮夸、不要用「喵」「呢」「哦～」"
        "之类的口癖或颜文字，也不要使用任何 emoji 或表情符号；不确定或不适合回应时，简短带过即可。";
}

// C#86：去除输出里的 emoji / 常见表情符号（默认骰娘对话不发 emoji，即使模型硬要发也过滤掉）。
inline std::string stripEmoji(const std::string& s) {
    std::string o; o.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        // 解码码点
        unsigned int cp = 0;
        if (step == 1) cp = c;
        else if (step == 2 && i + 1 < s.size()) cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
        else if (step == 3 && i + 2 < s.size()) cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        else if (step == 4 && i + 3 < s.size()) cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        bool emoji =
            (cp >= 0x1F000 && cp <= 0x1FAFF) ||   // emoji 主区（含表情/符号/旗帜等）
            (cp >= 0x2600 && cp <= 0x27BF)   ||   // 杂项符号 + 装饰符号（☀✂✨等）
            (cp >= 0x2B00 && cp <= 0x2BFF)   ||   // 星形/箭头装饰
            (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||   // 区域指示符（旗帜）
            cp == 0x2764 || cp == 0x2705 || cp == 0x274C ||
            (cp >= 0xFE00 && cp <= 0xFE0F)   ||   // 变体选择符
            cp == 0x200D || cp == 0x20E3;         // ZWJ / keycap
        if (!emoji) o.append(s, i, step);
        i += step;
    }
    // 去掉因删 emoji 残留的多余空格
    std::string r; r.reserve(o.size());
    for (size_t k = 0; k < o.size(); ++k) {
        if (o[k] == ' ' && (r.empty() || r.back() == ' ')) continue;
        r += o[k];
    }
    while (!r.empty() && (r.back() == ' ' || r.back() == '\n')) r.pop_back();
    return r;
}
inline bool noEmoji(ConfigManager& cfg) { return conf(cfg).value("no_emoji", true); }

// C#87：触发种类 —— None(不回) / Strong(被@或命中关键词，必回、无视冷却) /
// Standby(非@非关键词时，按 standby_prob 概率待机搭话，受冷却限制)。
enum class Trigger { None, Strong, Standby };
inline Trigger triggerKind(ConfigManager& cfg, const std::string& text, bool atMe) {
    json c = conf(cfg);
    if (atMe && c.value("at_bot", true)) return Trigger::Strong;   // 被 @ 必回
    if (c.contains("keywords") && c["keywords"].is_array())
        for (auto& kw : c["keywords"])
            if (kw.is_string() && !kw.get<std::string>().empty() && text.find(kw.get<std::string>()) != std::string::npos)
                return Trigger::Strong;                            // 命中关键词必回
    int prob = c.value("standby_prob", 0);
    if (prob > 0) {
        static std::mt19937 rng{std::random_device{}()};
        if ((int)(rng() % 100) < prob) return Trigger::Standby;    // 非关键词时的待机搭话
    }
    return Trigger::None;
}
inline bool shouldTrigger(ConfigManager& cfg, const std::string& text, bool atMe) {
    return triggerKind(cfg, text, atMe) != Trigger::None;
}

// 每群冷却限频（消费式：命中冷却返回 false，否则更新时间戳并返回 true）。
inline bool cooldownOk(ConfigManager& cfg, const std::string& groupKey) {
    int cd = conf(cfg).value("cooldown_sec", 5);
    if (cd <= 0) return true;
    static std::mutex m; static std::map<std::string, long> last;
    std::lock_guard<std::mutex> lk(m);
    long now = (long)std::time(nullptr);
    auto it = last.find(groupKey);
    if (it != last.end() && now - it->second < cd) return false;
    last[groupKey] = now;
    return true;
}

// 按字符数（非字节）截断到 max。
inline std::string truncateChars(const std::string& s, int maxChars) {
    if (maxChars <= 0) return s;
    int cnt = 0; size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t step = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (++cnt > maxChars) break;
        i += step;
    }
    return s.substr(0, i);
}

// 生成一条对话回复。@p contextBlock = 已格式化的近期对话（"昵称: 内容" 每行一条，可空），
// @p curSender/@p curText = 当前这条消息。返回回复文本；任何失败返回空（=不回复）。
// @p systemOverride 非空时用它作系统提示词基底（阶段E：NPC 用自己的人设覆盖骰娘）。
// @p modelOverride 非空时优先用该 id 的模型（NPC 可指定专属模型）。
inline std::string generate(ConfigManager& cfg, const std::string& contextBlock,
                            const std::string& curSender, const std::string& curText,
                            const std::string& background = "",
                            const aitools::ToolExec& toolExec = nullptr,
                            const std::string& systemOverride = "",
                            const std::string& modelOverride = "") {
    ai::Model m;
    {   // 选模型：modelOverride > chat.model_id > 首个启用模型。
        std::string want = !modelOverride.empty() ? modelOverride : conf(cfg).value("model_id", std::string());
        bool got = false;
        for (auto& mm : ai::models(cfg)) {
            if (!mm.enabled) continue;
            if (!got) { m = mm; got = true; }
            if (!want.empty() && mm.id == want) { m = mm; got = true; break; }
        }
        if (!got) return "";
    }
    json c = conf(cfg);
    std::string sys;
    if (!systemOverride.empty()) {
        sys = systemOverride;   // 阶段E：NPC 人设，完全替换骰娘默认人设
    } else {
        std::string persona = c.value("persona", std::string());
        sys = c.value("prompt", std::string());
        if (sys.empty()) sys = defaultPrompt();
        if (!persona.empty()) sys += "\n人设/性格：" + persona;
    }
    // 阶段 B/C：注入群历史摘要 + 相关记忆作为背景（供参考，不要直接复述）。
    if (!background.empty())
        sys += "\n\n【群历史记忆（较早对话的摘要，作背景参考，不要直接复述）】\n" + background;

    bool isNpc = !systemOverride.empty();
    std::string user;
    if (!contextBlock.empty()) user = "群里最近的对话：\n" + contextBlock + "\n";
    user += "最新消息 —— " + curSender + "：" + curText + "\n"
          + (isNpc ? "请严格以你扮演的角色身份自然、简短地回复这条消息。"
                   : "请以骰娘身份自然、简短地回复这条消息。");

    int maxChars = c.value("max_chars", 200);
    int maxTok = maxChars * 2 + 64;
    std::string reply;
    if (toolExec && aitools::enabled(cfg)) {
        // 阶段D：带工具的对话（模型可掷骰/抽牌/查卡），最后据结果自然回复。
        reply = aitools::runChat(cfg, m, sys, user, maxTok, 30, toolExec);
    } else {
        ai::Result r = ai::chat(cfg, m, sys, user, maxTok, 25);   // 温度用全局 params
        if (r.ok) reply = r.reply;
    }
    if (reply.empty()) return "";
    if (noEmoji(cfg)) reply = stripEmoji(reply);   // C#86：默认不发 emoji
    if (reply.empty()) return "";
    return truncateChars(reply, maxChars);
}

}  // namespace dice::aichat
