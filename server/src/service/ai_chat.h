#pragma once
// ─── Dice!Next — AI 对话回复（智能化阶段 A）────────────────────────
// 让骰娘能根据群里的上下文自然聊天。触发（参考 aiplugin4 Received）：被 @骰娘 / 命中触发
// 关键词 / 概率待机。用「人设 + 近期对话上下文（chat.db 最近 N 轮）」请求大模型生成回复。
// 强限频（每群冷却）避免刷屏；最大字数截断；失败/未配置/未触发一律不回复。
//
// 配置 dice/ai.chat：
//   { enabled, persona, prompt(系统提示词，空=内置默认),
//     at_bot(被@时回复), keywords[](含则回复), standby_prob(0-100 非触发也按概率回),
//     decision(待机搭话前先问「决策」用途的模型要不要插话),
//     context_rounds(喂给AI的上下文轮数), max_chars(回复上限), cooldown_sec(每群冷却),
//     reply_at(是否@发送者) }；模型由用途分配的「回复」「决策」决定。

#include "../config/config_manager.h"
#include "ai_usage.h"
#include "ai_tools.h"
#include "ai_conversation_turns.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <random>
#include <ctime>
#include <array>
#include <chrono>
#include <deque>

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
                    inner == "图片" || inner.rfind("图片:", 0) == 0) o += "[\xe5\x9b\xbe\xe7\x89\x87]";  // [图片]
                else if (inner.rfind("CQ:face", 0) == 0) o += "[\xe8\xa1\xa8\xe6\x83\x85]";   // [表情]
                else if (inner.rfind("CQ:record", 0) == 0) o += "[\xe8\xaf\xad\xe9\x9f\xb3]"; // [语音]
                else if (inner.rfind("CQ:video", 0) == 0) o += "[\xe8\xa7\x86\xe9\xa2\x91]";  // [视频]
                else if (inner.rfind("CQ:at", 0) == 0) o += "@";
                else if (inner.rfind("CQ:", 0) != 0) o.append(s, i, e - i + 1);
                i = e + 1; continue;
            }
        }
        o += s[i++];
    }
    return o;
}

// 用户自定义过滤词（dice/ai.chat.filters，字符串数组）。喂给 AI 前把这些内容
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
    return "你是这个跑团群的骰娘。按设定的人设和群友聊天，没设定时就像一个平实、熟悉的团友。"
        "先接对方这句话想说的事：吐槽可以接一句，追问要承接前情，具体问题就给具体答案。"
        "用聊天时会说出口的话，只发回复本身。别替别人编经历，也不用每次都追问。";
}

// 去除输出里的 emoji / 常见表情符号（默认骰娘对话不发 emoji，即使模型硬要发也过滤掉）。
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

// 触发种类 —— None(不回) / Strong(被@或命中关键词，进入对话、无视冷却) /
// Standby(非@非关键词时，按 standby_prob 概率待机搭话，受冷却限制)。
enum class Trigger { None, Strong, Standby };
inline Trigger triggerKind(ConfigManager& cfg, const std::string& text, bool atMe, bool allowStandby = true) {
    json c = conf(cfg);
    if (atMe && c.value("at_bot", true)) { DICE_LOG_DEBUG_M("AI 对话", "触发判断：被 @，进入对话。"); return Trigger::Strong; }
    if (c.contains("keywords") && c["keywords"].is_array())
        for (auto& kw : c["keywords"])
            if (kw.is_string() && !kw.get<std::string>().empty() && text.find(kw.get<std::string>()) != std::string::npos) {
                DICE_LOG_DEBUG_M("AI 对话", "触发判断：命中关键词「{}」，进入对话。", kw.get<std::string>());
                return Trigger::Strong;
            }
    if (!allowStandby) return Trigger::None;
    int prob = c.value("standby_prob", 0);
    if (prob > 0) {
        static std::mutex rngMutex;
        static std::mt19937 rng{std::random_device{}()};
        int roll;
        { std::lock_guard<std::mutex> lk(rngMutex); roll = (int)(rng() % 100); }
        bool hit = roll < prob;
        DICE_LOG_DEBUG_M("AI 对话", "触发判断：待机搭话掷出 {}，{}概率 {}，{}。", roll, hit ? "低于" : "不低于", prob, hit ? "进入待机搭话" : "不回复");
        if (hit) return Trigger::Standby;
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

struct HistoryLine {
    std::string sender, text;
    bool self;
    bool quoted = false;
    std::string image;
    bool supplement = false;
};
using History = std::vector<HistoryLine>;

// Keep only the observations already paid for. The database still decides whether
// a message is visible (scope, recall and history settings) before any lookup.
class RecentImageObservations {
public:
    using Key = std::array<std::string, 4>; // platform, adapter, group, message
    void remember(Key key, const std::string& description) {
        if (key.back().empty() || description.empty()) return;
        std::lock_guard lock(mutex_);
        std::erase_if(entries_, [&](const Entry& e) { return e.key == key; });
        if (entries_.size() == 256) entries_.pop_front();
        entries_.push_back({std::move(key), truncateChars(description, 480), Clock::now()});
    }
    std::string find(const Key& key) const {
        if (key.back().empty()) return {};
        std::lock_guard lock(mutex_);
        for (auto i = entries_.rbegin(); i != entries_.rend(); ++i) {
            if (Clock::now() - i->time >= std::chrono::hours(1)) break;
            if (i->key == key) return i->description;
        }
        return {};
    }
private:
    using Clock = std::chrono::steady_clock;
    struct Entry { Key key; std::string description; Clock::time_point time; };
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
};
inline RecentImageObservations& recentImages() {
    static RecentImageObservations images;
    return images;
}

inline bool refersToEarlierImage(const std::string& text) {
    for (const char* cue : {"刚才的图", "刚才那张", "上张图", "那张图", "这张图", "图里", "图上",
                            "截图里", "截图上", "剛才的圖", "剛才那張", "上張圖", "那張圖", "這張圖",
                            "圖裡", "圖上", "截圖裡", "截圖上", "that image", "that screenshot", "previous image"})
        if (text.find(cue) != std::string::npos) return true;
    return false;
}

// Keep the speaker's words distinct from machine observations for both decision and reply.
inline std::string conversationTurn(const History& context, const std::string& sender,
                                    const std::string& text, const std::string& image) {
    json turn = {{"当前发言人", sender}, {"当前发言", text}};
    // Spend the transcript budget newest-first, but present the retained turns chronologically.
    constexpr size_t historyBytes = 16000;
    size_t remaining = historyBytes;
    std::vector<json> retained, supplements;
    for (auto it = context.rbegin(); it != context.rend(); ++it) {
        const auto& line = *it;
        if (!line.quoted && !line.supplement && remaining == 0) continue;
        auto content = truncateChars(line.text, line.quoted ? 1500 : 800);
        if (content.size() < line.text.size()) content += "…";
        json item = {{"发言人", truncateChars(line.sender, 64)}, {"本账号发言", line.self}, {"内容", content}};
        if (!line.image.empty()) item["当时的识图观察（可能有误）"] = line.image;
        if (line.supplement) supplements.push_back(std::move(item));
        else if (line.quoted && line.sender.empty() && line.text.empty())
            turn["当前发言引用的原消息"] = nullptr;
        else if (line.quoted) turn["当前发言引用的原消息"] = std::move(item);
        else {
            const size_t bytes = item.dump(-1, ' ', false, json::error_handler_t::replace).size();
            if (bytes > remaining) remaining = 0;
            else {
                retained.push_back(std::move(item));
                remaining -= bytes;
            }
        }
    }
    for (auto it = retained.rbegin(); it != retained.rend(); ++it)
        turn["此前对话（从早到晚）"].push_back(std::move(*it));
    for (auto it = supplements.rbegin(); it != supplements.rend(); ++it)
        turn["当前发言之后的补充（从早到晚）"].push_back(std::move(*it));
    if (!image.empty()) turn["当前附件的识图观察（可能有误）"] = image;
    return turn.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Short references need a subject for embedding. A new topic uses its own words.
inline std::string recallQuery(const std::string& text, const History& recent) {
    for (const auto& line : recent) {
        if (line.quoted && line.sender.empty() && line.text.empty()) return text;
        if (line.quoted)
            return text + "\n" + truncateChars(line.sender, 32) + "：" + truncateChars(line.text, 144)
                + (line.image.empty() ? "" : "\n" + truncateChars(line.image, 144));
    }
    if (refersToEarlierImage(text)) {
        for (auto it = recent.rbegin(); it != recent.rend(); ++it)
            if (!it->image.empty()) return text + "\n" + truncateChars(it->image, 144);
    }
    if (recent.empty() || text.empty() || truncateChars(text, 32) != text) return text;
    for (const char* cue : {"刚才", "刚刚", "上次", "之前", "后来", "那个", "那件", "这个", "接着", "然后呢", "继续"}) {
        if (text.find(cue) == std::string::npos) continue;
        std::string query = text;
        const size_t start = recent.size() > 2 ? recent.size() - 2 : 0;
        for (size_t i = start; i < recent.size(); ++i)
            query += "\n" + truncateChars(recent[i].sender, 32) + "：" + truncateChars(recent[i].text, 144);
        return query;
    }
    return text;
}

// 生成一条对话回复。@p contextBlock = 按时间顺序排列、带发言人和本账号标记的近期对话，
// @p curSender/@p curText = 当前这条消息。返回回复文本；任何失败返回空（=不回复）。
// @p systemOverride 非空时用它作系统提示词基底（阶段E：NPC 用自己的人设覆盖骰娘）。
// @p modelOverride 非空时优先用该 id 的模型（NPC 可指定专属模型）。
inline std::string generate(ConfigManager& cfg, const History& contextBlock,
                            const std::string& curSender, const std::string& curText,
                            const std::string& background = "",
                            const aitools::ToolExec& toolExec = nullptr,
                            const std::string& systemOverride = "",
                            const std::string& modelOverride = "",
                            const std::string& knowledge = "", const std::string& image = "", const std::string& audience = "") {
    bool isNpc = !systemOverride.empty();
    const std::string usage = isNpc ? "npc" : "reply";
    ai::PurposeScope purpose(usage);
    // 先静默检查有没有可用模型；真正选模型（含日志）在下面调用时进行。
    if (ai::pick(cfg, usage, modelOverride, false).empty()) { DICE_LOG_DEBUG_M("AI 对话", "用途「{}」没有可用模型，不回复。", usage); return ""; }
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
    sys += "\n这一轮可以包含多位群友连续补充的发言，按先后顺序理解整段，针对仍未解决的话题只接一次。只输出要发给对方的话。有明确引用时先接被引用的发言；原消息缺失、或先前的识图观察没有所问细节时不要补写，必要时请对方重发。历史图片的观察只说明当时读到的内容。先辨认谁在对谁说话，接住当前发言。图片可以帮助理解玩笑、问题或场景；"
           "只有对方要你描述或读图时才逐项说明。需要看图才答得出、但看不清时，说清缺了什么；其余情况正常接话。"
           "旧记忆和资料只在和本轮有关时使用，若玩家刚纠正过，以最新说法为准。";
    if (isNpc) sys += "按角色知道的信息、与对方的关系及上一轮行动接戏，把玩家的行动和台词留给玩家。";
    std::string user;
    if (!background.empty()) user += "过往记忆（供参考）：\n" + background + "\n";
    if (!knowledge.empty()) user += "相关参考资料：\n" + knowledge + "\n";
    user += conversationTurn(contextBlock, curSender, curText, image);
    if (!audience.empty()) user += "\n本轮回复对象：" + audience;

    int maxChars = c.value("max_chars", 200);
    int maxTok = maxChars * 2 + 64;
    std::string reply;
    if (toolExec && aitools::enabled(cfg)) {
        // 阶段D：带工具的对话（模型可掷骰/抽牌/查卡），最后据结果自然回复。
        reply = aitools::runChat(cfg, ai::pick(cfg, usage, modelOverride), sys, user, maxTok, 30, toolExec);   // NPC 专属模型排在最前
    } else {
        ai::Result r = ai::chatFor(cfg, usage, sys, user, maxTok, 25, -1, modelOverride);   // 温度用全局 params
        if (r.ok) reply = r.reply;
    }
    if (reply.empty()) { DICE_LOG_INFO_M("AI 对话", "{} 模型未给出可发送回复，本次不回复。", ai::traceContext()); return ""; }
    if (noEmoji(cfg)) reply = stripEmoji(reply);   // 默认不发 emoji
    if (reply.empty()) return "";
    std::string out = truncateChars(reply, maxChars);
    if (out.size() < reply.size()) DICE_LOG_DEBUG_M("AI 对话", "回复超过 {} 字，已截断。", maxChars);
    DICE_LOG_INFO_M("AI 对话", "{} 生成回复：{}", ai::traceContext(), out);
    return out;
}

// 决策：待机搭话（非 @、非关键词）前，先用「决策」用途的模型判断此刻要不要插话。
// 只输出很短的 JSON，比直接生成回复便宜。模型不可用或输出无法解析时视为「不插话」。
inline bool decisionEnabled(ConfigManager& cfg) { return conf(cfg).value("decision", false); }
inline std::string defaultDecisionPrompt() {
    return "判断这轮群聊适合现在接话、等等还是保持安静。多人补充同一个话题时一起理解。"
        "有人还在列举、铺陈或明确说没说完，选择 wait；问题完整或玩家把行动交给角色，选择 reply。"
        "群友在相互交谈、换了无关的话题或已经自行解决，选择 skip。图片本身不等于提问。"
        "只输出 JSON，action 为 reply、wait 或 skip，reason 用一句短话说明。";
}
inline TurnAction parseTurnAction(const std::string& text, TurnAction fallback) {
    const auto begin = text.find('{'), end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) return fallback;
    auto j = json::parse(text.substr(begin, end - begin + 1), nullptr, false);
    if (!j.is_object()) return fallback;
    const auto a = j.find("action");
    if (a != j.end()) {
        if (*a == "reply") return TurnAction::Reply;
        if (*a == "wait") return TurnAction::Wait;
        if (*a == "skip") return TurnAction::Skip;
    }
    const auto r = j.find("reply");
    return r != j.end() && r->is_boolean() ? (r->get<bool>() ? TurnAction::Reply : TurnAction::Skip) : fallback;
}
struct TurnFocus {
    size_t target = 1; // 0 = room, 1 = anchor, 2... = visible supplements
    std::optional<std::string> recall; // empty = skip, absent = previous retrieval behaviour
};

inline TurnFocus parseFocus(const std::string& reply, size_t speakers) {
    TurnFocus focus;
    auto first = reply.find('{'), last = reply.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first) return focus;
    auto data = json::parse(reply.substr(first, last - first + 1), nullptr, false);
    if (!data.is_object()) return focus;
    auto target = data.find("target");
    if (target != data.end() && target->is_number_unsigned() && target->get<uint64_t>() <= speakers)
        focus.target = target->get<size_t>();
    auto query = data.find("memory_query");
    if (query != data.end() && query->is_string()) {
        auto value = query->get<std::string>();
        auto begin = value.find_first_not_of(" \t\r\n");
        focus.recall = begin == std::string::npos ? "" : truncateChars(value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1), 160);
    }
    return focus;
}

inline TurnAction planTurn(ConfigManager& cfg, const History& context, const std::string& sender,
                           const std::string& text, const std::string& image, const std::string& role,
                           bool canWait, bool addressed, const std::string& model = "", TurnFocus* focus = nullptr) {
    std::string sys = defaultDecisionPrompt() + "\n你代表：" + (role.empty() ? "骰娘" : role);
    sys += addressed ? "。本轮有人主动找你。" : "。根据上下文判断是否还在和你对话。";
    if (!canWait) sys += " 等待窗口已结束，只能 reply 或 skip。";
    if (focus) sys += " JSON 同时给出 target：回答全群填0，首条发言填1，后续补充依次从2编号。"
        "memory_query 为需要检索的过往事实或约定，当前材料已够用、寒暄或只需接话时填空字符串。";
    auto fallback = addressed ? TurnAction::Reply : TurnAction::Skip;
    const bool dedicated = decisionEnabled(cfg) && ai::hasModel(cfg, "decision");
    auto r = ai::chatFor(cfg, dedicated ? "decision" : role.empty() ? "reply" : "npc", sys,
        conversationTurn(context, sender, text, image), focus ? 220 : 120, 15, 0.1, dedicated ? "" : model);
    if (focus && r.ok) *focus = parseFocus(r.reply, 1 + std::count_if(context.begin(), context.end(),
        [](const auto& line) { return line.supplement; }));
    auto action = r.ok ? parseTurnAction(r.reply, fallback) : fallback;
    if (action == TurnAction::Wait && !canWait) action = TurnAction::Skip;
    DICE_LOG_INFO_M("AI 决策", "{} 本轮{}；{}", ai::traceContext(), action == TurnAction::Reply ? "接话" :
        action == TurnAction::Wait ? "等待补充" : "保持安静", r.ok ? r.reply : r.error);
    return action;
}
inline bool decide(ConfigManager& cfg, const History& ctx, const std::string& sender,
                   const std::string& text, const std::string& image = "") {
    return planTurn(cfg, ctx, sender, text, image, "", false, false) == TurnAction::Reply;
}

}  // namespace dice::aichat
