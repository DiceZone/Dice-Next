#pragma once
// ─── Dice!Next — 图片发送方式────────────────────────
// WebUI 上传的图片以前直接生成 http://<访问host>/api/assets/.. 链接写进回复，
// NapCat 跑在别的设备时 localhost 链接取不到图。现在回复/模板里统一存平台中立
// 图片码 [img,file=<本地路径>]，发送时由适配器按 config dice.image_send 转成
// 平台可用形式：
//   base64  — 读本地文件转 base64://…（默认，跨设备通用）
//   httpurl — http://<host>/api/assets/<名>（host 默认 localhost:<端口>，可配公网）
// 旧数据里已存的 http://..../api/assets/.. 链接同样被识别并按此重新转换。

#include "../config/config_manager.h"

#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace dice::imgsend {

inline ConfigManager* g_cfg = nullptr;
inline void init(ConfigManager& cfg) { g_cfg = &cfg; }

inline nlohmann::json conf() {
    if (!g_cfg) return nlohmann::json::object();
    try { return g_cfg->get<nlohmann::json>("dice/image_send", nlohmann::json::object()); }
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

/// 读本地文件 → base64://…（失败返回空）。中文路径走 u8string。
inline std::string fileToBase64(const std::string& path) {
    std::filesystem::path p(std::u8string(path.begin(), path.end()));
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return "";
    return "base64://" + drogon::utils::base64Encode(
        reinterpret_cast<const unsigned char*>(bytes.data()), (unsigned int)bytes.size());
}

/// 发送期解析：把图片段里的本地路径/本站资产链接转成平台可用值。
/// 远程外链、base64://、file:// 原样放行。
inline std::string resolve(const std::string& file) {
    if (file.empty()) return file;
    if (file.rfind("base64://", 0) == 0 || file.rfind("file:", 0) == 0) return file;
    std::string name;        // data/assets 内的文件名（可走 httpurl）
    std::string localPath;   // 本地实际路径（走 base64）
    if (file.rfind("http://", 0) == 0 || file.rfind("https://", 0) == 0) {
        name = assetName(file);
        if (name.empty()) return file;   // 真外链（QQ 图床等）原样
        localPath = "data/assets/" + name;
    } else {
        localPath = file;
        std::string norm = file;
        for (auto& ch : norm) if (ch == '\\') ch = '/';
        static const std::string kDir = "data/assets/";
        if (norm.rfind(kDir, 0) == 0) {
            std::string rest = norm.substr(kDir.size());
            if (!rest.empty() && rest.find('/') == std::string::npos) name = rest;
        }
    }
    auto c = conf();
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
    // base64 模式，或 httpurl 下无法定位到可服务的资产（任意本地路径）→ 内嵌。
    std::string b64 = fileToBase64(localPath);
    return b64.empty() ? file : b64;
}

}  // namespace dice::imgsend
