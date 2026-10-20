#pragma once
// 管理后台第三方登录的 HTTP 接口。登录相关的 /providers、/start、/callback、/logo 无需会话；
// 其余配置接口需要完整权限的会话（受限管理员由前置拦截挡在外面，这里再核一次）。

#include "sso_auth.h"
#include "web_auth.h"
#include "../common/logger.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <filesystem>
#include <fstream>
#include <functional>

namespace dice::sso {

/// maxAge：>0 为持久 Cookie 的秒数，0 为浏览器会话 Cookie，<0 为立即过期。
using CookieFactory = std::function<drogon::Cookie(const std::string& name, const std::string& value, int maxAge)>;

namespace detail {

inline drogon::HttpResponsePtr jsonResponse(const json& body, drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->addHeader("Cache-Control", "no-store");
    resp->setBody(body.dump());
    return resp;
}

inline drogon::HttpResponsePtr fail(const std::string& message, drogon::HttpStatusCode code = drogon::k400BadRequest) {
    return jsonResponse({{"code", static_cast<int>(code)}, {"message", message}}, code);
}

inline drogon::HttpResponsePtr redirect(const std::string& location) {
    auto resp = drogon::HttpResponse::newRedirectionResponse(location, drogon::k302Found);
    resp->addHeader("Cache-Control", "no-store");
    resp->addHeader("Referrer-Policy", "no-referrer");
    return resp;
}

inline std::string roleAttribute(const drogon::HttpRequestPtr& req) {
    try { return req->attributes()->get<std::string>("dn_role"); } catch (...) { return {}; }
}

/// 前置拦截已验证会话并写入权限；没有写入（X-API-Key 调用）视为完整权限。
inline bool fullAccess(const drogon::HttpRequestPtr& req) {
    const auto role = roleAttribute(req);
    return role.empty() || role == "full";
}

inline std::filesystem::path logoDir(const std::string& configPath) {
    return std::filesystem::path(configPath) / "sso-logos";
}

inline std::string logoExtension(const std::string& bytes) {
    auto starts = [&](const std::string& magic) { return bytes.compare(0, magic.size(), magic) == 0; };
    if (starts("\x89PNG\r\n\x1a\n")) return "png";
    if (starts("\xFF\xD8\xFF")) return "jpg";
    if (starts("GIF87a") || starts("GIF89a")) return "gif";
    if (bytes.size() > 12 && starts("RIFF") && bytes.compare(8, 4, "WEBP") == 0) return "webp";
    return {};   // 不收 SVG：可以夹带脚本
}

inline std::string contentTypeFor(const std::string& ext) {
    if (ext == "png") return "image/png";
    if (ext == "jpg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    return "image/webp";
}

/// 登录页能看到的提供方。BDC 需已启用并至少有一把骰娘 Key；自定义提供方需已启用且有绑定。
inline json loginProviders(ConfigManager& cfg) {
    json conf = config(cfg), out = json::array();
    const auto& bdc = conf["bdc"];
    if (bdc.value("enabled", false) && !bdc.value("client_id", std::string()).empty() && !bdcApiKeys(cfg).empty())
        out.push_back({{"id", "bdc"}, {"kind", "bdc"}, {"name", "骰子之间"}, {"logo", ""}});
    for (const auto& provider : conf["providers"]) {
        if (!provider.is_object() || !provider.value("enabled", true)) continue;
        if (provider.value("bindings", json::array()).empty()) continue;
        std::string logo = provider.value("logo_url", std::string());
        if (!provider.value("logo_file", std::string()).empty())
            logo = "/api/auth/sso/logo/" + provider.value("id", std::string()) + "?v=" + std::to_string(provider.value("logo_version", 0));
        out.push_back({{"id", provider.value("id", std::string())}, {"kind", "oidc"},
                       {"name", provider.value("name", std::string())}, {"logo", logo}});
    }
    return out;
}

inline json publicProvider(const json& provider) {
    json bindings = json::array();
    for (const auto& binding : provider.value("bindings", json::array()))
        if (binding.is_object())
            bindings.push_back({{"sub", binding.value("sub", std::string())},
                                {"label", binding.value("label", std::string())},
                                {"bound_at", binding.value("bound_at", std::string())}});
    std::string logo = provider.value("logo_url", std::string());
    if (!provider.value("logo_file", std::string()).empty())
        logo = "/api/auth/sso/logo/" + provider.value("id", std::string()) + "?v=" + std::to_string(provider.value("logo_version", 0));
    return {{"id", provider.value("id", std::string())}, {"name", provider.value("name", std::string())},
            {"issuer", provider.value("issuer", std::string())}, {"client_id", provider.value("client_id", std::string())},
            {"has_secret", !provider.value("client_secret", std::string()).empty()},
            {"scopes", provider.value("scopes", std::string("openid profile email"))},
            {"enabled", provider.value("enabled", true)}, {"logo", logo},
            {"logo_url", provider.value("logo_url", std::string())},
            {"has_logo_file", !provider.value("logo_file", std::string()).empty()}, {"bindings", bindings}};
}

inline std::string nowIso() {
    const auto now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

/// 用任一可用的骰娘 Key 向账号站登记或移除回调地址。
inline HttpResult changeBdcRedirect(ConfigManager& cfg, const std::string& redirectUri, bool add) {
    HttpResult last;
    for (const auto& key : bdcApiKeys(cfg)) {
        last = httpJson(add ? "POST" : "DELETE", std::string(kBdcIssuer) + "/api/oauth/dice-panel/redirect-uris",
                        {{"X-API-Key", key}}, json{{"redirect_uri", redirectUri}}.dump());
        if (last.status == 200) return last;
        if (last.status != 401) break;   // 这把 Key 失效时换下一把，其他错误直接报
    }
    return last;
}

} // namespace detail

/// 公寓托管实例：用公开访问地址登记并启用骰子之间账号登录。骰主自己停用过（enabled=false）
/// 就不再自动打开；已登记且已启用则什么也不做。返回结果说明，供日志使用。
inline std::string ensureManagedBdcLogin(ConfigManager& cfg, const std::string& publicUrl) {
    const auto base = normalizeBase(publicUrl);
    if (!base) return "公开访问地址不是 HTTPS，跳过";
    if (bdcApiKeys(cfg).empty()) return "尚未配置骰娘 Key，跳过";
    json conf = config(cfg);
    json& bdc = conf["bdc"];
    if (bdc.contains("enabled") && !bdc.value("enabled", false)) return "骰主已停用，跳过";
    const std::string redirectUri = callbackFor(*base);
    const auto uris = bdc.value("redirect_uris", json::array());
    if (bdc.value("enabled", false) && std::find(uris.begin(), uris.end(), json(redirectUri)) != uris.end()) return "已启用";
    const auto result = detail::changeBdcRedirect(cfg, redirectUri, true);
    if (result.status != 200 || !result.body.is_object()) return "向骰子之间登记失败（HTTP " + std::to_string(result.status) + "）";
    bdc["client_id"] = result.body.value("client_id", std::string());
    bdc["redirect_uris"] = result.body.value("redirect_uris", json::array());
    bdc["enabled"] = true;
    return saveConfig(cfg, conf) ? "已启用，回调地址 " + redirectUri : "无法保存配置";
}

/// 注册全部第三方登录接口。sessionCookieName 为本实例会话 Cookie 名。
template <typename Router>
void registerRoutes(drogon::HttpAppFramework& app, ConfigManager& cfg, Router& router,
                    const std::string& configPath, const std::string& sessionCookieName,
                    CookieFactory makeCookie) {
    using drogon::HttpRequestPtr;
    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

    // 登录页的提供方列表（公开）。
    app.registerHandler("/api/auth/sso/providers", [&cfg](const HttpRequestPtr&, Callback&& cb) {
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}, {"data", detail::loginProviders(cfg)}}));
    }, {drogon::Get});

    // 发起授权：login 公开；bind 需要完整权限的会话，绑定的是骰主自己在第三方的身份。
    auto start = [&cfg, &router, sessionCookieName, makeCookie](const HttpRequestPtr& req, Callback&& cb, const std::string& purpose) {
        const std::string providerId = req->getParameter("provider");
        const auto base = normalizeBase(req->getParameter("base"));
        if (!base) { cb(detail::fail("后台地址必须是 HTTPS")); return; }
        std::string bindToken;
        if (purpose == "bind") {
            bindToken = req->getCookie(sessionCookieName);
            if (bindToken.empty() || !WebAuth::instance().validToken(bindToken) ||
                roleOf(WebAuth::instance().identityOf(bindToken), cfg, router) != Role::Full) {
                cb(detail::fail("需要以完整权限登录后再绑定", drogon::k403Forbidden)); return;
            }
            if (providerId == "bdc") { cb(detail::fail("骰子之间账号无需绑定")); return; }
        }
        json conf = config(cfg);
        std::string clientId, scope, issuer;
        const std::string redirectUri = callbackFor(*base);
        if (providerId == "bdc") {
            const auto& bdc = conf["bdc"];
            if (!bdc.value("enabled", false) || bdcApiKeys(cfg).empty()) { cb(detail::fail("未启用骰子之间账号登录")); return; }
            const auto uris = bdc.value("redirect_uris", json::array());
            if (std::find(uris.begin(), uris.end(), json(redirectUri)) == uris.end()) {
                cb(detail::fail("当前地址尚未登记为回调地址，请先用口令登录后在后台设置中登记")); return;
            }
            clientId = bdc.value("client_id", std::string()); scope = "openid email"; issuer = kBdcIssuer;
        } else {
            const json* provider = findProvider(conf, providerId);
            if (!provider || !provider->value("enabled", true)) { cb(detail::fail("登录方式不存在或已停用")); return; }
            clientId = provider->value("client_id", std::string());
            scope = provider->value("scopes", std::string("openid profile email"));
            issuer = provider->value("issuer", std::string());
        }
        const auto endpoints = discover(issuer);
        if (clientId.empty() || !endpoints) { cb(detail::fail("无法读取登录提供方的配置", drogon::k502BadGateway)); return; }
        Flow flow{providerId, randomUrlToken(48), redirectUri, *base, purpose, bindToken};
        const std::string challenge = pkceChallenge(flow.verifier);
        const std::string state = FlowStore::instance().put(flow);
        if (flow.verifier.empty() || challenge.empty() || state.empty()) { cb(detail::fail("登录请求过多，请稍后再试", drogon::k429TooManyRequests)); return; }
        std::string location = endpoints->authorization;
        location += (location.find('?') == std::string::npos ? "?" : "&");
        location += "response_type=code&client_id=" + urlEncode(clientId) + "&redirect_uri=" + urlEncode(redirectUri) +
                    "&scope=" + urlEncode(scope) + "&state=" + urlEncode(state) +
                    "&code_challenge=" + urlEncode(challenge) + "&code_challenge_method=S256";
        auto resp = detail::redirect(location);
        resp->addCookie(makeCookie(kStateCookie, state, kFlowSeconds));
        cb(resp);
    };
    app.registerHandler("/api/auth/sso/start", [start](const HttpRequestPtr& req, Callback&& cb) {
        start(req, std::move(cb), "login");
    }, {drogon::Get});
    app.registerHandler("/api/auth/sso/bind/start", [start](const HttpRequestPtr& req, Callback&& cb) {
        start(req, std::move(cb), "bind");
    }, {drogon::Get});

    // 授权回调（公开）：换令牌、取用户信息、按用途发会话或记录绑定。
    app.registerHandler("/api/auth/sso/callback",
        [&cfg, &router, sessionCookieName, makeCookie](const HttpRequestPtr& req, Callback&& cb) {
        const std::string state = req->getParameter("state");
        const auto flow = FlowStore::instance().take(state);
        if (!flow) { cb(detail::fail("登录请求已过期，请回到后台重新发起")); return; }
        auto back = [&](const std::string& query, const std::string& hash = {}) {
            auto resp = detail::redirect(flow->base + "/?" + query + hash);
            resp->addCookie(makeCookie(kStateCookie, "", -1));
            return resp;
        };
        if (req->getCookie(kStateCookie) != state) { cb(back("sso_error=state")); return; }
        if (!req->getParameter("error").empty()) { cb(back("sso_error=denied")); return; }
        const std::string code = req->getParameter("code");
        json conf = config(cfg);
        std::string clientId, clientSecret, issuer;
        if (flow->provider == "bdc") {
            clientId = conf["bdc"].value("client_id", std::string()); issuer = kBdcIssuer;
        } else if (const json* provider = findProvider(conf, flow->provider)) {
            clientId = provider->value("client_id", std::string());
            clientSecret = provider->value("client_secret", std::string());
            issuer = provider->value("issuer", std::string());
        }
        const auto endpoints = discover(issuer);
        if (code.empty() || clientId.empty() || !endpoints) { cb(back("sso_error=provider")); return; }
        std::string form = "grant_type=authorization_code&code=" + urlEncode(code) +
            "&redirect_uri=" + urlEncode(flow->redirectUri) + "&client_id=" + urlEncode(clientId) +
            "&code_verifier=" + urlEncode(flow->verifier);
        if (!clientSecret.empty()) form += "&client_secret=" + urlEncode(clientSecret);
        const auto token = httpJson("POST", endpoints->token, {}, form, "application/x-www-form-urlencoded");
        const std::string accessToken = token.body.is_object() ? token.body.value("access_token", std::string()) : std::string();
        if (token.status != 200 || accessToken.empty()) { cb(back("sso_error=token")); return; }
        const auto info = httpJson("GET", endpoints->userinfo, {{"Authorization", "Bearer " + accessToken}});
        if (info.status != 200 || !info.body.is_object()) { cb(back("sso_error=userinfo")); return; }
        const std::string sub = info.body.value("sub", std::string());

        if (flow->purpose == "bind") {
            const auto& auth = WebAuth::instance();
            if (!auth.validToken(flow->bindToken) || roleOf(auth.identityOf(flow->bindToken), cfg, router) != Role::Full) {
                cb(back("sso_error=forbidden")); return;
            }
            json* provider = findProvider(conf, flow->provider);
            if (!provider || sub.empty()) { cb(back("sso_error=provider")); return; }
            json& bindings = (*provider)["bindings"];
            if (!bindings.is_array()) bindings = json::array();
            const bool known = std::any_of(bindings.begin(), bindings.end(), [&](const json& b) { return b.value("sub", std::string()) == sub; });
            if (!known) {
                std::string label = info.body.value("preferred_username", info.body.value("email", info.body.value("name", sub)));
                bindings.push_back({{"sub", sub}, {"label", label}, {"bound_at", detail::nowIso()}});
                if (!saveConfig(cfg, conf)) { cb(back("sso_error=save")); return; }
                DICE_LOG_INFO("网页后台：已绑定第三方登录「{}」的身份 {}。", flow->provider, label);
            }
            cb(back("sso=bound", "#/webui-settings")); return;
        }

        json identity;
        if (flow->provider == "bdc") {
            const std::string qq = qqFromUserinfo(info.body);
            if (qq.empty()) { cb(back("sso_error=qq")); return; }
            identity = {{"kind", "bdc"}, {"qq", qq}, {"label", qq}};
        } else {
            if (sub.empty()) { cb(back("sso_error=provider")); return; }
            const json* provider = findProvider(conf, flow->provider);
            std::string label;
            for (const auto& binding : provider ? provider->value("bindings", json::array()) : json::array())
                if (binding.value("sub", std::string()) == sub) label = binding.value("label", sub);
            identity = {{"kind", "oidc"}, {"provider", flow->provider}, {"sub", sub}, {"label", label.empty() ? sub : label}};
        }
        if (roleOf(identity, cfg, router) == Role::None) {
            DICE_LOG_WARN("网页后台：第三方登录「{}」的身份 {} 没有后台权限，已拒绝。", flow->provider, identity.value("label", std::string()));
            cb(back(flow->provider == "bdc" ? "sso_error=forbidden" : "sso_error=unbound")); return;
        }
        bool persisted = true;
        const std::string sessionToken = WebAuth::instance().issueIdentityToken(identity, kSessionSeconds, &persisted);
        if (!persisted) { cb(back("sso_error=save")); return; }
        DICE_LOG_INFO("网页后台：{} 通过第三方登录「{}」登录。", identity.value("label", std::string()), flow->provider);
        auto resp = back("sso=login");
        resp->addCookie(makeCookie(sessionCookieName, sessionToken, kSessionSeconds));
        cb(resp);
    }, {drogon::Get});

    // 已上传的提供方图标（公开）。
    app.registerHandler("/api/auth/sso/logo/{1}", [&cfg, configPath](const HttpRequestPtr&, Callback&& cb, const std::string& id) {
        json conf = config(cfg);
        const json* provider = findProvider(conf, id);
        const std::string ext = provider ? provider->value("logo_file", std::string()) : std::string();
        if (ext.empty()) { cb(detail::fail("not found", drogon::k404NotFound)); return; }
        std::ifstream in(detail::logoDir(configPath) / (id + "." + ext), std::ios::binary);
        if (!in) { cb(detail::fail("not found", drogon::k404NotFound)); return; }
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeString(detail::contentTypeFor(ext));
        resp->addHeader("X-Content-Type-Options", "nosniff");
        resp->addHeader("Cache-Control", "public, max-age=86400");
        resp->setBody(std::move(bytes));
        cb(resp);
    }, {drogon::Get});

    // 配置总览（不含任何密钥）。
    app.registerHandler("/api/auth/sso/config", [&cfg](const HttpRequestPtr&, Callback&& cb) {
        json conf = config(cfg), providers = json::array();
        for (const auto& provider : conf["providers"]) if (provider.is_object()) providers.push_back(detail::publicProvider(provider));
        const auto& bdc = conf["bdc"];
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}, {"data", {
            {"bdc", {{"enabled", bdc.value("enabled", false)}, {"client_id", bdc.value("client_id", std::string())},
                     {"redirect_uris", bdc.value("redirect_uris", json::array())}, {"has_key", !bdcApiKeys(cfg).empty()}}},
            {"providers", providers}, {"callback_path", kCallbackPath}, {"session_hours", kSessionSeconds / 3600}}}}));
    }, {drogon::Get});

    // 启用 / 停用骰子之间账号登录；启用时登记当前后台地址为回调地址。
    app.registerHandler("/api/auth/sso/bdc", [&cfg](const HttpRequestPtr& req, Callback&& cb) {
        if (!detail::fullAccess(req)) { cb(detail::fail("需要骰主权限", drogon::k403Forbidden)); return; }
        const json body = json::parse(req->body(), nullptr, false);
        if (!body.is_object() || !body.contains("enabled") || !body["enabled"].is_boolean()) { cb(detail::fail("enabled must be boolean")); return; }
        json conf = config(cfg);
        json& bdc = conf["bdc"];
        if (body["enabled"].get<bool>()) {
            if (bdcApiKeys(cfg).empty()) { cb(detail::fail("尚未在任何适配器配置骰娘 Key")); return; }
            if (body.contains("base")) {
                const auto base = normalizeBase(body.value("base", std::string()));
                if (!base) { cb(detail::fail("后台地址必须是 HTTPS")); return; }
                const auto result = detail::changeBdcRedirect(cfg, callbackFor(*base), true);
                if (result.status != 200 || !result.body.is_object()) {
                    const std::string reason = result.body.is_object() ? result.body.value("error_description", std::string()) : std::string();
                    cb(detail::fail(reason.empty() ? "无法向骰子之间登记回调地址" : reason, drogon::k502BadGateway)); return;
                }
                bdc["client_id"] = result.body.value("client_id", std::string());
                bdc["redirect_uris"] = result.body.value("redirect_uris", json::array());
            }
            if (bdc.value("client_id", std::string()).empty()) { cb(detail::fail("请先登记至少一个回调地址")); return; }
            bdc["enabled"] = true;
        } else {
            bdc["enabled"] = false;
            WebAuth::instance().revokeWhere([](const json& identity) { return identity.value("kind", std::string()) == "bdc"; });
        }
        if (!saveConfig(cfg, conf)) { cb(detail::fail("无法保存配置", drogon::k500InternalServerError)); return; }
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}}));
    }, {drogon::Put});

    // 移除一个已登记的回调地址。
    app.registerHandler("/api/auth/sso/bdc/redirect-uris", [&cfg](const HttpRequestPtr& req, Callback&& cb) {
        if (!detail::fullAccess(req)) { cb(detail::fail("需要骰主权限", drogon::k403Forbidden)); return; }
        const json body = json::parse(req->body(), nullptr, false);
        const std::string uri = body.is_object() ? body.value("redirect_uri", std::string()) : std::string();
        if (uri.empty()) { cb(detail::fail("redirect_uri required")); return; }
        json conf = config(cfg);
        json& bdc = conf["bdc"];
        const auto result = detail::changeBdcRedirect(cfg, uri, false);
        if (result.status == 200 && result.body.is_object()) bdc["redirect_uris"] = result.body.value("redirect_uris", json::array());
        else {
            json kept = json::array();
            for (const auto& item : bdc.value("redirect_uris", json::array())) if (item != uri) kept.push_back(item);
            bdc["redirect_uris"] = kept;
        }
        if (!saveConfig(cfg, conf)) { cb(detail::fail("无法保存配置", drogon::k500InternalServerError)); return; }
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}}));
    }, {drogon::Delete});

    // 新建或修改自定义提供方；删除提供方。
    app.registerHandler("/api/auth/sso/providers/{1}", [&cfg, configPath](const HttpRequestPtr& req, Callback&& cb, const std::string& id) {
        if (!detail::fullAccess(req)) { cb(detail::fail("需要骰主权限", drogon::k403Forbidden)); return; }
        if (!validProviderId(id)) { cb(detail::fail("提供方标识只能由 2~32 位字母、数字、-、_ 组成")); return; }
        json conf = config(cfg);
        if (req->method() == drogon::Delete) {
            json kept = json::array();
            for (const auto& provider : conf["providers"]) if (provider.value("id", std::string()) != id) kept.push_back(provider);
            conf["providers"] = kept;
            if (!saveConfig(cfg, conf)) { cb(detail::fail("无法保存配置", drogon::k500InternalServerError)); return; }
            std::error_code ec;
            for (const char* ext : {"png", "jpg", "gif", "webp"}) std::filesystem::remove(detail::logoDir(configPath) / (id + "." + ext), ec);
            WebAuth::instance().revokeWhere([&id](const json& identity) {
                return identity.value("kind", std::string()) == "oidc" && identity.value("provider", std::string()) == id;
            });
            cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}})); return;
        }
        const json body = json::parse(req->body(), nullptr, false);
        if (!body.is_object()) { cb(detail::fail("invalid JSON")); return; }
        std::string issuer = body.value("issuer", std::string());
        while (!issuer.empty() && issuer.back() == '/') issuer.pop_back();
        const std::string name = body.value("name", std::string());
        const std::string clientId = body.value("client_id", std::string());
        const std::string logoUrl = body.value("logo_url", std::string());
        if (name.empty() || name.size() > 60) { cb(detail::fail("名称为 1~60 个字符")); return; }
        if (clientId.empty() || clientId.size() > 200) { cb(detail::fail("client_id 不能为空")); return; }
        if (!logoUrl.empty() && !splitHttpsUrl(logoUrl)) { cb(detail::fail("图标链接必须是 HTTPS")); return; }
        if (!discover(issuer)) { cb(detail::fail("无法读取发行方的 OpenID 配置（需 HTTPS 且提供 /.well-known/openid-configuration）")); return; }
        json* provider = findProvider(conf, id);
        if (!provider) {
            conf["providers"].push_back({{"id", id}, {"bindings", json::array()}});
            provider = &conf["providers"].back();
        }
        (*provider)["name"] = name;
        (*provider)["issuer"] = issuer;
        (*provider)["client_id"] = clientId;
        if (body.contains("client_secret") && body["client_secret"].is_string() && !body["client_secret"].get<std::string>().empty())
            (*provider)["client_secret"] = body["client_secret"];
        if (body.value("clear_secret", false)) provider->erase("client_secret");
        (*provider)["scopes"] = body.value("scopes", std::string("openid profile email"));
        (*provider)["enabled"] = body.value("enabled", true);
        (*provider)["logo_url"] = logoUrl;
        if (!saveConfig(cfg, conf)) { cb(detail::fail("无法保存配置", drogon::k500InternalServerError)); return; }
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}, {"data", detail::publicProvider(*provider)}}));
    }, {drogon::Put, drogon::Delete});

    // 解除一个第三方身份绑定（同时让它的会话失效）。
    app.registerHandler("/api/auth/sso/providers/{1}/bindings", [&cfg](const HttpRequestPtr& req, Callback&& cb, const std::string& id) {
        if (!detail::fullAccess(req)) { cb(detail::fail("需要骰主权限", drogon::k403Forbidden)); return; }
        const json body = json::parse(req->body(), nullptr, false);
        const std::string sub = body.is_object() ? body.value("sub", std::string()) : std::string();
        json conf = config(cfg);
        json* provider = findProvider(conf, id);
        if (!provider || sub.empty()) { cb(detail::fail("not found", drogon::k404NotFound)); return; }
        json kept = json::array();
        for (const auto& binding : provider->value("bindings", json::array())) if (binding.value("sub", std::string()) != sub) kept.push_back(binding);
        (*provider)["bindings"] = kept;
        if (!saveConfig(cfg, conf)) { cb(detail::fail("无法保存配置", drogon::k500InternalServerError)); return; }
        WebAuth::instance().revokeWhere([&](const json& identity) {
            return identity.value("kind", std::string()) == "oidc" && identity.value("provider", std::string()) == id &&
                   identity.value("sub", std::string()) == sub;
        });
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}}));
    }, {drogon::Delete});

    // 上传 / 删除提供方图标（PNG、JPEG、GIF、WebP，最大 256 KB）。
    app.registerHandler("/api/auth/sso/providers/{1}/logo", [&cfg, configPath](const HttpRequestPtr& req, Callback&& cb, const std::string& id) {
        if (!detail::fullAccess(req)) { cb(detail::fail("需要骰主权限", drogon::k403Forbidden)); return; }
        json conf = config(cfg);
        json* provider = findProvider(conf, id);
        if (!provider) { cb(detail::fail("not found", drogon::k404NotFound)); return; }
        std::error_code ec;
        for (const char* ext : {"png", "jpg", "gif", "webp"}) std::filesystem::remove(detail::logoDir(configPath) / (id + "." + ext), ec);
        if (req->method() == drogon::Delete) {
            provider->erase("logo_file");
        } else {
            const json body = json::parse(req->body(), nullptr, false);
            const std::string bytes = drogon::utils::base64Decode(body.is_object() ? body.value("content", std::string()) : std::string());
            if (bytes.empty() || bytes.size() > 256 * 1024) { cb(detail::fail("图标须为不超过 256 KB 的图片")); return; }
            const std::string ext = detail::logoExtension(bytes);
            if (ext.empty()) { cb(detail::fail("仅支持 PNG、JPEG、GIF、WebP 图片")); return; }
            std::filesystem::create_directories(detail::logoDir(configPath), ec);
            std::ofstream out(detail::logoDir(configPath) / (id + "." + ext), std::ios::binary | std::ios::trunc);
            if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
                cb(detail::fail("无法保存图标", drogon::k500InternalServerError)); return;
            }
            (*provider)["logo_file"] = ext;
            (*provider)["logo_version"] = provider->value("logo_version", 0) + 1;
        }
        if (!saveConfig(cfg, conf)) { cb(detail::fail("无法保存配置", drogon::k500InternalServerError)); return; }
        cb(detail::jsonResponse({{"code", 0}, {"message", "ok"}, {"data", detail::publicProvider(*provider)}}));
    }, {drogon::Post, drogon::Delete});
}

} // namespace dice::sso
