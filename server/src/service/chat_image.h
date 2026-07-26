#pragma once
// ─── Dice!Next — 模拟聊天图片本地化────────────────────────
// 新版 NTQQ 图片资源 URL 需要 rkey 且很快失效，网页直接 <img src=QQ地址> 会 400/
// 裂开。收到消息时趁 rkey 还新鲜，把远端图片下到 data/chat/images/，chat.db 里的
// 图码改指向本站 /api/chat/images/<名>，网页永远从本骰子服务器取图。
//
// 这些缓存图遵守聊天记录保留期（chat/retention_days）：按文件名前缀的收到时间戳
// 超期即删（与 chat_messages 行同一 cutoff），并与用户上传图 data/assets 分开目录。

#include <string>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <cctype>
#include <random>

namespace dice::chatimg {

inline std::string cqUnescape(std::string s) {
    auto rep = [&](const std::string& a, const std::string& b) {
        size_t p = 0; while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
    };
    rep("&#44;", ","); rep("&#91;", "["); rep("&#93;", "]"); rep("&amp;", "&");
    return s;
}

// 取 [CQ:image,...] 段里某字段值（已反转义），到逗号或 ] 为止。
inline std::string fieldValue(const std::string& inner, const std::string& key) {
    size_t k = inner.find(key + "=");
    if (k == std::string::npos) return "";
    k += key.size() + 1;
    size_t e = inner.find(',', k);
    size_t b = inner.find(']', k);
    size_t end = (std::min)(e == std::string::npos ? inner.size() : e,
                            b == std::string::npos ? inner.size() : b);
    return cqUnescape(inner.substr(k, end - k));
}

// content 里是否含远端（http）图片码。
inline bool hasRemoteImage(const std::string& content) {
    size_t p = 0;
    while ((p = content.find("[CQ:image", p)) != std::string::npos) {
        size_t end = content.find(']', p); if (end == std::string::npos) break;
        std::string inner = content.substr(p, end - p + 1);
        std::string u = fieldValue(inner, "url"); if (u.empty()) u = fieldValue(inner, "file");
        if (u.rfind("http", 0) == 0) return true;
        p = end + 1;
    }
    return false;
}

inline std::string guessExt(const std::string& url) {
    std::string low; for (char c : url) low += (char)std::tolower((unsigned char)c);
    for (const char* e : {".png", ".gif", ".jpeg", ".jpg", ".webp", ".bmp"})
        if (low.find(e) != std::string::npos) return e;
    return ".jpg";
}

inline std::string randToken() {
    static std::mt19937 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s; for (int i = 0; i < 6; ++i) s += hex[rng() & 15]; return s;
}

// curl 下载（-K 配置文件传参，防 shell 注入）；成功返回 true。
inline bool download(const std::string& url, const std::string& outPath) {
    std::string cfgPath = outPath + ".curlcfg";
    { std::ofstream cf(cfgPath, std::ios::binary);
      cf << "url = \"" << url << "\"\noutput = \"" << outPath << "\"\n"
         << "--max-time 6\n--connect-timeout 3\n--silent\n--fail\n--location\n"; }
    std::string cmd = "curl -K \"" + cfgPath + "\"";
    int rc = std::system(cmd.c_str());
    std::error_code ec; std::filesystem::remove(cfgPath, ec);
    if (rc != 0) { std::filesystem::remove(outPath, ec); return false; }
    return std::filesystem::exists(outPath, ec);
}

// 把 content 里的远端图码下载到本地并改写为 [CQ:image,file=/api/chat/images/<名>]；
// 失败的单张保留原 URL（至少 rkey 未失效时还能显示）；无远端图则原样返回。
// ⚠️ 同步阻塞（含 curl）——调用方应放到后台线程，勿在消息主循环里直接调。
inline std::string localize(const std::string& content) {
    if (!hasRemoteImage(content)) return content;
    std::error_code ec; std::filesystem::create_directories("data/chat/images", ec);
    std::string out; size_t i = 0;
    while (i < content.size()) {
        if (content.compare(i, 9, "[CQ:image") == 0) {
            size_t end = content.find(']', i);
            if (end != std::string::npos) {
                std::string inner = content.substr(i, end - i + 1);
                std::string url = fieldValue(inner, "url"); if (url.empty()) url = fieldValue(inner, "file");
                if (url.rfind("http", 0) == 0) {
                    std::string name = std::to_string((long long)std::time(nullptr)) + "_" + randToken() + guessExt(url);
                    std::string path = "data/chat/images/" + name;
                    out += download(url, path) ? ("[CQ:image,file=/api/chat/images/" + name + "]") : inner;
                } else out += inner;
                i = end + 1; continue;
            }
        }
        out += content[i++];
    }
    return out;
}

// 删除 data/chat/images 下收到时间早于 cutoff 的缓存图（按文件名前缀 <epoch>_ 判断）。
inline void pruneOld(int64_t cutoff) {
    namespace fs = std::filesystem; std::error_code ec;
    if (!fs::is_directory("data/chat/images", ec)) return;
    for (const auto& e : fs::directory_iterator("data/chat/images", ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string fn = e.path().filename().string();
        long long ts = std::atoll(fn.c_str());   // 文件名前缀 = 收到时间戳
        if (ts > 0 && ts < cutoff) fs::remove(e.path(), ec);
    }
}

}  // namespace dice::chatimg
