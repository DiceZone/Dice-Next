#pragma once
// ─── Dice!Next — AI 网关（阶段一）──────────────────────────
// OpenAI 兼容协议（/v1/chat/completions）的统一调用层。复用 {api} 的「curl 配置文件
// 传参、防注入、带超时」安全模式，但**独立门控** dice/ai.enabled，且**不阻止私网地址**
// ——因为骰主常用本地 AI（Ollama / LM Studio 等，跑在 localhost/局域网）。仅骰主经网页
// 后台配置模型，SSRF 风险低。
//
// 配置 dice/ai：
//   { "enabled": false,
//     "models": [ { id,name,base_url,api_key,model,enabled,
//                   price_in,price_out (每百万 token 美元价), token_limit,cost_limit (0=不限),
//                   used_tokens,used_cost (累计用量，网关自动累加) } ] }

#include "../common/logger.h"
#include "../common/subprocess.h"
#include "../config/config_manager.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <mutex>
#include <cmath>
#include <map>

namespace dice::ai {

using json = nlohmann::json;

// 提供商：一份 Base URL + API Key，供多个模型共用。
struct Provider {
    std::string id, name, baseUrl, apiKey;
    bool enabled = true;
};

struct Model {
    std::string id, name, baseUrl, apiKey, model;
    std::string providerId;              // 空 = 旧格式，自带 base_url/api_key
    std::string kind = "chat";           // chat（通用）| embedding（嵌入）
    bool vision = false;                 // 通用模型是否支持图片输入
    bool enabled = true;
    double priceIn = 0, priceOut = 0;    // 每百万 token 的美元单价
    long long tokenLimit = 0;            // 0 = 不限
    double costLimit = 0;                // 0 = 不限
    long long usedTokens = 0;
    double usedCost = 0;
};

struct Result {
    bool ok = false;
    std::string reply, error;
    std::string modelId;                 // 实际使用的模型
    long long promptTokens = 0, completionTokens = 0, totalTokens = 0;
    double cost = 0;
    long long latencyMs = 0;
};

inline json conf(ConfigManager& cfg) {
    try { return cfg.get<json>("dice/ai", json::object()); } catch (...) { return json::object(); }
}
inline bool enabled(ConfigManager& cfg) {
    json c = conf(cfg); return c.is_object() && c.value("enabled", false);
}

inline Model modelFromJson(const json& j) {
    Model m;
    m.id = j.value("id", std::string());
    m.name = j.value("name", std::string());
    m.baseUrl = j.value("base_url", std::string());
    m.apiKey = j.value("api_key", std::string());
    m.model = j.value("model", std::string());
    m.providerId = j.value("provider_id", std::string());
    m.kind = j.value("kind", std::string("chat")) == "embedding" ? "embedding" : "chat";
    m.vision = j.value("vision", false);
    m.enabled = j.value("enabled", true);
    m.priceIn = j.value("price_in", 0.0);
    m.priceOut = j.value("price_out", 0.0);
    m.tokenLimit = j.value("token_limit", (long long)0);
    m.costLimit = j.value("cost_limit", 0.0);
    m.usedTokens = j.value("used_tokens", (long long)0);
    m.usedCost = j.value("used_cost", 0.0);
    return m;
}
inline Provider providerFromJson(const json& j) {
    Provider p;
    p.id = j.value("id", std::string());
    p.name = j.value("name", std::string());
    p.baseUrl = j.value("base_url", std::string());
    p.apiKey = j.value("api_key", std::string());
    p.enabled = j.value("enabled", true);
    return p;
}
inline std::vector<Provider> providersOf(const json& c) {
    std::vector<Provider> v;
    if (c.is_object() && c.contains("providers") && c["providers"].is_array())
        for (auto& pj : c["providers"]) if (pj.is_object()) v.push_back(providerFromJson(pj));
    return v;
}
// 模型的连接信息取自所属提供商；提供商停用时模型也视为停用。
inline void resolveProvider(Model& m, const std::vector<Provider>& providers) {
    if (m.providerId.empty()) return;
    for (auto& p : providers) {
        if (p.id != m.providerId) continue;
        m.baseUrl = p.baseUrl; m.apiKey = p.apiKey;
        if (!p.enabled) m.enabled = false;
        return;
    }
    m.baseUrl.clear(); m.apiKey.clear(); m.enabled = false;   // 所属提供商已删除
}
inline std::vector<Model> modelsOf(const json& c) {
    std::vector<Model> v;
    auto providers = providersOf(c);
    if (c.is_object() && c.contains("models") && c["models"].is_array())
        for (auto& mj : c["models"]) if (mj.is_object()) {
            Model m = modelFromJson(mj);
            resolveProvider(m, providers);
            v.push_back(std::move(m));
        }
    return v;
}
inline std::vector<Model> models(ConfigManager& cfg) { return modelsOf(conf(cfg)); }

// 全局请求参数（temperature/top_p/max_tokens/penalties），可在 AI 页调。
inline json params(ConfigManager& cfg) {
    json c = conf(cfg);
    return (c.is_object() && c.contains("params") && c["params"].is_object()) ? c["params"] : json::object();
}

// 数字保留校验 —— AI 润色/翻译绝不能改动或丢失骰点/结果数字。
inline std::vector<std::string> extractNumbers(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c >= '0' && c <= '9') cur += c; else if (!cur.empty()) { v.push_back(cur); cur.clear(); } }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
// out 是否按出现次数保留了 orig 里的所有数字（多重集包含）。原文无数字则恒真。
inline bool preservesNumbers(const std::string& orig, const std::string& out) {
    auto on = extractNumbers(orig);
    if (on.empty()) return true;
    std::map<std::string, int> need, have;
    for (auto& n : on) need[n]++;
    for (auto& n : extractNumbers(out)) have[n]++;
    for (auto& kv : need) if (have[kv.first] < kv.second) return false;
    return true;
}

// Visual action hints are executable copy-paste commands. AI post-processing
// may translate their labels, but must never rewrite the command itself.
inline std::vector<std::string> extractActionCommands(const std::string& text) {
    std::vector<std::string> commands;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const size_t label = line.find("  // ");
        if (label != std::string::npos) {
            std::string command = line.substr(0, label);
            const size_t begin = command.find_first_not_of(" \t");
            const size_t last = command.find_last_not_of(" \t");
            if (begin != std::string::npos && (command[begin] == '.' || command[begin] == '!'
                || command[begin] == '/' || command.compare(begin, std::string("。").size(), "。") == 0))
                commands.push_back(command.substr(begin, last - begin + 1));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return commands;
}

inline bool preservesActionCommands(const std::string& original, const std::string& output) {
    for (const auto& command : extractActionCommands(original))
        if (output.find(command) == std::string::npos) return false;
    return true;
}

// 把某模型的用量累加并保存（读-改-写加锁，避免并发调用时互相覆盖）。
inline std::mutex& usageMutex() { static std::mutex m; return m; }
inline void addUsage(ConfigManager& cfg, const std::string& modelId, long long tokens, double cost) {
    std::lock_guard<std::mutex> lk(usageMutex());
    try {
        json c = conf(cfg);
        if (!c.is_object() || !c.contains("models") || !c["models"].is_array()) return;
        for (auto& mj : c["models"]) {
            if (mj.is_object() && mj.value("id", std::string()) == modelId) {
                mj["used_tokens"] = mj.value("used_tokens", (long long)0) + tokens;
                mj["used_cost"] = mj.value("used_cost", 0.0) + cost;
                break;
            }
        }
        cfg.set<json>("dice/ai", c);
        cfg.save();
    } catch (...) {}
}

// curl POST JSON（配置文件传参防注入）。返回 body，status 带回 HTTP 码（0=失败）。
inline std::string httpPostJson(const std::string& url, const std::string& apiKey,
                                const std::string& body, int timeoutSec, int& status) {
    namespace fs = std::filesystem;
    status = 0;
    auto esc = [](const std::string& s) { std::string o; o.reserve(s.size() + 8);
        for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; };
    static std::atomic<long long> seq{0};
    long long id = ++seq;
    fs::path tmp = fs::temp_directory_path();
    fs::path cfgF = tmp / ("dnai_" + std::to_string(id) + ".cfg");
    fs::path bodyF = tmp / ("dnai_" + std::to_string(id) + ".body");
    std::error_code ec;
    try {
        { std::ofstream bf(bodyF, std::ios::binary); bf << body; }
        std::ofstream cf(cfgF, std::ios::binary);
        cf << "url = \"" << esc(url) << "\"\n";
        cf << "request = \"POST\"\n";
        cf << "max-time = " << timeoutSec << "\n";
        cf << "silent\nshow-error\nlocation\n";
        cf << "proto = \"=http,https\"\n";
        cf << "max-filesize = 4194304\n";                 // 4 MB 上限
        cf << "header = \"Content-Type: application/json\"\n";
        if (!apiKey.empty()) cf << "header = \"Authorization: Bearer " << esc(apiKey) << "\"\n";
        cf << "data-binary = \"@" << esc(bodyF.string()) << "\"\n";
        cf << "write-out = \"\\n%{http_code}\"\n";
    } catch (...) { fs::remove(cfgF, ec); fs::remove(bodyF, ec); return ""; }

    // curl runs directly, never through a shell: see common/subprocess.h.
    std::string out = dice::proc::curlConfig(cfgF).output;
    fs::remove(cfgF, ec); fs::remove(bodyF, ec);
    auto nl = out.find_last_of('\n');
    if (nl != std::string::npos) { status = std::atoi(out.c_str() + nl + 1); out.erase(nl); }
    return out;
}

// base_url → 完整的 chat/completions 端点（用户填到 /v1 即可，自动补路径）。
inline void applyProviderRequestCompatibility(const std::string& baseUrl, json& body) {
    // Official Claude endpoint: omit sampling overrides across model generations.
    if (baseUrl == "https://api.anthropic.com" || baseUrl.rfind("https://api.anthropic.com/", 0) == 0) {
        body.erase("temperature"); body.erase("top_p");
        body.erase("frequency_penalty"); body.erase("presence_penalty");
    }
}

inline std::string endpoint(const std::string& baseUrl) {
    std::string u = baseUrl;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.find("/chat/completions") != std::string::npos) return u;
    return u + "/chat/completions";
}

// 清洗大模型输出中的不可见控制符。部分上游文本会使用 \f(0x0C) 分段，模型复现后会在
// 回复里塞 \f/\v；QQ 客户端显示成奇怪字符。把 \f/\v（真实字节或字面量转义）转成换行，
// 删掉其它控制符，并折叠多余空行。
inline std::string sanitize(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x0C || c == 0x0B) { o += '\n'; continue; }                 // FF/VT 字节 → 换行
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == 'f' || s[i + 1] == 'v')) { o += '\n'; ++i; continue; }  // 字面量 \f/\v
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') continue;       // 其它控制符删除
        if (c == 0x7F) continue;
        o += (char)c;
    }
    // 折叠 3+ 连续换行为 2 个，并去掉行尾空格。
    std::string r; int nl = 0;
    for (char c : o) {
        if (c == '\r') continue;
        if (c == '\n') { if (++nl <= 2) r += '\n'; continue; }
        nl = 0; r += c;
    }
    // 去首尾空白
    auto b = r.find_first_not_of(" \t\n");
    auto e = r.find_last_not_of(" \t\n");
    return (b == std::string::npos) ? std::string() : r.substr(b, e - b + 1);
}

// ─── 调试日志 ─────────────────────────────────────────────
// debug 级别记录完整请求、思考过程与回复，便于骰主排查。正文含群聊内容与人物卡，
// 只在骰主主动调到 debug 时写入；内嵌图片由日志层替换为占位，API Key 不在请求体里。
inline bool debugOn() { return dice::shouldRecordLog(dice::LogLevel::Debug); }

// 当前调用所属用途（由 usage 层设置），用于日志标注。
inline std::string& currentPurpose() { thread_local std::string p; return p; }
inline std::string& traceContext() { static thread_local std::string value; return value; }
struct TraceScope {
    std::string previous;
    explicit TraceScope(std::string value) : previous(traceContext()) { traceContext() = std::move(value); }
    ~TraceScope() { traceContext() = previous; }
};
struct PurposeScope {
    std::string prev;
    explicit PurposeScope(const std::string& p) : prev(currentPurpose()) { currentPurpose() = p; }
    ~PurposeScope() { currentPurpose() = prev; }
};
inline long long nextRequestNo() { static std::atomic<long long> n{0}; return ++n; }
inline std::string requestTag(long long no) {
    std::string t = "AI#" + std::to_string(no);
    if (!currentPurpose().empty()) t += " " + currentPurpose();
    return t;
}

// 单条日志上限约 16KB，长文按 UTF-8 边界切段写入。
inline void debugBlock(const std::string& title, const std::string& text) {
    if (!debugOn()) return;
    constexpr size_t chunk = 12000;
    if (text.size() <= chunk) { DICE_LOG_DEBUG_M("AI 服务", "{}\n{}", title, text); return; }
    std::vector<std::string> parts;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = std::min(text.size(), at + chunk);
        while (end < text.size() && end > at && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        parts.push_back(text.substr(at, end - at));
        at = end;
    }
    for (size_t i = 0; i < parts.size(); ++i)
        DICE_LOG_DEBUG_M("AI 服务", "{}（第 {}/{} 段）\n{}", title, i + 1, parts.size(), parts[i]);
}

// 把 messages 数组整理成便于阅读的文本。
inline std::string formatMessages(const json& msgs) {
    std::string out;
    if (!msgs.is_array()) return out;
    for (auto& m : msgs) {
        if (!m.is_object()) continue;
        std::string role = m.value("role", std::string("?"));
        out += "【" + role + "】";
        if (m.contains("tool_call_id") && m["tool_call_id"].is_string()) out += "（" + m["tool_call_id"].get<std::string>() + "）";
        out += "\n";
        if (m.contains("content")) {
            const auto& c = m["content"];
            if (c.is_string()) out += c.get<std::string>() + "\n";
            else if (c.is_array()) for (auto& part : c) {
                if (!part.is_object()) continue;
                std::string ty = part.value("type", std::string());
                if (ty == "text") out += part.value("text", std::string()) + "\n";
                else if (ty == "image_url") {
                    std::string u = part.contains("image_url") && part["image_url"].is_object()
                        ? part["image_url"].value("url", std::string()) : std::string();
                    out += "[图片] " + u + "\n";
                } else out += "[" + ty + "]\n";
            }
        }
        if (m.contains("tool_calls") && m["tool_calls"].is_array())
            for (auto& tc : m["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function") || !tc["function"].is_object()) continue;
                auto& f = tc["function"];
                std::string args = f.contains("arguments") ? (f["arguments"].is_string() ? f["arguments"].get<std::string>() : f["arguments"].dump()) : std::string();
                out += "调用工具 " + f.value("name", std::string()) + " " + args + "\n";
            }
    }
    return out;
}

// 请求参数（不含 messages/tools 正文）。
inline std::string formatParams(const json& body) {
    json p = json::object();
    for (auto& [k, v] : body.items()) if (k != "messages" && k != "tools" && k != "input") p[k] = v;
    if (body.contains("tools") && body["tools"].is_array()) {
        json names = json::array();
        for (auto& t : body["tools"]) if (t.contains("function")) names.push_back(t["function"].value("name", std::string()));
        p["tools"] = names;
    }
    return p.dump();
}

inline void logRequest(long long no, const Model& m, const std::string& url, const json& body) {
    if (!debugOn()) return;
    std::string head = "[" + requestTag(no) + "] 请求 → 模型「" + (m.name.empty() ? m.model : m.name) + "」(" + m.model + ") " + url;
    std::string text = "参数：" + formatParams(body);
    if (body.contains("messages")) text += "\n" + formatMessages(body["messages"]);
    debugBlock(head, text);
}

// 从回复里拆出思考过程：reasoning_content / reasoning 字段，或正文中的 <think>…</think>。
// 正文里的 <think> 段会被移除，不发给用户。
inline std::string takeReasoning(const json& msg, std::string& content) {
    std::string think;
    for (const char* k : {"reasoning_content", "reasoning"})
        if (msg.contains(k) && msg[k].is_string() && !msg[k].get<std::string>().empty()) { think = msg[k].get<std::string>(); break; }
    for (;;) {
        auto b = content.find("<think>");
        if (b == std::string::npos) break;
        auto e = content.find("</think>", b);
        std::string inner = content.substr(b + 7, e == std::string::npos ? std::string::npos : e - b - 7);
        if (!think.empty()) think += "\n";
        think += inner;
        content.erase(b, e == std::string::npos ? std::string::npos : e + 8 - b);
    }
    return think;
}

inline void logResponse(long long no, int status, long long latencyMs, const std::string& think,
                        const std::string& content, const json& toolCalls, long long pt, long long ct) {
    if (!debugOn()) return;
    std::string head = "[" + requestTag(no) + "] 回复 ← HTTP " + std::to_string(status) + "，耗时 "
        + std::to_string(latencyMs) + " 毫秒，token " + std::to_string(pt) + "+" + std::to_string(ct);
    std::string text;
    if (!think.empty()) text += "【思考过程】\n" + think + "\n";
    text += "【回复】\n" + (content.empty() ? std::string("（空）") : content) + "\n";
    if (toolCalls.is_array() && !toolCalls.empty()) {
        text += "【请求调用工具】\n";
        for (auto& tc : toolCalls) {
            if (!tc.is_object() || !tc.contains("function")) continue;
            auto& f = tc["function"];
            text += f.value("name", std::string()) + " " + (f.contains("arguments") ? (f["arguments"].is_string() ? f["arguments"].get<std::string>() : f["arguments"].dump()) : std::string()) + "\n";
        }
    }
    debugBlock(head, text);
}
inline void logRawFailure(long long no, int status, const std::string& resp) {
    if (!debugOn()) return;
    debugBlock("[" + requestTag(no) + "] 失败 ← HTTP " + std::to_string(status) + "，原始响应", resp.empty() ? std::string("（空）") : resp);
}

// 正在处理中的请求数（负载均衡用）。
inline std::mutex& inflightMutex() { static std::mutex m; return m; }
inline std::map<std::string, int>& inflightMap() { static std::map<std::string, int> m; return m; }
inline int inflight(const std::string& id) {
    std::lock_guard<std::mutex> lk(inflightMutex());
    auto it = inflightMap().find(id);
    return it == inflightMap().end() ? 0 : it->second;
}
struct InflightGuard {
    std::string id;
    explicit InflightGuard(std::string i) : id(std::move(i)) { std::lock_guard<std::mutex> lk(inflightMutex()); ++inflightMap()[id]; }
    ~InflightGuard() { std::lock_guard<std::mutex> lk(inflightMutex()); if (--inflightMap()[id] <= 0) inflightMap().erase(id); }
};

// 主调用：给定模型 + 系统/用户提示 → 结果（含用量/费用/延迟），并累计用量写回 config。
inline Result chat(ConfigManager& cfg, const Model& m,
                   const std::string& systemPrompt, const std::string& userPrompt,
                   int maxTokens = 512, int timeoutSec = 30, double tempOverride = -1) {
    Result r;
    r.modelId = m.id;
    if (m.baseUrl.empty() || m.model.empty()) { r.error = "model not configured (base_url/model)"; DICE_LOG_WARN("[AI] model not configured: id={}", m.id); return r; }
    if (m.tokenLimit > 0 && m.usedTokens >= m.tokenLimit) { r.error = "token limit reached"; DICE_LOG_WARN("[AI] token limit reached: model={} used={}/{}", m.id, m.usedTokens, m.tokenLimit); return r; }
    if (m.costLimit > 0 && m.usedCost >= m.costLimit) { r.error = "cost limit reached"; DICE_LOG_WARN("[AI] cost limit reached: model={} used={:.6f}/{:.6f}", m.id, m.usedCost, m.costLimit); return r; }

    json msgs = json::array();
    if (!systemPrompt.empty()) msgs.push_back({{"role", "system"}, {"content", systemPrompt}});
    msgs.push_back({{"role", "user"}, {"content", userPrompt}});
    // 请求参数从 dice/ai.params 读取；调用方可覆盖 max_tokens / temperature。
    json p = params(cfg);
    double temp = (tempOverride >= 0) ? tempOverride : p.value("temperature", 1.0);
    int mt = (maxTokens > 0) ? maxTokens : p.value("max_tokens", 1024);
    json body = {{"model", m.model}, {"messages", msgs}, {"max_tokens", mt}, {"temperature", temp}};
    body["top_p"] = p.value("top_p", 1.0);
    if (p.contains("frequency_penalty")) body["frequency_penalty"] = p.value("frequency_penalty", 0.0);
    if (p.contains("presence_penalty"))  body["presence_penalty"]  = p.value("presence_penalty", 0.0);

    const long long no = nextRequestNo();
    applyProviderRequestCompatibility(m.baseUrl, body);
    const std::string url = endpoint(m.baseUrl);
    logRequest(no, m, url, body);
    InflightGuard busy(m.id);
    auto t0 = std::chrono::steady_clock::now();
    int status = 0;
    std::string resp = httpPostJson(url, m.apiKey, body.dump(), timeoutSec, status);
    r.latencyMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (status < 200 || status >= 300) {
        r.error = "HTTP " + std::to_string(status);
        if (!resp.empty()) r.error += ": " + resp.substr(0, 300);
        DICE_LOG_WARN("[AI] HTTP error {}: model={} latency={}ms", status, m.model, r.latencyMs);
        logRawFailure(no, status, resp);
        return r;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) {
            r.error = (j["error"].is_object() ? j["error"].value("message", std::string("API error")) : std::string("API error"));
            DICE_LOG_WARN("[AI] API error: model={} error={}", m.model, r.error);
            logRawFailure(no, status, resp);
            return r;
        }
        std::string think;
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& c0 = j["choices"][0];
            if (c0.contains("message") && c0["message"].is_object()) {
                auto& msg = c0["message"];
                std::string content = msg.contains("content") && msg["content"].is_string() ? msg["content"].get<std::string>() : std::string();
                think = takeReasoning(msg, content);
                r.reply = sanitize(content);   // 清洗 \f 等控制符
            }
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            r.promptTokens = j["usage"].value("prompt_tokens", (long long)0);
            r.completionTokens = j["usage"].value("completion_tokens", (long long)0);
            r.totalTokens = j["usage"].value("total_tokens", r.promptTokens + r.completionTokens);
        }
        logResponse(no, status, r.latencyMs, think, r.reply, json(), r.promptTokens, r.completionTokens);
    } catch (...) { r.error = "AI 服务响应格式无效"; DICE_LOG_WARN("模型「{}」返回的响应格式无效，本次未取得可用回复。", m.model); logRawFailure(no, status, resp); return r; }

    if (r.reply.empty()) { r.error = "empty reply"; DICE_LOG_WARN("[AI] empty reply: model={}", m.model); return r; }
    r.cost = (r.promptTokens / 1.0e6) * m.priceIn + (r.completionTokens / 1.0e6) * m.priceOut;
    r.ok = true;
    if (r.totalTokens > 0 || r.cost > 0) addUsage(cfg, m.id, r.totalTokens, r.cost);
    DICE_LOG_INFO("[AI] ok model={} tokens={}/{}/{} cost={:.6f} latency={}ms reply_len={}", m.model, r.promptTokens, r.completionTokens, r.totalTokens, r.cost, r.latencyMs, r.reply.size());
    return r;
}

// ─── 智能化阶段D：工具调用（function-calling）──────────────────
struct ToolCall { std::string id, name, argsJson; };
struct ChatRawResult {
    bool ok = false;
    std::string error;
    std::string content;                 // 助手文本（有 tool_calls 时可能为空）
    std::vector<ToolCall> toolCalls;
    json assistantMsg = json::object();  // 原始 assistant message（供追加进消息历史）
    long long promptTokens = 0, completionTokens = 0, totalTokens = 0;
    double cost = 0;
};

// 底层调用：直接传 messages 数组 + 可选 tools；返回文本或 tool_calls。用于工具调用循环。
inline ChatRawResult chatRaw(ConfigManager& cfg, const Model& m, const json& messages,
                             const json& tools, int maxTokens, int timeoutSec, double tempOverride = -1) {
    ChatRawResult r;
    if (m.baseUrl.empty() || m.model.empty()) { r.error = "model not configured"; return r; }
    if (m.tokenLimit > 0 && m.usedTokens >= m.tokenLimit) { r.error = "token limit reached"; return r; }
    if (m.costLimit > 0 && m.usedCost >= m.costLimit) { r.error = "cost limit reached"; return r; }
    json p = params(cfg);
    double temp = (tempOverride >= 0) ? tempOverride : p.value("temperature", 1.0);
    int mt = (maxTokens > 0) ? maxTokens : p.value("max_tokens", 1024);
    json body = {{"model", m.model}, {"messages", messages}, {"max_tokens", mt}, {"temperature", temp}};
    body["top_p"] = p.value("top_p", 1.0);
    if (tools.is_array() && !tools.empty()) { body["tools"] = tools; body["tool_choice"] = "auto"; }
    const long long no = nextRequestNo();
    applyProviderRequestCompatibility(m.baseUrl, body);
    const std::string url = endpoint(m.baseUrl);
    logRequest(no, m, url, body);
    InflightGuard busy(m.id);
    auto t0 = std::chrono::steady_clock::now();
    int status = 0;
    std::string resp = httpPostJson(url, m.apiKey, body.dump(), timeoutSec, status);
    long long latency = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (status < 200 || status >= 300) {
        r.error = "HTTP " + std::to_string(status) + (resp.empty() ? "" : ": " + resp.substr(0, 200));
        DICE_LOG_WARN("[AI] HTTP error {}: model={} latency={}ms", status, m.model, latency);
        logRawFailure(no, status, resp);
        return r;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) { r.error = j["error"].is_object() ? j["error"].value("message", std::string("API error")) : std::string("API error"); logRawFailure(no, status, resp); return r; }
        std::string think;
        json rawCalls;
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& c0 = j["choices"][0];
            if (c0.contains("message") && c0["message"].is_object()) {
                r.assistantMsg = c0["message"];
                auto& msg = c0["message"];
                std::string content = msg.contains("content") && msg["content"].is_string() ? msg["content"].get<std::string>() : std::string();
                think = takeReasoning(msg, content);
                r.content = sanitize(content);
                // 思考过程不回传：部分服务商（如 DeepSeek）在下一轮收到 reasoning_content 会报错。
                r.assistantMsg.erase("reasoning_content");
                r.assistantMsg.erase("reasoning");
                if (r.assistantMsg.contains("content") && r.assistantMsg["content"].is_string()) r.assistantMsg["content"] = content;
                if (msg.contains("tool_calls")) rawCalls = msg["tool_calls"];
                if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                    for (auto& tc : msg["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        ToolCall t;
                        t.id = tc.value("id", std::string());
                        if (tc.contains("function") && tc["function"].is_object()) {
                            t.name = tc["function"].value("name", std::string());
                            // arguments 是 JSON 字符串
                            if (tc["function"].contains("arguments")) {
                                auto& a = tc["function"]["arguments"];
                                t.argsJson = a.is_string() ? a.get<std::string>() : a.dump();
                            }
                        }
                        if (!t.name.empty()) r.toolCalls.push_back(std::move(t));
                    }
                }
            }
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            r.promptTokens = j["usage"].value("prompt_tokens", (long long)0);
            r.completionTokens = j["usage"].value("completion_tokens", (long long)0);
            r.totalTokens = j["usage"].value("total_tokens", r.promptTokens + r.completionTokens);
        }
        logResponse(no, status, latency, think, r.content, rawCalls, r.promptTokens, r.completionTokens);
    } catch (...) { r.error = "invalid response: " + resp.substr(0, 200); logRawFailure(no, status, resp); return r; }
    r.cost = (r.promptTokens / 1.0e6) * m.priceIn + (r.completionTokens / 1.0e6) * m.priceOut;
    r.ok = true;
    if (r.totalTokens > 0 || r.cost > 0) addUsage(cfg, m.id, r.totalTokens, r.cost);
    DICE_LOG_INFO("[AI] ok model={} tokens={}/{}/{} cost={:.6f} latency={}ms tool_calls={}", m.model, r.promptTokens, r.completionTokens, r.totalTokens, r.cost, latency, r.toolCalls.size());
    return r;
}

// ─── 智能化阶段C：Embedding（向量化）────────────────────────────
// base_url → /v1/embeddings 端点（用户填到 /v1 即可）。
inline std::string endpointEmbed(const std::string& baseUrl) {
    std::string u = baseUrl;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.find("/embeddings") != std::string::npos) return u;
    return u + "/embeddings";
}

struct EmbedResult {
    bool ok = false;
    std::string error;
    std::vector<std::vector<float>> vectors;   // 每条输入一个向量
    long long totalTokens = 0;
};

// 把一批文本向量化。model 只需 baseUrl/apiKey + embedModelName（嵌入模型名，不同于对话模型）。
inline EmbedResult embed(ConfigManager& cfg, const Model& m, const std::string& embedModelName,
                         const std::vector<std::string>& texts, int timeoutSec = 30) {
    EmbedResult r;
    if (m.baseUrl.empty() || embedModelName.empty()) { r.error = "embed model not configured"; return r; }
    if (texts.empty()) { r.ok = true; return r; }
    if (m.tokenLimit > 0 && m.usedTokens >= m.tokenLimit) { r.error = "token limit reached"; return r; }
    if (m.costLimit > 0 && m.usedCost >= m.costLimit) { r.error = "cost limit reached"; return r; }
    json body = {{"model", embedModelName}, {"input", texts}};
    const long long no = nextRequestNo();
    const std::string url = endpointEmbed(m.baseUrl);
    if (debugOn()) {
        std::string text;
        for (size_t i = 0; i < texts.size(); ++i) text += std::to_string(i + 1) + ". " + texts[i] + "\n";
        debugBlock("[" + requestTag(no) + "] 嵌入请求 → " + embedModelName + " " + url + "，共 " + std::to_string(texts.size()) + " 条", text);
    }
    InflightGuard busy(m.id);
    int status = 0;
    std::string resp = httpPostJson(url, m.apiKey, body.dump(), timeoutSec, status);
    if (status < 200 || status >= 300) {
        r.error = "HTTP " + std::to_string(status) + (resp.empty() ? "" : (": " + resp.substr(0, 200)));
        DICE_LOG_WARN("[AI] embed HTTP {} model={} err={}", status, embedModelName, resp.substr(0, 120));
        logRawFailure(no, status, resp);
        return r;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) { r.error = j["error"].is_object() ? j["error"].value("message", std::string("API error")) : std::string("API error"); return r; }
        if (j.contains("data") && j["data"].is_array()) {
            r.vectors.reserve(j["data"].size());
            for (auto& d : j["data"]) {
                std::vector<float> v;
                if (d.contains("embedding") && d["embedding"].is_array()) {
                    v.reserve(d["embedding"].size());
                    for (auto& f : d["embedding"]) if (f.is_number()) v.push_back((float)f.get<double>());
                }
                r.vectors.push_back(std::move(v));
            }
        }
        if (j.contains("usage") && j["usage"].is_object())
            r.totalTokens = j["usage"].value("total_tokens", (long long)j["usage"].value("prompt_tokens", (long long)0));
    } catch (...) { r.error = "invalid embed response: " + resp.substr(0, 160); return r; }
    if (r.vectors.size() != texts.size()) { r.error = "embed count mismatch"; return r; }
    r.ok = true;
    if (r.totalTokens > 0) addUsage(cfg, m.id, r.totalTokens, (r.totalTokens / 1.0e6) * m.priceIn);
    DICE_LOG_INFO("[AI] embed ok model={} n={} dim={} tokens={}", embedModelName, r.vectors.size(),
                  r.vectors.empty() ? 0 : r.vectors[0].size(), r.totalTokens);
    if (debugOn())
        DICE_LOG_DEBUG_M("AI 服务", "[{}] 嵌入回复 ← {} 条向量，维度 {}，token {}", requestTag(no), r.vectors.size(),
                         r.vectors.empty() ? 0 : r.vectors[0].size(), r.totalTokens);
    return r;
}
inline EmbedResult embed(ConfigManager& cfg, const Model& m, const std::vector<std::string>& texts, int timeoutSec = 30) {
    return embed(cfg, m, m.model, texts, timeoutSec);
}

// ─── 语音识别（OpenAI 兼容 /audio/transcriptions）──────────────────
inline std::string endpointTranscribe(const std::string& baseUrl) {
    std::string u = baseUrl;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.find("/audio/transcriptions") != std::string::npos) return u;
    return u + "/audio/transcriptions";
}

// 把一个本地音频文件转成文字。失败返回空，error 带原因。
inline std::string transcribe(ConfigManager& cfg, const Model& m, const std::filesystem::path& audio,
                              std::string& error, int timeoutSec = 60) {
    namespace fs = std::filesystem;
    error.clear();
    if (m.baseUrl.empty() || m.model.empty()) { error = "model not configured"; return ""; }
    if (m.tokenLimit > 0 && m.usedTokens >= m.tokenLimit) { error = "token limit reached"; return ""; }
    if (m.costLimit > 0 && m.usedCost >= m.costLimit) { error = "cost limit reached"; return ""; }
    auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; } return o; };
    static std::atomic<long long> seq{0};
    fs::path cfgF = fs::temp_directory_path() / ("dnstt_" + std::to_string(++seq) + ".cfg");
    const long long no = nextRequestNo();
    const std::string url = endpointTranscribe(m.baseUrl);
    std::error_code ec;
    try {
        std::ofstream cf(cfgF, std::ios::binary);
        cf << "url = \"" << esc(url) << "\"\n";
        cf << "max-time = " << timeoutSec << "\n";
        cf << "silent\nshow-error\nlocation\n";
        cf << "proto = \"=http,https\"\n";
        cf << "max-filesize = 4194304\n";
        if (!m.apiKey.empty()) cf << "header = \"Authorization: Bearer " << esc(m.apiKey) << "\"\n";
        cf << "form = \"file=@" << esc(audio.string()) << "\"\n";
        cf << "form-string = \"model=" << esc(m.model) << "\"\n";
        cf << "form-string = \"response_format=json\"\n";
        cf << "write-out = \"\\n%{http_code}\"\n";
    } catch (...) { fs::remove(cfgF, ec); error = "cannot write request"; return ""; }
    if (debugOn())
        DICE_LOG_DEBUG_M("AI 服务", "[{}] 语音识别请求 → 模型「{}」({}) {}，音频 {} 字节", requestTag(no),
                         m.name.empty() ? m.model : m.name, m.model, url, fs::file_size(audio, ec));
    InflightGuard busy(m.id);
    auto t0 = std::chrono::steady_clock::now();
    std::string out = dice::proc::curlConfig(cfgF).output;
    fs::remove(cfgF, ec);
    long long latency = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    int status = 0;
    auto nl = out.find_last_of('\n');
    if (nl != std::string::npos) { status = std::atoi(out.c_str() + nl + 1); out.erase(nl); }
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status) + (out.empty() ? "" : ": " + out.substr(0, 200));
        DICE_LOG_WARN("[AI] transcribe HTTP {} model={} latency={}ms", status, m.model, latency);
        logRawFailure(no, status, out);
        return "";
    }
    std::string text;
    try {
        json j = json::parse(out);
        if (j.contains("error")) { error = j["error"].is_object() ? j["error"].value("message", std::string("API error")) : std::string("API error"); logRawFailure(no, status, out); return ""; }
        text = sanitize(j.value("text", std::string()));
    } catch (...) { error = "invalid transcription response"; logRawFailure(no, status, out); return ""; }
    DICE_LOG_INFO("[AI] transcribe ok model={} latency={}ms chars={}", m.model, latency, text.size());
    if (debugOn()) debugBlock("[" + requestTag(no) + "] 语音识别结果 ← 耗时 " + std::to_string(latency) + " 毫秒", text.empty() ? std::string("（空）") : text);
    if (text.empty()) error = "empty transcription";
    return text;
}

// 余弦相似度（向量维度不同则返回 -1）。
inline float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return -1.f;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += (double)a[i] * b[i]; na += (double)a[i] * a[i]; nb += (double)b[i] * b[i]; }
    if (na <= 0 || nb <= 0) return -1.f;
    return (float)(dot / (std::sqrt(na) * std::sqrt(nb)));
}

// 向量 ↔ JSON 字符串（存进 ai_memory.embedding；紧凑存储）。
inline std::string vecToJson(const std::vector<float>& v) { json a = json::array(); for (float f : v) a.push_back(f); return a.dump(); }
inline std::vector<float> vecFromJson(const std::string& s) {
    std::vector<float> v;
    try { json a = json::parse(s); if (a.is_array()) for (auto& f : a) if (f.is_number()) v.push_back((float)f.get<double>()); } catch (...) {}
    return v;
}

}  // namespace dice::ai
