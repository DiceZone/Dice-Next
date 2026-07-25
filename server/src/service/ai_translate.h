#pragma once
// ─── Dice!Next — AI 回复翻译（C#68 阶段三）──────────────────────
// 骰主在 AI 页定义「自定义语言」（名称 + .lang 切换关键词，无需 i18n 文件）。某群/某
// 用户 .lang 切到自定义语言后：回复先按正常语言链生成，发送前经大模型翻译成目标语言。
// 范围可选（掷骰文本 / 自定义回复 / 插件结果）。同文本+同语言有内存缓存，避免重复计费。
// 失败/超时/未配置一律发原文，不影响正常使用。
//
// 配置 dice/ai.translate：
//   { enabled, model_id(空=首个启用模型),
//     scope_dice, scope_reply, scope_plugin,
//     langs: [ { name:"德语", keywords:["de","german","德语","deutsch"] } ] }

#include "../config/config_manager.h"
#include "ai_gateway.h"
#include <nlohmann/json.hpp>
#include "../common/logger.h"
#include <string>
#include <map>
#include <mutex>
#include <cctype>

namespace dice::aitrans {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("translate") && a["translate"].is_object()) ? a["translate"] : json::object();
}
inline bool enabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && conf(cfg).value("enabled", false);
}
// C#78：某回复类别是否在翻译覆盖范围内（roll/deck/fun/custom/plugin）。默认全覆盖，
// 兼容旧配置（旧的 scope_dice/reply/plugin → roll/custom/plugin）。
inline bool covers(ConfigManager& cfg, const std::string& cat) {
    if (cat.empty()) return false;
    json c = conf(cfg);
    if (c.contains("cov") && c["cov"].is_object()) return c["cov"].value(cat, false);
    // 旧字段兼容
    if (cat == "roll")   return c.value("scope_dice", true);
    if (cat == "custom") return c.value("scope_reply", true);
    if (cat == "plugin") return c.value("scope_plugin", true);
    return true;   // deck/fun 无旧字段 → 默认覆盖
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/// 用户输入（.lang 的参数）匹配某个自定义 AI 语言 → 返回其显示名；无匹配返回空。
/// 语言名本身与 keywords 都可作为切换词（不区分大小写）。
inline std::string matchLang(ConfigManager& cfg, const std::string& input) {
    std::string in = lower(input);
    if (in.empty()) return "";
    json c = conf(cfg);
    if (!c.contains("langs") || !c["langs"].is_array()) return "";
    for (auto& l : c["langs"]) {
        if (!l.is_object()) continue;
        std::string name = l.value("name", std::string());
        if (name.empty()) continue;
        if (lower(name) == in) return name;
        if (l.contains("keywords") && l["keywords"].is_array())
            for (auto& kw : l["keywords"])
                if (kw.is_string() && lower(kw.get<std::string>()) == in) return name;
    }
    return "";
}

// 选翻译模型（translate.model_id 指定且启用则用它，否则首个启用模型）。
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

// 翻译缓存（同源文本+同目标语言只计费一次）。内存缓存，超上限整体清空（简单可靠）。
inline std::mutex& cacheMutex() { static std::mutex m; return m; }
inline std::map<std::string, std::string>& cache() { static std::map<std::string, std::string> c; return c; }

// C#81：内置翻译提示词（前端可查看/覆盖）。{lang} 会替换为目标语言名。
inline std::string defaultPrompt() {
    return "你是 TRPG 跑团骰娘的翻译器。把用户给的骰娘回复翻译成「{lang}」。**原样保留所有数字、"
        "骰点表达式（如 D100=57）、成功/失败等级、@提及、[图片] 等方括号代码与占位符，不得改动"
        "数字、增删内容或解释**。只输出译文。";
}
inline std::string fillLang(std::string tpl, const std::string& lang) {
    size_t p; while ((p = tpl.find("{lang}")) != std::string::npos) tpl.replace(p, 6, lang);
    return tpl;
}

/// 把 @p text 翻译成 @p targetLang（显示名，如「德语」）。失败返回原文。
inline std::string translate(ConfigManager& cfg, const std::string& targetLang, const std::string& text) {
    if (text.empty() || targetLang.empty()) return text;
    std::string key = targetLang + "\x1f" + text;
    {
        std::lock_guard<std::mutex> lk(cacheMutex());
        auto it = cache().find(key);
        if (it != cache().end()) return it->second;
    }
    ai::Model m;
    if (!pickModel(cfg, m)) return text;
    DICE_LOG_INFO("[AI translate] start model={} lang={} text_len={}", m.id, targetLang, text.size());

    // C#81：系统提示词优先用配置里的 prompt（前端可编辑，{lang} 占位）；留空则用内置默认。
    std::string tpl = conf(cfg).value("prompt", std::string());
    if (tpl.empty()) tpl = defaultPrompt();
    std::string sys = fillLang(tpl, targetLang);
    int maxTok = (int)(text.size()) + 256;
    if (maxTok > 2048) maxTok = 2048;
    ai::Result r = ai::chat(cfg, m, sys, text, maxTok, 20, /*tempOverride=*/0.3);
    if (!r.ok || r.reply.empty()) { if (!r.ok) DICE_LOG_WARN("[AI translate] failed model={} lang={} error={}", m.id, targetLang, r.error); return text; }

    std::string out = r.reply;
    auto b = out.find_first_not_of(" \t\r\n");
    auto e = out.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return text;
    out = out.substr(b, e - b + 1);
    // C#78：译文若丢失/改动了原文数字（骰点结果）→ 判为翻译不可靠，发原文。
    if (!ai::preservesNumbers(text, out)) { DICE_LOG_WARN("[AI translate] numbers changed, falling back to original lang={}", targetLang); return text; }
    {
        std::lock_guard<std::mutex> lk(cacheMutex());
        if (cache().size() > 1000) cache().clear();
    DICE_LOG_INFO("[AI translate] ok model={} lang={} tokens={} latency={}ms in_len={} out_len={}", m.id, targetLang, r.totalTokens, r.latencyMs, text.size(), out.size());
        cache()[key] = out;
    }
    return out;
}

}  // namespace dice::aitrans
