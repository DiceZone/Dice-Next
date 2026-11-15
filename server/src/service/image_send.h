#pragma once
// ─── Dice!Next — 图片发送方式────────────────────────
// WebUI 上传的图片以前直接生成 http://<访问host>/api/assets/.. 链接写进回复，
// NapCat 跑在别的设备时 localhost 链接取不到图。现在回复/模板里统一存平台中立
// 图片码 [img,file=<本地路径>]，发送时由适配器按 config dice.image_send 转成
// 平台可用形式：
//   base64  — 读本地文件转 base64://…（默认，给 NapCat 等外置协议端，跨设备通用）
//   httpurl — http://<host>/api/assets/<名>（host 默认 localhost:<端口>，可配公网）
// 内置登录与骰同机：未选 httpurl 时发绝对 file:///（OpenOPQ 可直接 open），
// 文件读不到再回退 base64。不要把相对 data/images/… 交给它当缓存名。
// 旧数据里已存的 http://..../api/assets/.. 链接同样被识别并按此重新转换。

#include "../config/config_manager.h"
#include "../config/scoped_settings.h"
#include "image_resource_service.h"

#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace dice::imgsend {

inline ConfigManager* g_cfg = nullptr;
inline void init(ConfigManager& cfg) { g_cfg = &cfg; }

inline nlohmann::json conf(const std::string& platform = {},
                           const std::string& adapterId = {}) {
    if (!g_cfg) return nlohmann::json::object();
    try {
        return scoped_settings::resolveSection(
            g_cfg->getAll(), "dice", platform, adapterId)
            .value("image_send", nlohmann::json::object());
    }
    catch (...) { return nlohmann::json::object(); }
}

/// http(s)://host/api/assets/<name> 或路径串 → 资产文件名（不是本站资产返回空）。
inline std::string assetName(const std::string& s) {
    static const std::string kSeg = "/api/assets/";
    auto p = s.find(kSeg);
    if (p == std::string::npos) return "";
    std::string name = s.substr(p + kSeg.size());
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find("..") != std::string::npos) return "";
    return name;
}

/// 读本地文件 → base64://…（失败返回空）。直接用 filesystem 路径，避免再转成
/// 相对 data/images/… 交给 OneBot/OpenOPQ（它们会当成自己的缓存目录）。
inline std::string fileToBase64(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return "";
    return "base64://" + drogon::utils::base64Encode(
        reinterpret_cast<const unsigned char*>(bytes.data()), (unsigned int)bytes.size());
}
inline std::string fileToBase64(const std::string& path) {
    return fileToBase64(image_resources::fromUtf8(path));
}

inline bool readableFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec
        && std::filesystem::file_size(path, ec) > 0 && !ec;
}

/// 同机协议端：绝对 file:///；文件不在盘上再读成 base64（失败为空）。
inline std::string fileToUriOrBase64(const std::filesystem::path& path) {
    if (readableFile(path)) return image_resources::toFileUri(path);
    return fileToBase64(path);
}

inline std::string encodeLocal(const std::filesystem::path& path,
                               const std::string& platform = {},
                               const std::string& adapterId = {},
                               bool preferFileUri = false) {
    auto c = conf(platform, adapterId);
    if (c.value("mode", std::string("base64")) == "httpurl") {
        if (auto resources = image_resources::current()) {
            auto host = c.value("host", std::string());
            if (host.empty()) host = "localhost:" + std::to_string(g_cfg ? g_cfg->get<int>("server/port", 18088) : 18088);
            if (!host.starts_with("http://") && !host.starts_with("https://")) host = "http://" + host;
            while (!host.empty() && host.back() == '/') host.pop_back();
            try { return host + resources->share(image_resources::utf8(path.lexically_relative(resources->root()))); }
            catch (...) { /* Private caches must stay inline, never become public links. */ }
        }
    }
    if (preferFileUri) return fileToUriOrBase64(path);
    return fileToBase64(path);
}

/// 发送期解析：把图片段里的本地路径/本站资产链接转成平台可用值。
/// 远程外链、base64:// 原样放行。preferFileUri（内置登录）发绝对 file:///；
/// 否则读文件内嵌。读不到返回空——不要把相对路径交给协议端去 open。
inline std::string resolve(const std::string& file,
                           const std::string& platform = {},
                           const std::string& adapterId = {},
                           bool preferFileUri = false) {
    if (file.empty()) return file;
    if (file.rfind("base64://", 0) == 0) return file;
    const std::string local = image_resources::stripFileUri(file);
    if (auto resources = image_resources::current()) {
        if (auto path = resources->resolve(local))
            return encodeLocal(*path, platform, adapterId, preferFileUri);
        if (resources->known(local)) return {}; // Recycled aliases must not fall back to old physical copies.
    }
    std::string name;        // data/assets 内的文件名（可走 httpurl）
    std::string localPath;   // 本地实际路径
    if (local.rfind("http://", 0) == 0 || local.rfind("https://", 0) == 0) {
        name = assetName(local);
        if (name.empty()) return local;   // 真外链（QQ 图床等）原样
        localPath = "data/assets/" + name;
    } else {
        localPath = local;
        static const std::string kDir = "data/assets/";
        if (local.rfind(kDir, 0) == 0) {
            std::string rest = local.substr(kDir.size());
            if (!rest.empty() && rest.find('/') == std::string::npos) name = rest;
        }
    }
    auto c = conf(platform, adapterId);
    std::string mode = c.value("mode", std::string("base64"));
    if (mode == "httpurl" && !name.empty()) {
        std::string host = c.value("host", std::string());
        if (host.empty()) {
            int port = 18088;
            if (g_cfg) { try { port = g_cfg->get<int>("server/port", 18088); } catch (...) {} }
            host = "localhost:" + std::to_string(port);
        }
        if (host.rfind("http://", 0) != 0 && host.rfind("https://", 0) != 0) host = "http://" + host;
        while (!host.empty() && host.back() == '/') host.pop_back();
        return host + "/api/assets/" + name;
    }
    const auto path = image_resources::fromUtf8(localPath);
    if (preferFileUri) return fileToUriOrBase64(path);
    return fileToBase64(path);
}

}  // namespace dice::imgsend
