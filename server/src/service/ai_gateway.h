#pragma once
// ─── Dice!Next — AI 网关（C#67 阶段一）──────────────────────────
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

struct Model {
    std::string id, name, baseUrl, apiKey, model;
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
    m.enabled = j.value("enabled", true);
    m.priceIn = j.value("price_in", 0.0);
    m.priceOut = j.value("price_out", 0.0);
    m.tokenLimit = j.value("token_limit", (long long)0);
    m.costLimit = j.value("cost_limit", 0.0);
    m.usedTokens = j.value("used_tokens", (long long)0);
    m.usedCost = j.value("used_cost", 0.0);
    return m;
}
inline std::vector<Model> models(ConfigManager& cfg) {
    std::vector<Model> v;
    json c = conf(cfg);
    if (c.is_object() && c.contains("models") && c["models"].is_array())
        for (auto& mj : c["models"]) if (mj.is_object()) v.push_back(modelFromJson(mj));
    return v;
}

// C#78：全局请求参数（temperature/top_p/max_tokens/penalties），可在 AI 页调。
inline json params(ConfigManager& cfg) {
    json c = conf(cfg);
    return (c.is_object() && c.contains("params") && c["params"].is_object()) ? c["params"] : json::object();
}

// C#78：数字保留校验 —— AI 润色/翻译绝不能改动或丢失骰点/结果数字。
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

    std::string cmd = "curl -K \"" + cfgF.string() + "\"";
    std::string out;
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe) {
        std::array<char, 8192> buf; size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }
    fs::remove(cfgF, ec); fs::remove(bodyF, ec);
    auto nl = out.find_last_of('\n');
    if (nl != std::string::npos) { status = std::atoi(out.c_str() + nl + 1); out.erase(nl); }
    return out;
}

// base_url → 完整的 chat/completions 端点（用户填到 /v1 即可，自动补路径）。
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

// 主调用：给定模型 + 系统/用户提示 → 结果（含用量/费用/延迟），并累计用量写回 config。
inline Result chat(ConfigManager& cfg, const Model& m,
                   const std::string& systemPrompt, const std::string& userPrompt,
                   int maxTokens = 512, int timeoutSec = 30, double tempOverride = -1) {
    Result r;
    if (m.baseUrl.empty() || m.model.empty()) { r.error = "model not configured (base_url/model)"; DICE_LOG_WARN("[AI] model not configured: id={}", m.id); return r; }
    if (m.tokenLimit > 0 && m.usedTokens >= m.tokenLimit) { r.error = "token limit reached"; DICE_LOG_WARN("[AI] token limit reached: model={} used={}/{}", m.id, m.usedTokens, m.tokenLimit); return r; }
    if (m.costLimit > 0 && m.usedCost >= m.costLimit) { r.error = "cost limit reached"; DICE_LOG_WARN("[AI] cost limit reached: model={} used={:.6f}/{:.6f}", m.id, m.usedCost, m.costLimit); return r; }

    json msgs = json::array();
    if (!systemPrompt.empty()) msgs.push_back({{"role", "system"}, {"content", systemPrompt}});
    msgs.push_back({{"role", "user"}, {"content", userPrompt}});
    // C#78：请求参数从 dice/ai.params 读取；调用方可覆盖 max_tokens / temperature。
    json p = params(cfg);
    double temp = (tempOverride >= 0) ? tempOverride : p.value("temperature", 1.0);
    int mt = (maxTokens > 0) ? maxTokens : p.value("max_tokens", 1024);
    json body = {{"model", m.model}, {"messages", msgs}, {"max_tokens", mt}, {"temperature", temp}};
    body["top_p"] = p.value("top_p", 1.0);
    if (p.contains("frequency_penalty")) body["frequency_penalty"] = p.value("frequency_penalty", 0.0);
    if (p.contains("presence_penalty"))  body["presence_penalty"]  = p.value("presence_penalty", 0.0);

    DICE_LOG_DEBUG("[AI] >>> request model={} max_tokens={} temp={}", m.model, mt, temp);
    DICE_LOG_DEBUG("[AI] >>> system: {}", systemPrompt.size() > 500 ? systemPrompt.substr(0, 500) + "..." : systemPrompt);
    DICE_LOG_DEBUG("[AI] >>> user: {}", userPrompt.size() > 500 ? userPrompt.substr(0, 500) + "..." : userPrompt);
    auto t0 = std::chrono::steady_clock::now();
    int status = 0;
    std::string resp = httpPostJson(endpoint(m.baseUrl), m.apiKey, body.dump(), timeoutSec, status);
    r.latencyMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    DICE_LOG_DEBUG("[AI] <<< HTTP {} latency={}ms body_size={}", status, r.latencyMs, resp.size());
    if (!resp.empty()) DICE_LOG_DEBUG("[AI] <<< response: {}", resp.size() > 1000 ? resp.substr(0, 1000) + "..." : resp);

    if (status < 200 || status >= 300) {
        r.error = "HTTP " + std::to_string(status);
        if (!resp.empty()) r.error += ": " + resp.substr(0, 300);
        DICE_LOG_WARN("[AI] HTTP error {}: model={} latency={}ms", status, m.model, r.latencyMs);
        return r;
    }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) {
            r.error = (j["error"].is_object() ? j["error"].value("message", std::string("API error")) : std::string("API error"));
            DICE_LOG_WARN("[AI] API error: model={} error={}", m.model, r.error);
            return r;
        }
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& c0 = j["choices"][0];
            if (c0.contains("message") && c0["message"].contains("content") && c0["message"]["content"].is_string())
                r.reply = sanitize(c0["message"]["content"].get<std::string>());   // C#79：清洗 \f 等控制符
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            r.promptTokens = j["usage"].value("prompt_tokens", (long long)0);
            r.completionTokens = j["usage"].value("completion_tokens", (long long)0);
            r.totalTokens = j["usage"].value("total_tokens", r.promptTokens + r.completionTokens);
        }
    } catch (...) { r.error = "invalid response: " + resp.substr(0, 200); DICE_LOG_WARN("[AI] invalid response: model={} resp={}", m.model, resp.substr(0, 200)); return r; }

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
    json p = params(cfg);
    double temp = (tempOverride >= 0) ? tempOverride : p.value("temperature", 1.0);
    int mt = (maxTokens > 0) ? maxTokens : p.value("max_tokens", 1024);
    json body = {{"model", m.model}, {"messages", messages}, {"max_tokens", mt}, {"temperature", temp}};
    body["top_p"] = p.value("top_p", 1.0);
    if (tools.is_array() && !tools.empty()) { body["tools"] = tools; body["tool_choice"] = "auto"; }
    int status = 0;
    std::string resp = httpPostJson(endpoint(m.baseUrl), m.apiKey, body.dump(), timeoutSec, status);
    if (status < 200 || status >= 300) { r.error = "HTTP " + std::to_string(status) + (resp.empty() ? "" : ": " + resp.substr(0, 200)); return r; }
    try {
        json j = json::parse(resp);
        if (j.contains("error")) { r.error = j["error"].is_object() ? j["error"].value("message", std::string("API error")) : std::string("API error"); return r; }
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto& c0 = j["choices"][0];
            if (c0.contains("message") && c0["message"].is_object()) {
                r.assistantMsg = c0["message"];
                auto& msg = c0["message"];
                if (msg.contains("content") && msg["content"].is_string()) r.content = sanitize(msg["content"].get<std::string>());
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
    } catch (...) { r.error = "invalid response: " + resp.substr(0, 200); return r; }
    r.cost = (r.promptTokens / 1.0e6) * m.priceIn + (r.completionTokens / 1.0e6) * m.priceOut;
    r.ok = true;
    if (r.totalTokens > 0 || r.cost > 0) addUsage(cfg, m.id, r.totalTokens, r.cost);
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
    json body = {{"model", embedModelName}, {"input", texts}};
    int status = 0;
    std::string resp = httpPostJson(endpointEmbed(m.baseUrl), m.apiKey, body.dump(), timeoutSec, status);
    if (status < 200 || status >= 300) {
        r.error = "HTTP " + std::to_string(status) + (resp.empty() ? "" : (": " + resp.substr(0, 200)));
        DICE_LOG_WARN("[AI] embed HTTP {} model={} err={}", status, embedModelName, resp.substr(0, 120));
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
    if (r.totalTokens > 0) addUsage(cfg, m.id, r.totalTokens, 0.0);
    DICE_LOG_INFO("[AI] embed ok model={} n={} dim={} tokens={}", embedModelName, r.vectors.size(),
                  r.vectors.empty() ? 0 : r.vectors[0].size(), r.totalTokens);
    return r;
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
