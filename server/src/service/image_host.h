#pragma once
// ─── Dice!Next — 图床（稳定图片 URL）─────────────────────────────
// QQ 等平台的图片链接是临时的（几小时失效），日志若直接引用就会显示不出来。
// 本模块把「已落地的本地图片」上传到稳定图床，拿到长期可访问的 URL；生成 txt /
// 上传到 log 站时用这些稳定 URL 替换原始临时链接。**绝不把 base64 塞进 log 站**。
//
// 图源模式（config dice.image_host.mode）：
//   qq      — 直接记录平台原图 URL（默认），不隐式下载或转存。
//   none    — 不上传（仅本地落地，HTML 导出可 base64 内嵌，但 log 站无稳定图）
//   generic — 通用第三方图床：multipart POST 上传 → 从 JSON 取 URL。配 url/file_field/
//             headers[]/result_path(点路径)/extra_fields{}，可适配 ImgBB/sm.ms/兰空/Chevereto
//   local   — 骰主自托管：bot 本地 serve 图片，配 public_base（需公网 HTTPS 可达）
//
// 存储约定：images 列存「稳定 http URL」(generic 成功) 或「本地文件名」(local/none/失败)。

#include "../config/config_manager.h"
#include "../config/scoped_settings.h"
#include "../common/libcurl_http.h"
#include "../message/qq_official_message.h"
#include "../message/log_images.h"
#include "image_resource_service.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <cstdio>
#include <array>

namespace dice::imghost {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg, const std::string& platform = {},
                 const std::string& adapterId = {}) {
    try {
        return scoped_settings::resolveSection(cfg.getAll(), "dice", platform, adapterId)
            .value("image_host", json::object());
    } catch (...) { return json::object(); }
}
inline std::string mode(ConfigManager& cfg, const std::string& platform = {},
                        const std::string& adapterId = {}) {
    return conf(cfg, platform, adapterId).value("mode", std::string("qq"));
}
inline std::string publicBase(ConfigManager& cfg, const std::string& platform = {},
                              const std::string& adapterId = {}) {
    return conf(cfg, platform, adapterId).value("public_base", std::string());
}

// 本地落地文件 data/logs/images/<file> 的对外 URL（local 模式，需配 public_base）。
inline std::string publicUrlFor(ConfigManager& cfg, const std::string& filename,
                                const std::string& platform = {}, const std::string& adapterId = {}) {
    std::string base = publicBase(cfg, platform, adapterId);
    if (base.empty()) return "";
    if (base.back() == '/') base.pop_back();
    return base + "/api/logs/images/" + filename;
}
// images 列里的 ref（http 稳定 url 或 本地文件名）→ 对外可访问 URL（无法对外则空）。
inline std::string resolveRef(ConfigManager& cfg, const std::string& ref,
                              const std::string& platform = {}, const std::string& adapterId = {}) {
    if (ref.empty()) return "";
    if (auto resources = image_resources::current()) if (auto path = resources->resolve(ref)) {
        auto base = publicBase(cfg, platform, adapterId);
        if (base.empty()) return "";
        if (base.back() == '/') base.pop_back();
        const auto relative = image_resources::utf8(path->lexically_relative(resources->root()));
        if (relative.starts_with("cache/log/")) return base + "/api/logs/images/" + image_resources::utf8(path->filename());
        if (relative.starts_with("cache/")) return "";
        return base + resources->share(relative);
    }
    if (ref.rfind("http", 0) == 0) return ref;
    // 骰娘回复里的本地资产（[img,file=data/assets/..]）→ /api/assets/ 路径。
    std::string norm = ref; for (auto& ch : norm) if (ch == '\\') ch = '/';
    if (norm.rfind("data/assets/", 0) == 0) {
        std::string base = publicBase(cfg, platform, adapterId);
        if (base.empty()) return "";
        if (base.back() == '/') base.pop_back();
        return base + "/api/assets/" + norm.substr(std::string("data/assets/").size());
    }
    return publicUrlFor(cfg, ref, platform, adapterId);
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

// Build multipart arguments separately so presets can be verified without
// uploading private images or requiring third-party credentials.
inline std::vector<std::string> genericUploadArgs(const json& c, const std::string& localPath) {
    std::string url = c.value("url", std::string());
    if (url.empty()) return {};
    std::string field = c.value("file_field", std::string("file"));
    std::vector<std::string> args{"-s", "-S", "--max-time", "30", "-X", "POST"};
    if (c.contains("headers") && c["headers"].is_array())
        for (auto& h : c["headers"]) if (h.is_string()) { args.push_back("-H"); args.push_back(h.get<std::string>()); }
    const auto provider = c.value("provider", std::string());
    const auto key = c.value("api_key", std::string());
    if (key.find_first_of("\r\n") != std::string::npos) return {};
    if (provider == "imgbb" && !key.empty()) { args.push_back("--form-string"); args.push_back("key=" + key); }
    if (provider == "lsky") {
        args.push_back("-H"); args.push_back("Accept: application/json");
        if (!key.empty()) { args.push_back("-H"); args.push_back("Authorization: " + (key.starts_with("Bearer ") ? key : "Bearer " + key)); }
    }
    if (provider == "chevereto" && !key.empty()) { args.push_back("-H"); args.push_back("X-API-Key: " + key); }
    if (c.contains("extra_fields") && c["extra_fields"].is_object())
        for (auto it = c["extra_fields"].begin(); it != c["extra_fields"].end(); ++it)
            if (it.value().is_string()) { args.push_back("--form-string"); args.push_back(it.key() + "=" + it.value().get<std::string>()); }
    args.push_back("-F"); args.push_back(field + "=@" + localPath); args.push_back(url);
    return args;
}

inline std::optional<std::string> uploadGeneric(ConfigManager& cfg, const std::string& localPath,
                                                const std::string& platform = {}, const std::string& adapterId = {}) {
    const auto c = conf(cfg, platform, adapterId);
    if (genericUploadArgs(c, localPath).empty()) return std::nullopt;
    dice::http::Fetch req;
    req.url = c.value("url", std::string());
    req.method = "POST";
    req.timeoutSec = 30;
    req.allowHttp = true;
    if (c.contains("headers") && c["headers"].is_array())
        for (auto& h : c["headers"]) if (h.is_string()) {
            const auto line = h.get<std::string>();
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
            req.headers.emplace_back(std::move(name), std::move(value));
        }
    const auto provider = c.value("provider", std::string());
    const auto key = c.value("api_key", std::string());
    if (provider == "lsky") {
        req.headers.emplace_back("Accept", "application/json");
        if (!key.empty()) req.headers.emplace_back("Authorization", key.starts_with("Bearer ") ? key : "Bearer " + key);
    }
    if (provider == "chevereto" && !key.empty()) req.headers.emplace_back("X-API-Key", key);
    if (provider == "imgbb" && !key.empty()) req.form.push_back({"key", key, {}, {}, {}});
    if (c.contains("extra_fields") && c["extra_fields"].is_object())
        for (auto it = c["extra_fields"].begin(); it != c["extra_fields"].end(); ++it)
            if (it.value().is_string()) req.form.push_back({it.key(), it.value().get<std::string>(), {}, {}, {}});
    req.form.push_back({c.value("file_field", std::string("file")), {}, localPath, {}, {}});
    const auto run = dice::http::fetch(req);
    if (run.body.empty()) return std::nullopt;
    const auto resultPath = c.value("result_path", std::string("data.url"));
    try { auto j = json::parse(run.body); std::string u = qqmsg::imageUrl(jsonByPath(j, resultPath)); if (!u.empty()) return u; }
    catch (...) {}
    return std::nullopt;
}

// 把日志内容里的图片标记（[图片] / [CQ:image,..]）按顺序替换为稳定 URL 的 CQ 码，
// 供 log 站渲染。解析不出对外 URL 的保留 [图片]。**只产出 URL，绝不 base64。**
inline std::string substituteImages(const std::string& content, const std::string& imagesJson, ConfigManager* cfg) {
    if (imagesJson.empty()) return content;
    json imgs;
    try { imgs = json::parse(imagesJson); } catch (...) { return content; }
    if (!imgs.is_array() || imgs.empty()) return content;
    std::string out;
    size_t last = 0, idx = 0;
    for (const auto& image : logimages::scan(content)) {
        out += content.substr(last, image.start - last);
        // A URL already in the transcript is the source selected at recording
        // time. Never replace it with a cache filename or a different host.
        std::string url = qqmsg::imageUrl(image.ref);
        if (url.empty() && idx < imgs.size() && imgs[idx].is_string()) {
            const auto ref = imgs[idx].get<std::string>();
            url = cfg ? resolveRef(*cfg, ref) : qqmsg::imageUrl(ref);
            url = qqmsg::imageUrl(url);
        }
        out += url.empty() ? "[图片]" : qqmsg::imageCode(url);
        ++idx; last = image.start + image.length;
    }
    out += content.substr(last);
    return out;
}

inline std::string substituteImages(const std::string& content, const std::string& imagesJson, ConfigManager& cfg) {
    return substituteImages(content, imagesJson, &cfg);
}

}  // namespace dice::imghost
