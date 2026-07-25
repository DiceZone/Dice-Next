#pragma once
// ─── Dice!Next — AI 工具调用（智能化阶段D，agentic）──────────────────────
// 让骰娘用 OpenAI function-calling 真的操作骰子功能，而不只是编文本：
//   · roll_dice(expression, reason)  投掷骰子表达式（走真实骰子引擎）
//   · draw_deck(name)                从牌堆抽一张
//   · get_attr(name)                 读取发送者人物卡的一个属性
// 工具的「定义」在这里（schema），「执行」由 main.cpp 注入的 toolExec 回调完成（它能访问
// 骰子引擎 / 牌堆 / 人物卡）。对话时若模型返回 tool_calls，就执行并把结果回喂，最多循环
// max_rounds 轮，最后让模型据工具结果生成自然语言回复。
//
// 配置 dice/ai.tools：{ enabled, roll_dice, draw_deck, get_attr, max_rounds }

#include "../config/config_manager.h"
#include "ai_gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>

namespace dice::aitools {

using json = nlohmann::json;
// 工具执行回调：入参 (工具名, 参数对象) → 返回给模型的结果文本。
using ToolExec = std::function<std::string(const std::string&, const json&)>;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("tools") && a["tools"].is_object()) ? a["tools"] : json::object();
}
inline bool enabled(ConfigManager& cfg) { return ai::enabled(cfg) && conf(cfg).value("enabled", false); }
inline int maxRounds(ConfigManager& cfg) { int n = conf(cfg).value("max_rounds", 3); return n < 1 ? 1 : (n > 6 ? 6 : n); }

// 按配置开关生成启用的工具 schema 数组（OpenAI tools 格式）。全关则返回空数组。
inline json toolDefs(ConfigManager& cfg) {
    json c = conf(cfg);
    json arr = json::array();
    if (c.value("roll_dice", true)) arr.push_back({{"type", "function"}, {"function", {
        {"name", "roll_dice"},
        {"description", "投掷骰子表达式并返回结果。任何需要随机、检定、伤害、属性生成时都用它，不要自己编造点数。例如 d20、3d6+2、d100、4d6k3。"},
        {"parameters", {{"type", "object"}, {"properties", {
            {"expression", {{"type", "string"}, {"description", "骰子表达式，如 d100、2d6+3、4d6k3"}}},
            {"reason", {{"type", "string"}, {"description", "投骰原因，可选，如 力量检定"}}}
        }}, {"required", json::array({"expression"})}}}
    }}});
    if (c.value("draw_deck", true)) arr.push_back({{"type", "function"}, {"function", {
        {"name", "draw_deck"},
        {"description", "从指定名称的牌堆里随机抽取一张并返回其内容（如姓名、事件、灵感等）。"},
        {"parameters", {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}, {"description", "牌堆名称"}}}
        }}, {"required", json::array({"name"})}}}
    }}});
    if (c.value("get_attr", true)) arr.push_back({{"type", "function"}, {"function", {
        {"name", "get_attr"},
        {"description", "读取当前发送者人物卡里的一个属性值（如 力量、理智/san、hp、闪避）。不知道数值时用它查，别瞎猜。"},
        {"parameters", {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}, {"description", "属性名，如 力量、理智、HP、侦查"}}}
        }}, {"required", json::array({"name"})}}}
    }}});
    // AI 深化：写卡工具（默认关，写操作有风险）。只改发送者本人卡。
    if (c.value("set_attr", false)) arr.push_back({{"type", "function"}, {"function", {
        {"name", "set_attr"},
        {"description", "修改当前发送者人物卡的一个属性。value 可以是绝对值（如 60）或相对增减（如 +3、-5，用于扣血/回理智等）。只在用户明确要求改自己卡时用，不要擅自改。"},
        {"parameters", {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}, {"description", "属性名，如 力量、生命值、理智、hp"}}},
            {"value", {{"type", "string"}, {"description", "绝对值如 60，或相对增减如 +3 / -5"}}}
        }}, {"required", json::array({"name", "value"})}}}
    }}});
    // C#83：执行任意骰子指令（含插件），把「所有指令」暴露给 AI 作为动作。
    if (c.value("run_command", true)) arr.push_back({{"type", "function"}, {"function", {
        {"name", "run_command"},
        {"description", "执行一条骰子机器人指令并返回结果。当用户想用掷骰以外的功能（查询、检定、人物卡 .st、抽签/塔罗、jrrp、日程等任意指令/插件）时，把对应指令交给它执行，然后把结果自然转述给用户。指令可带或不带前缀。"},
        {"parameters", {{"type", "object"}, {"properties", {
            {"command", {{"type", "string"}, {"description", "完整指令，如 .st 力量60、.jrrp、.draw 塔罗、r 2d6+3"}}}
        }}, {"required", json::array({"command"})}}}
    }}});
    // C#83：在帮助文档里搜索相关词条 —— 用户问「怎么用/有没有X功能」时。
    if (c.value("search_help", true)) arr.push_back({{"type", "function"}, {"function", {
        {"name", "search_help"},
        {"description", "在帮助文档里搜索与关键词相关的指令/词条并返回。用户问某功能怎么用、有没有某个功能、某指令是什么时用它，再据结果解释。"},
        {"parameters", {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}, {"description", "要搜索的关键词，如 先攻、人物卡、暗骰"}}}
        }}, {"required", json::array({"query"})}}}
    }}});
    return arr;
}

// 工具调用循环：给定系统/用户提示，若模型请求工具则执行并回喂，最多 maxRounds 轮，最后
// 返回自然语言回复。任何失败返回空。maxTok 用于每次生成上限。
inline std::string runChat(ConfigManager& cfg, const ai::Model& m, const std::string& systemPrompt,
                           const std::string& userPrompt, int maxTok, int timeoutSec, const ToolExec& exec) {
    json tools = toolDefs(cfg);
    json messages = json::array();
    if (!systemPrompt.empty()) messages.push_back({{"role", "system"}, {"content", systemPrompt}});
    messages.push_back({{"role", "user"}, {"content", userPrompt}});

    int rounds = maxRounds(cfg);
    for (int i = 0; i <= rounds; ++i) {
        // 最后一轮强制不带工具，逼模型据已有工具结果给出文本回复。
        bool last = (i == rounds);
        ai::ChatRawResult r = ai::chatRaw(cfg, m, messages, last ? json::array() : tools, maxTok, timeoutSec);
        if (!r.ok) return "";
        if (r.toolCalls.empty() || last) return r.content;   // 无工具请求 → 最终回复
        messages.push_back(r.assistantMsg);                  // 助手的 tool_calls 轮
        for (auto& tc : r.toolCalls) {
            json args = json::object();
            try { if (!tc.argsJson.empty()) args = json::parse(tc.argsJson); } catch (...) {}
            std::string result;
            try { result = exec ? exec(tc.name, args) : std::string("(工具不可用)"); } catch (...) { result = "(工具执行出错)"; }
            if (result.empty()) result = "(无结果)";
            messages.push_back({{"role", "tool"}, {"tool_call_id", tc.id}, {"content", result}});
        }
    }
    return "";
}

}  // namespace dice::aitools
