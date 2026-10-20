#pragma once
// ─── 管理后台第三方登录（OpenID Connect）────────────────────────
// 两类登录来源：
// - 骰子之间（BDC）：用任一适配器配置的骰娘 Key 向账号站登记回调地址；账号站给出
//   验证过的真实 QQ，后台按本地骰主名单与信任等级决定能否进入（骰主=完整权限，
//   管理员=受限权限，其余拒绝）。
// - 自定义 OIDC 提供方：骰主手工填写发行方与客户端；只有骰主在后台里亲自绑定过的
//   第三方身份可以登录，登录后为完整权限。
// 第三方登录的会话固定一天有效，不向提供方回查；本地口令登录始终保留作应急入口。

#include "../config/config_manager.h"

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dice::sso {

using json = nlohmann::json;

inline constexpr const char* kBdcIssuer = "https://account.dice.zone";
inline constexpr const char* kCallbackPath = "/api/auth/sso/callback";
inline constexpr const char* kStateCookie = "dn_sso_state";
inline constexpr int kSessionSeconds = 24 * 3600;
inline constexpr int kFlowSeconds = 600;
inline constexpr int kTrustAdmin = 4;
inline constexpr int kTrustMaster = 256;

enum class Role { None, Admin, Full };

inline const char* roleName(Role role) {
    return role == Role::Full ? "full" : role == Role::Admin ? "admin" : "none";
}

/// 由本地有效信任等级得出后台权限：骰主完整，管理员（≥4）受限，其余不可进入。
inline Role roleForTrust(int trust) {
    if (trust >= kTrustMaster) return Role::Full;
    if (trust >= kTrustAdmin) return Role::Admin;
    return Role::None;
}

/// 账号站的注册邮箱是验证过的 QQ 邮箱：前缀即真实 QQ 号。其他邮箱一律不认。
inline std::string qqFromUserinfo(const json& info) {
    if (!info.is_object() || !info.value("email_verified", false)) return {};
    std::string email = info.value("email", std::string());
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string suffix = "@qq.com";
    if (email.size() <= suffix.size() || email.compare(email.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    const std::string qq = email.substr(0, email.size() - suffix.size());
    if (qq.size() < 5 || qq.size() > 12 || qq.front() == '0') return {};
    if (!std::all_of(qq.begin(), qq.end(), [](unsigned char c) { return std::isdigit(c); })) return {};
    return qq;
}

inline std::string base64url(const unsigned char* data, size_t size) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const unsigned value = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += alphabet[(value >> 18) & 63]; out += alphabet[(value >> 12) & 63];
        out += alphabet[(value >> 6) & 63]; out += alphabet[value & 63];
    }
    if (i + 1 == size) {
        const unsigned value = data[i] << 16;
        out += alphabet[(value >> 18) & 63]; out += alphabet[(value >> 12) & 63];
    } else if (i + 2 == size) {
        const unsigned value = (data[i] << 16) | (data[i + 1] << 8);
        out += alphabet[(value >> 18) & 63]; out += alphabet[(value >> 12) & 63]; out += alphabet[(value >> 6) & 63];
    }
    return out;
}

inline std::string randomUrlToken(size_t bytes = 32) {
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) return {};
    return base64url(buffer.data(), buffer.size());
}

/// PKCE S256：BASE64URL(SHA256(verifier))。
inline std::string pkceChallenge(const std::string& verifier) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int size = 0;
    if (EVP_Digest(verifier.data(), verifier.size(), digest, &size, EVP_sha256(), nullptr) != 1) return {};
    return base64url(digest, size);
}

inline std::string urlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

struct UrlParts { std::string origin, pathQuery; };

/// 只接受 https://host[:port][/path][?query]，不带用户名、口令与片段。
inline std::optional<UrlParts> splitHttpsUrl(const std::string& url) {
    const std::string scheme = "https://";
    if (url.size() <= scheme.size() || url.size() > 1000 || url.compare(0, scheme.size(), scheme) != 0) return std::nullopt;
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return std::nullopt;
    if (url.find('#') != std::string::npos) return std::nullopt;
    const auto slash = url.find_first_of("/?", scheme.size());
    const std::string authority = url.substr(scheme.size(), slash == std::string::npos ? std::string::npos : slash - scheme.size());
    if (authority.empty() || authority.find('@') != std::string::npos) return std::nullopt;
    std::string pathQuery = slash == std::string::npos ? "/" : url.substr(slash);
    if (!pathQuery.empty() && pathQuery.front() == '?') pathQuery = "/" + pathQuery;
    return UrlParts{scheme + authority, pathQuery};
}

/// 浏览器所在的后台地址（含反代前缀，例如 https://pub.dice.zone/myname）。
inline std::optional<std::string> normalizeBase(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    const auto parts = splitHttpsUrl(base);
    if (!parts || parts->pathQuery.find('?') != std::string::npos) return std::nullopt;
    return base;
}

inline std::string callbackFor(const std::string& base) { return base + kCallbackPath; }

/// 受限管理员（信任等级 ≥4 的非骰主）不能做的事：改骰主名单、口令、服务配置与第三方登录，
/// 安装/启停/编辑插件与规则包，恢复或下载备份。插件与规则包能执行代码、读写配置，备份
/// 含口令哈希与各类密钥，放开任何一条都等于放开骰主权限。
inline bool blockedForRestrictedAdmin(const std::string& method, const std::string& path) {
    auto under = [&](const char* prefix) {
        const std::string p(prefix);
        return path == p || path.rfind(p + "/", 0) == 0;
    };
    if (under("/api/backup") && method != "GET") return true;
    if (path == "/api/backup/download" || path == "/api/backup/export" || path.rfind("/api/backup/download", 0) == 0) return true;
    if (method == "GET") return false;
    for (const char* prefix : {"/api/masters", "/api/auth/sso", "/api/system/webui-auth", "/api/system/server-config",
                               "/api/system/update", "/api/system/plugin-verify", "/api/system/js-fetch",
                               "/api/plugins/js", "/api/mod/lua", "/api/mods", "/api/rulepacks"})
        if (under(prefix)) return true;
    // 玩家 Lua 变量里的 trust 键直接对应信任等级。
    if (path.rfind("/api/players/", 0) == 0 && path.size() > 7 && path.compare(path.size() - 7, 7, "/luavar") == 0) return true;
    return false;
}

// ─── HTTP ───────────────────────────────────────────────────────
struct HttpResult { int status = 0; json body; };

inline HttpResult httpJson(const std::string& method, const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers = {},
                           const std::string& body = {}, const std::string& contentType = {}) {
    const auto parts = splitHttpsUrl(url);
    // OIDC 端点不带查询串；带了就不请求，免得 drogon 二次编码出错。
    if (!parts || parts->pathQuery.find('?') != std::string::npos) return {};
    // 独立事件循环：同步请求不占用管理面板与适配器的循环。
    trantor::EventLoopThread loop("sso-http"); loop.run();
    auto client = drogon::HttpClient::newHttpClient(parts->origin, loop.getLoop(), false, true);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath(parts->pathQuery);
    req->setMethod(method == "POST" ? drogon::Post : method == "DELETE" ? drogon::Delete : drogon::Get);
    req->addHeader("Accept", "application/json");
    for (const auto& [name, value] : headers) req->addHeader(name, value);
    if (!body.empty()) {
        req->addHeader("Content-Type", contentType.empty() ? "application/json" : contentType);
        req->setBody(body);
    }
    const auto [result, response] = client->sendRequest(req, 10.0);
    if (result != drogon::ReqResult::Ok || !response || response->body().size() > 1024 * 1024) return {};
    return {static_cast<int>(response->statusCode()), json::parse(response->body(), nullptr, false)};
}

// ─── 配置 ───────────────────────────────────────────────────────
inline json config(ConfigManager& cfg) {
    json value = cfg.get<json>("webui/sso", json::object());
    if (!value.is_object()) value = json::object();
    if (!value.contains("bdc") || !value["bdc"].is_object()) value["bdc"] = json::object();
    if (!value.contains("providers") || !value["providers"].is_array()) value["providers"] = json::array();
    return value;
}

inline bool saveConfig(ConfigManager& cfg, const json& value) {
    cfg.set<json>("webui/sso", value);
    return cfg.save();
}

/// 各适配器配置的骰娘 Key（bdc_ 前缀）。任一把都能登记 BDC 登录。
inline std::vector<std::string> bdcApiKeys(ConfigManager& cfg) {
    std::vector<std::string> keys;
    const json all = cfg.getAll();
    if (!all.contains("adapters") || !all["adapters"].is_array()) return keys;
    for (const auto& adapter : all["adapters"]) {
        if (!adapter.is_object()) continue;
        const std::string key = adapter.value("heart_api_key", adapter.value("heartApiKey", std::string()));
        if (key.rfind("bdc_", 0) == 0 && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    return keys;
}

inline json* findProvider(json& conf, const std::string& id) {
    for (auto& provider : conf["providers"])
        if (provider.is_object() && provider.value("id", std::string()) == id) return &provider;
    return nullptr;
}

/// 提供方 id：字母数字、-、_，2~32 位。bdc 保留给内置提供方。
inline bool validProviderId(const std::string& id) {
    return id.size() >= 2 && id.size() <= 32 && id != "bdc" &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

// ─── 发现文档 ────────────────────────────────────────────────────
struct Endpoints { std::string authorization, token, userinfo; };

inline std::optional<Endpoints> discover(const std::string& issuerRaw) {
    std::string issuer = issuerRaw;
    while (!issuer.empty() && issuer.back() == '/') issuer.pop_back();
    if (!splitHttpsUrl(issuer)) return std::nullopt;
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::time_t, Endpoints>> cache;
    const auto now = std::time(nullptr);
    {
        std::lock_guard lock(mutex);
        const auto it = cache.find(issuer);
        if (it != cache.end() && now - it->second.first < 3600) return it->second.second;
    }
    const auto result = httpJson("GET", issuer + "/.well-known/openid-configuration");
    if (result.status != 200 || !result.body.is_object()) return std::nullopt;
    Endpoints endpoints{result.body.value("authorization_endpoint", std::string()),
                        result.body.value("token_endpoint", std::string()),
                        result.body.value("userinfo_endpoint", std::string())};
    if (!splitHttpsUrl(endpoints.authorization) || !splitHttpsUrl(endpoints.token) || !splitHttpsUrl(endpoints.userinfo))
        return std::nullopt;
    std::lock_guard lock(mutex);
    cache[issuer] = {now, endpoints};
    return endpoints;
}

// ─── 授权流程状态 ─────────────────────────────────────────────────
struct Flow {
    std::string provider, verifier, redirectUri, base, purpose, bindToken;
    std::time_t created = 0;
};

class FlowStore {
public:
    static FlowStore& instance() { static FlowStore store; return store; }
    std::string put(Flow flow) {
        std::lock_guard lock(mutex_);
        prune();
        if (flows_.size() >= 200) return {};
        const std::string state = randomUrlToken(24);
        if (state.empty()) return {};
        flow.created = std::time(nullptr);
        flows_[state] = std::move(flow);
        return state;
    }
    std::optional<Flow> take(const std::string& state) {
        std::lock_guard lock(mutex_);
        prune();
        const auto it = flows_.find(state);
        if (it == flows_.end()) return std::nullopt;
        Flow flow = std::move(it->second);
        flows_.erase(it);
        return flow;
    }
private:
    void prune() {
        const auto now = std::time(nullptr);
        for (auto it = flows_.begin(); it != flows_.end();)
            it = now - it->second.created > kFlowSeconds ? flows_.erase(it) : std::next(it);
    }
    std::mutex mutex_;
    std::map<std::string, Flow> flows_;
};

// ─── 会话权限 ────────────────────────────────────────────────────
/// 会话身份对应的当前权限。口令会话与 API Key 为完整权限；第三方会话每次按当前配置与
/// 本地信任等级重新判定（身份被解绑、降级或第三方登录被关闭后立即失效）。
template <typename Router>
Role roleOf(const json& identity, ConfigManager& cfg, const Router& router, int* trustOut = nullptr) {
    if (trustOut) *trustOut = kTrustMaster;
    if (!identity.is_object() || identity.empty()) return Role::Full;
    const std::string kind = identity.value("kind", std::string());
    json conf = config(cfg);
    if (kind == "bdc") {
        if (!conf["bdc"].value("enabled", false)) return Role::None;
        const int trust = router.panelTrustOfQQ(identity.value("qq", std::string()));
        if (trustOut) *trustOut = trust;
        return roleForTrust(trust);
    }
    if (kind == "oidc") {
        const json* provider = findProvider(conf, identity.value("provider", std::string()));
        if (!provider || !provider->value("enabled", true)) return Role::None;
        const std::string sub = identity.value("sub", std::string());
        for (const auto& binding : provider->value("bindings", json::array()))
            if (binding.is_object() && binding.value("sub", std::string()) == sub) return Role::Full;
        return Role::None;
    }
    return Role::None;
}

} // namespace dice::sso
