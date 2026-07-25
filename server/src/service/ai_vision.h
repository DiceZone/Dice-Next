#pragma once
// ─── Dice!Next — AI 图像识别（C#85，多模态）──────────────────────────
// 消息里带图片时，用一个「视觉模型」（OpenAI 兼容的多模态 /chat/completions，content
// 为 text+image_url 数组）识别图片内容，得到一段文字描述，注入对话，让骰娘能"看懂图"
// 并据此回应。视觉模型可与对话模型分开指定（很多场景对话用便宜模型、识图用多模态模型）。
//
// 图片来源：实时消息里 [CQ:image,url=http...] 的 rkey 还新鲜，直接把 http URL 交给视觉
// 模型拉取；若是本地图（/api/chat/images/<名> 或本地路径）则读文件转 base64 data URL。
//
// 配置 dice/ai.vision：{ enabled, model_id(空=首个启用), prompt(空=内置默认), max_images }

#include "../config/config_manager.h"
#include "ai_gateway.h"
#include "chat_image.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <ctime>
#include <functional>

namespace dice::aivision {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("vision") && a["vision"].is_object()) ? a["vision"] : json::object();
}
inline bool enabled(ConfigManager& cfg) { return ai::enabled(cfg) && conf(cfg).value("enabled", false); }
inline int maxImages(ConfigManager& cfg) { int n = conf(cfg).value("max_images", 2); return n < 1 ? 1 : (n > 4 ? 4 : n); }
inline bool passUrl(ConfigManager& cfg) { return conf(cfg).value("pass_url", true); }
inline std::string defaultPrompt() {
    return "请用一两句话客观、简洁地描述这张图片里的主要内容（人物、物体、场景、文字等）。"
        "只描述你看到的，不要展开联想。";
}

// 标准 base64 编码。
inline std::string base64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        bool two = (i + 1 < in.size());
        if (two) v |= (unsigned char)in[i + 1] << 8;
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
        o += two ? T[(v >> 6) & 63] : '='; o += '=';
    }
    return o;
}
inline std::string fileToDataUrl(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() > 8 * 1024 * 1024) return "";   // 8MB 上限
    std::string ext = chatimg::guessExt(path);
    std::string mime = (ext == ".png") ? "image/png" : (ext == ".gif") ? "image/gif"
                     : (ext == ".webp") ? "image/webp" : "image/jpeg";
    return "data:" + mime + ";base64," + base64(data);
}

// 下载远程图片并转为 base64 data URL（用于 pass_url=false 时）
inline std::string downloadToDataUrl(const std::string& url) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("data/chat/images/tmp", ec);
    std::string tmpName = "data/chat/images/tmp/" + std::to_string((long long)std::time(nullptr)) + "_" + chatimg::randToken() + ".tmp";
    if (!chatimg::download(url, tmpName)) return "";
    std::string result = fileToDataUrl(tmpName);
    fs::remove(tmpName, ec);
    return result;
}

// 从 content 里抽取可交给视觉模型的图片来源（http URL 或 base64 data URL），最多 maxN 张。
inline std::vector<std::string> extractImageSrcs(const std::string& content, int maxN, bool passUrlDirect = true) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = content.find("[CQ:image", p)) != std::string::npos && (int)out.size() < maxN) {
        size_t end = content.find(']', p); if (end == std::string::npos) break;
        std::string inner = content.substr(p, end - p + 1);
        std::string u = chatimg::fieldValue(inner, "url");
        if (u.empty()) u = chatimg::fieldValue(inner, "file");
        p = end + 1;
        if (u.empty()) continue;
        if (u.rfind("http", 0) == 0) {
            if (passUrlDirect) { out.push_back(u); continue; }
            // Download and convert to base64
            std::string downloaded = downloadToDataUrl(u);
            if (!downloaded.empty()) out.push_back(downloaded);
            continue;
        }
        // 本地图：/api/chat/images/<名> 或本地路径 → base64
        std::string path;
        auto ip = u.find("/api/chat/images/");
        if (ip != std::string::npos) path = "data/chat/images/" + u.substr(ip + 17);
        else if (fs::exists(u)) path = u;
        else if (fs::exists("data/chat/images/" + u)) path = "data/chat/images/" + u;
        if (!path.empty()) { std::string d = fileToDataUrl(path); if (!d.empty()) out.push_back(d); }
    }
    return out;
}

// ─── A2：识别结果缓存（进程内，TTL 24h，容量 500）────────────────────
// 同一张图重复出现时不再调视觉模型（省钱降延迟）。缓存键：http 图去掉查询串
//（NTQQ rkey 每次都变，路径不变）；data:/本地图按内容哈希。
inline std::string visionCacheKey(const std::vector<std::string>& srcs) {
    std::string combined;
    for (auto& s : srcs) {
        if (s.rfind("http", 0) == 0) { auto q = s.find('?'); combined += s.substr(0, q); }
        else combined += std::to_string(std::hash<std::string>{}(s));
        combined += "|";
    }
    return std::to_string(std::hash<std::string>{}(combined));
}
struct VisionCacheEntry { std::string desc; long long ts; };
inline std::mutex& visionCacheMutex() { static std::mutex m; return m; }
inline std::map<std::string, VisionCacheEntry>& visionCache() {
    static std::map<std::string, VisionCacheEntry> c; return c;
}

// 识别 content 里的图片，返回一段文字描述（供注入对话）。无图/未启用/失败返回空。
inline std::string describe(ConfigManager& cfg, const std::string& content) {
    if (!enabled(cfg)) return "";
    auto srcs = extractImageSrcs(content, maxImages(cfg), passUrl(cfg));
    if (srcs.empty()) return "";
    // A2：查缓存（24h TTL）。
    const std::string ckey = visionCacheKey(srcs);
    const long long now = (long long)std::time(nullptr);
    {
        std::lock_guard<std::mutex> lk(visionCacheMutex());
        auto it = visionCache().find(ckey);
        if (it != visionCache().end() && now - it->second.ts < 86400) return it->second.desc;
    }
    ai::Model m; bool got = false;
    std::string want = conf(cfg).value("model_id", std::string());
    for (auto& mm : ai::models(cfg)) {
        if (!mm.enabled) continue;
        if (!got) { m = mm; got = true; }
        if (!want.empty() && mm.id == want) { m = mm; got = true; break; }
    }
    if (!got) return "";
    std::string prompt = conf(cfg).value("prompt", std::string());
    if (prompt.empty()) prompt = defaultPrompt();
    json contentArr = json::array();
    contentArr.push_back({{"type", "text"}, {"text", prompt}});
    for (auto& s : srcs)
        contentArr.push_back({{"type", "image_url"}, {"image_url", {{"url", s}}}});
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", contentArr}});
    ai::ChatRawResult r = ai::chatRaw(cfg, m, messages, json(nullptr), 300, 30);
    if (!r.ok || r.content.empty()) return "";
    // A2：写缓存（超量按最旧淘汰）。
    {
        std::lock_guard<std::mutex> lk(visionCacheMutex());
        auto& c = visionCache();
        if (c.size() >= 500) {
            auto oldest = c.begin();
            for (auto it = c.begin(); it != c.end(); ++it)
                if (it->second.ts < oldest->second.ts) oldest = it;
            c.erase(oldest);
        }
        c[ckey] = {r.content, (long long)std::time(nullptr)};
    }
    return r.content;
}

}  // namespace dice::aivision
