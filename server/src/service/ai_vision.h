#pragma once
// ─── Dice!Next — AI 图像识别（多模态）──────────────────────────
// 消息里带图片时，用一个「视觉模型」（OpenAI 兼容的多模态 /chat/completions，content
// 为 text+image_url 数组）识别图片内容，得到一段文字描述，注入对话，让骰娘能"看懂图"
// 并据此回应。视觉模型可与对话模型分开指定（很多场景对话用便宜模型、识图用多模态模型）。
//
// 图片来源：及时下载 [CQ:image,url=http...]，与本地缓存图一起校验格式和大小，
// 统一转为 Base64 data URL。图片链接不直接交给视觉服务。
//
// 配置 dice/ai.vision：{ enabled, prompt, max_images, max_image_kb }。

#include "../config/config_manager.h"
#include "ai_usage.h"
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
#include <openssl/evp.h>
#include "ai_vision_config.h"

namespace dice::aivision {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("vision") && a["vision"].is_object()) ? a["vision"] : json::object();
}
inline bool enabled(ConfigManager& cfg) { return ai::enabled(cfg) && conf(cfg).value("enabled", false); }
inline int maxImages(ConfigManager& cfg) { int n = conf(cfg).value("max_images", 2); return n < 1 ? 1 : (n > 4 ? 4 : n); }
inline bool passUrl(ConfigManager&) { return false; }
inline size_t maxImageBytes(ConfigManager& cfg) { return (size_t)normalizeSettings(conf(cfg))["max_image_kb"].get<int>() * 1024; }
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
inline std::string fileToDataUrl(const std::string& path, size_t maxBytes = 1024 * 1024, std::string* error = nullptr) {
    std::ifstream f(image_resources::fromUtf8(path), std::ios::binary);
    if (!f) { if (error) *error = "图片文件无法读取"; return ""; }
    f.seekg(0, std::ios::end);
    auto length = f.tellg();
    if (length <= 0 || (uint64_t)length > maxBytes) {
        if (error) *error = "图片大小 " + std::to_string((long long)length) + " 字节，超过上限 " + std::to_string(maxBytes / 1024) + " KB 或为空文件";
        return "";
    }
    f.seekg(0);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() > 8 * 1024 * 1024) return "";   // 8MB 上限
    std::string ext = chatimg::guessExt(path);
    // Downloads use .tmp names; infer MIME from bytes, not that suffix.
    if (data.size() >= 8 && data.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0) ext = ".png";
    else if (data.size() >= 6 && (data.compare(0, 6, "GIF87a") == 0 || data.compare(0, 6, "GIF89a") == 0)) ext = ".gif";
    else if (data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0) ext = ".webp";
    else if (data.size() >= 3 && (unsigned char)data[0] == 0xff && (unsigned char)data[1] == 0xd8 && (unsigned char)data[2] == 0xff) ext = ".jpg";
    else { if (error) *error = "图片格式不支持或下载内容不是图片"; return ""; }
    std::string mime = (ext == ".png") ? "image/png" : (ext == ".gif") ? "image/gif"
                     : (ext == ".webp") ? "image/webp" : "image/jpeg";
    return "data:" + mime + ";base64," + base64(data);
}

// 下载远程图片并转为 base64 data URL（用于 pass_url=false 时）
inline std::string downloadToDataUrl(const std::string& url, size_t maxBytes = 1024 * 1024, std::string* error = nullptr) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto temporary = image_resources::cacheDirectory("chat") / "tmp";
    fs::create_directories(temporary, ec);
    std::string tmpName = image_resources::utf8(temporary / (std::to_string((long long)std::time(nullptr)) + "_" + chatimg::randToken() + ".tmp"));
    int code = 0;
    if (!chatimg::download(url, tmpName, maxBytes, &code)) {
        if (error) *error = code == 63 ? "图片超过大小上限 " + std::to_string(maxBytes / 1024) + " KB，已跳过" : "图片下载失败，curl 错误码 " + std::to_string(code);
        return "";
    }
    std::string result = fileToDataUrl(tmpName, maxBytes, error);
    fs::remove(tmpName, ec);
    return result;
}

// 从 content 里抽取可交给视觉模型的图片来源（http URL 或 base64 data URL），最多 maxN 张。
inline std::vector<std::string> extractImageSrcs(const std::string& content, int maxN, bool /*passUrlDirect*/ = false, size_t maxBytes = 1024 * 1024, std::string* error = nullptr) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    size_t p = 0; int examined = 0;
    while ((p = content.find("[CQ:image", p)) != std::string::npos && examined++ < maxN) {
        size_t end = content.find(']', p); if (end == std::string::npos) break;
        std::string inner = content.substr(p, end - p + 1);
        std::string u = chatimg::fieldValue(inner, "url");
        if (u.empty()) u = chatimg::fieldValue(inner, "file");
        p = end + 1;
        if (u.empty()) continue;
        if (u.rfind("http", 0) == 0) {
            // Download and convert to base64
            std::string downloaded = downloadToDataUrl(u, maxBytes, error);
            if (!downloaded.empty()) out.push_back(downloaded);
            continue;
        }
        // 本地图：/api/chat/images/<名> 或本地路径 → base64
        std::string path;
        auto ip = u.find("/api/chat/images/");
        if (auto resources = image_resources::current(); resources && resources->resolve(u)) path = image_resources::resolvePath(u);
        else if (ip != std::string::npos) path = image_resources::utf8(image_resources::cacheDirectory("chat") / image_resources::fromUtf8(u.substr(ip + 17)));
        else if (fs::exists(u)) path = u;
        else if (fs::exists(image_resources::cacheDirectory("chat") / image_resources::fromUtf8(u))) path = image_resources::utf8(image_resources::cacheDirectory("chat") / image_resources::fromUtf8(u));
        if (path.empty() && error) *error = "消息中的图片文件未缓存或地址无法解析";
        if (!path.empty()) { std::string d = fileToDataUrl(path, maxBytes, error); if (!d.empty()) out.push_back(d); }
    }
    return out;
}

// ─── A2：识别结果缓存（进程内，TTL 24h，容量 500）────────────────────
// The full URL (including file identifiers in query parameters) is the identity.
// Preserve boundaries between images rather than joining with an ambiguous separator.
inline std::string visionCacheKey(const std::vector<std::string>& srcs) {
    const auto bytes = json(srcs).dump();
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) != 1) return bytes;
    static constexpr char hex[] = "0123456789abcdef";
    std::string key; key.reserve(size * 2);
    for (unsigned int i = 0; i < size; ++i) { key += hex[digest[i] >> 4]; key += hex[digest[i] & 15]; }
    return key;
}
struct VisionCacheEntry { std::string desc; long long ts; };
inline std::mutex& visionCacheMutex() { static std::mutex m; return m; }
inline std::map<std::string, VisionCacheEntry>& visionCache() {
    static std::map<std::string, VisionCacheEntry> c; return c;
}

// 识别 content 里的图片，返回一段文字描述（供注入对话）。无图/未启用/失败返回空。
inline std::string describe(ConfigManager& cfg, const std::string& content) {
    if (!enabled(cfg)) return "";
    std::string imageError;
    auto srcs = extractImageSrcs(content, maxImages(cfg), false, maxImageBytes(cfg), &imageError);
    if (!imageError.empty()) DICE_LOG_INFO_M("AI 识图", "{} {}", ai::traceContext(), imageError);
    if (srcs.empty()) {
        if (content.find("[CQ:image") != std::string::npos) {
            DICE_LOG_WARN_M("AI 识图", "图片无法读取：消息没有可访问的图片地址，或下载/格式校验失败。");
            return "[图片未能读取。请明确告知用户无法确认图片内容，不要猜测图中信息。]";
        }
        return "";
    }
    if (!ai::hasModel(cfg, "vision")) {
        DICE_LOG_WARN_M("AI 识图", "未配置可用的图像识别模型，请在模型用途中选择支持图片的模型。");
        return "[图片未能识别：尚未配置视觉模型。不要猜测图片内容。]";
    }
    // A2：查缓存（24h TTL）。
    std::vector<std::string> cacheParts = srcs;
    cacheParts.push_back(conf(cfg).value("prompt", std::string()));
    for (const auto& model : ai::pick(cfg, "vision", "", false)) cacheParts.push_back(model.id + ":" + model.model + ":" + model.baseUrl);
    const std::string ckey = visionCacheKey(cacheParts);
    const long long now = (long long)std::time(nullptr);
    {
        std::lock_guard<std::mutex> lk(visionCacheMutex());
        auto it = visionCache().find(ckey);
        if (it != visionCache().end() && now - it->second.ts < 86400) {
            DICE_LOG_INFO_M("AI 识图", "{} 识图结果（缓存，{} 张）：{}", ai::traceContext(), srcs.size(), it->second.desc);
            return it->second.desc + (imageError.empty() ? std::string() : "\n[另有图片未读取：" + imageError + "；不要猜测未读取的图片。]");
        }
    }
    std::string prompt = conf(cfg).value("prompt", std::string());
    if (prompt.empty()) prompt = defaultPrompt();
    json contentArr = json::array();
    contentArr.push_back({{"type", "text"}, {"text", prompt}});
    for (auto& s : srcs)
        contentArr.push_back({{"type", "image_url"}, {"image_url", {{"url", s}}}});
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", contentArr}});
    ai::ChatRawResult r = ai::chatRawFor(cfg, "vision", messages, json(nullptr), 300, 30);
    if (!r.ok || r.content.empty()) {
        DICE_LOG_WARN_M("AI 识图", "{} 图像识别失败：{}", ai::traceContext(), r.error.empty() ? "视觉模型未返回图片描述" : r.error);
        return "[图片识别失败，无法确认图片内容。请告知用户本次未能读图，不要编造图中信息。]";
    }
    DICE_LOG_INFO_M("AI 识图", "{} 识图结果（{} 张）：{}", ai::traceContext(), srcs.size(), r.content);
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
    return r.content + (imageError.empty() ? std::string() : "\n[另有图片未读取：" + imageError + "；不要猜测未读取的图片。]");
}

}  // namespace dice::aivision
