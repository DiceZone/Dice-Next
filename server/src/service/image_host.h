#pragma once
// ─── Dice!Next — 图床（稳定图片 URL）─────────────────────────────
// QQ 等平台的图片链接是临时的（几小时失效），日志若直接引用就会显示不出来。
// 本模块把「已落地的本地图片」上传到稳定图床，拿到长期可访问的 URL；生成 txt /
// 上传到 log 站时用这些稳定 URL 替换原始临时链接。**绝不把 base64 塞进 log 站**。
//
// 三种模式（config dice.image_host.mode）：
//   none    — 不上传（仅本地落地，HTML 导出可 base64 内嵌，但 log 站无稳定图）
//   generic — 通用第三方图床：multipart POST 上传 → 从 JSON 取 URL。配 url/file_field/
//             headers[]/result_path(点路径)/extra_fields{}，可适配 ImgBB/sm.ms/兰空/Chevereto
//   local   — 骰主自托管：bot 本地 serve 图片，配 public_base（需公网 HTTPS 可达）
//
// 存储约定：images 列存「稳定 http URL」(generic 成功) 或「本地文件名」(local/none/失败)。

#include "../config/config_manager.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <cstdio>
#include <array>

namespace dice::imghost {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    try { return cfg.get<json>("dice/image_host", json::object()); } catch (...) { return json::object(); }
}
inline std::string mode(ConfigManager& cfg) { return conf(cfg).value("mode", std::string("none")); }
inline std::string publicBase(ConfigManager& cfg) { return conf(cfg).value("public_base", std::string()); }

// 本地落地文件 data/logs/images/<file> 的对外 URL（local 模式，需配 public_base）。
inline std::string publicUrlFor(ConfigManager& cfg, const std::string& filename) {
    std::string base = publicBase(cfg);
    if (base.empty()) return "";
    if (base.back() == '/') base.pop_back();
    return base + "/api/logs/images/" + filename;
}
// images 列里的 ref（http 稳定 url 或 本地文件名）→ 对外可访问 URL（无法对外则空）。
inline std::string resolveRef(ConfigManager& cfg, const std::string& ref) {
    if (ref.rfind("http", 0) == 0) return ref;
    // C#57：骰娘回复里的本地资产（[img,file=data/assets/..]）→ /api/assets/ 路径。
    std::string norm = ref; for (auto& ch : norm) if (ch == '\\') ch = '/';
    if (norm.rfind("data/assets/", 0) == 0) {
        std::string base = publicBase(cfg);
        if (base.empty()) return "";
        if (base.back() == '/') base.pop_back();
        return base + "/api/assets/" + norm.substr(std::string("data/assets/").size());
    }
    return publicUrlFor(cfg, ref);
}

inline std::string runCapture(const std::string& cmd) {
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    std::string out; std::array<char, 4096> buf; size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

// 沿点路径取 JSON 值（"data.links.url"）→ 字符串。
inline std::string jsonByPath(const json& j, const std::string& path) {
    const json* cur = &j;
    size_t p = 0;
    while (true) {
        size_t dot = path.find('.', p);
        std::string key = path.substr(p, dot == std::string::npos ? std::string::npos : dot - p);
        if (!cur->is_object() || !cur->contains(key)) return "";
        cur = &(*cur)[key];
        if (dot == std::string::npos) break;
        p = dot + 1;
    }
    if (cur->is_string()) return cur->get<std::string>();
    if (cur->is_number_integer()) return std::to_string(cur->get<long long>());
    return "";
}

// generic 模式：curl multipart 上传本地图片 → 稳定 url（失败返回 nullopt）。
// 值来自骰主自配（半可信），用引号传参；本地路径由我们生成（可信）。
inline std::optional<std::string> uploadGeneric(ConfigManager& cfg, const std::string& localPath) {
    auto c = conf(cfg);
    std::string url = c.value("url", std::string());
    if (url.empty()) return std::nullopt;
    std::string field = c.value("file_field", std::string("file"));
    std::string resultPath = c.value("result_path", std::string("data.url"));
    std::string cmd = "curl -s -S --max-time 30 -X POST";
    if (c.contains("headers") && c["headers"].is_array())
        for (auto& h : c["headers"]) if (h.is_string()) cmd += " -H \"" + h.get<std::string>() + "\"";
    if (c.contains("extra_fields") && c["extra_fields"].is_object())
        for (auto it = c["extra_fields"].begin(); it != c["extra_fields"].end(); ++it)
            if (it.value().is_string()) cmd += " -F \"" + it.key() + "=" + it.value().get<std::string>() + "\"";
    cmd += " -F \"" + field + "=@" + localPath + "\" \"" + url + "\"";
    std::string out = runCapture(cmd);
    if (out.empty()) return std::nullopt;
    try { auto j = json::parse(out); std::string u = jsonByPath(j, resultPath); if (!u.empty()) return u; }
    catch (...) {}
    return std::nullopt;
}

// 把日志内容里的图片标记（[图片] / [CQ:image,..]）按顺序替换为稳定 URL 的 CQ 码，
// 供 log 站渲染。解析不出对外 URL 的保留 [图片]。**只产出 URL，绝不 base64。**
inline std::string substituteImages(const std::string& content, const std::string& imagesJson, ConfigManager& cfg) {
    if (imagesJson.empty()) return content;
    json imgs;
    try { imgs = json::parse(imagesJson); } catch (...) { return content; }
    if (!imgs.is_array() || imgs.empty()) return content;
    const std::string IMG = "[\xe5\x9b\xbe\xe7\x89\x87]";   // [图片]（8 字节）
    std::string out; size_t i = 0, idx = 0;
    auto emit = [&]() {
        std::string url = (idx < imgs.size() && imgs[idx].is_string())
            ? resolveRef(cfg, imgs[idx].get<std::string>()) : "";
        ++idx;
        out += url.empty() ? IMG : ("[CQ:image,url=" + url + "]");
    };
    while (i < content.size()) {
        if (content.compare(i, IMG.size(), IMG) == 0) { emit(); i += IMG.size(); continue; }
        if (content.compare(i, 9, "[CQ:image") == 0 ||
            content.compare(i, 5, "[img,") == 0) {   // C#57 平台中立图片码
            size_t end = content.find(']', i);
            if (end != std::string::npos) { emit(); i = end + 1; continue; }
        }
        out += content[i++];
    }
    return out;
}

}  // namespace dice::imghost
