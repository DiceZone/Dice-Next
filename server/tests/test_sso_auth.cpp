// 管理后台第三方登录：PKCE、QQ 身份、权限判定与受限管理员的拦截范围。

#include "test_framework.h"
#include "../src/service/sso_auth.h"
#include "../src/service/web_auth.h"

#include <chrono>
#include <filesystem>
#include <memory>

using namespace dice;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() / ("dnx-sso-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(root, ec); }
};

struct FakeRouter {
    int trust = 0;
    int panelTrustOfQQ(const std::string& qq) const { return qq == "10001" ? trust : 0; }
};

} // namespace

TEST(SsoAuth, PkceChallengeMatchesRfc7636Example) {
    ASSERT_EQ(sso::pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
              std::string("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
    const auto verifier = sso::randomUrlToken(48);
    ASSERT_EQ(verifier.size(), size_t(64));
    ASSERT_TRUE(verifier.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
}

TEST(SsoAuth, OnlyVerifiedQqMailboxesYieldAQqNumber) {
    ASSERT_EQ(sso::qqFromUserinfo({{"email", "10001@qq.com"}, {"email_verified", true}}), std::string("10001"));
    ASSERT_EQ(sso::qqFromUserinfo({{"email", "10001@QQ.COM"}, {"email_verified", true}}), std::string("10001"));
    ASSERT_TRUE(sso::qqFromUserinfo({{"email", "10001@qq.com"}, {"email_verified", false}}).empty());
    ASSERT_TRUE(sso::qqFromUserinfo({{"email", "shia@qq.com"}, {"email_verified", true}}).empty());
    ASSERT_TRUE(sso::qqFromUserinfo({{"email", "10001@example.com"}, {"email_verified", true}}).empty());
    ASSERT_TRUE(sso::qqFromUserinfo({{"email", "01234@qq.com"}, {"email_verified", true}}).empty());
}

TEST(SsoAuth, RolesFollowLocalTrustLevels) {
    ASSERT_TRUE(sso::roleForTrust(256) == sso::Role::Full);
    ASSERT_TRUE(sso::roleForTrust(4) == sso::Role::Admin);
    ASSERT_TRUE(sso::roleForTrust(5) == sso::Role::Admin);
    ASSERT_TRUE(sso::roleForTrust(3) == sso::Role::None);
    ASSERT_TRUE(sso::roleForTrust(-1) == sso::Role::None);
}

TEST(SsoAuth, RestrictedAdminsCannotReachPrivilegedOperations) {
    for (const char* path : {"/api/masters", "/api/masters/onebot_v11/10001", "/api/auth/sso/bdc",
                             "/api/auth/sso/providers/gh", "/api/system/webui-auth", "/api/system/server-config",
                             "/api/system/update/install", "/api/plugins/js/upload", "/api/mod/lua/toggle",
                             "/api/mods/upload", "/api/rulepacks/upload", "/api/backup/restore",
                             "/api/players/onebot_v11/10001/luavar"})
        ASSERT_TRUE(sso::blockedForRestrictedAdmin("POST", path));
    ASSERT_TRUE(sso::blockedForRestrictedAdmin("GET", "/api/backup/download"));
    ASSERT_TRUE(sso::blockedForRestrictedAdmin("GET", "/api/backup/export"));
    for (const char* path : {"/api/masters", "/api/plugins/js", "/api/auth/sso/config", "/api/backup/list"})
        ASSERT_FALSE(sso::blockedForRestrictedAdmin("GET", path));
    ASSERT_FALSE(sso::blockedForRestrictedAdmin("PUT", "/api/players/onebot_v11/10001"));
    ASSERT_FALSE(sso::blockedForRestrictedAdmin("POST", "/api/decks/upload"));
    ASSERT_FALSE(sso::blockedForRestrictedAdmin("POST", "/api/mastersx"));
}

TEST(SsoAuth, OnlyHttpsBasesAreAccepted) {
    ASSERT_EQ(*sso::normalizeBase("https://pub.dice.zone/myname/"), std::string("https://pub.dice.zone/myname"));
    ASSERT_EQ(sso::callbackFor("https://panel.example.com"), std::string("https://panel.example.com/api/auth/sso/callback"));
    ASSERT_FALSE(sso::normalizeBase("http://panel.example.com").has_value());
    ASSERT_FALSE(sso::normalizeBase("https://user@panel.example.com").has_value());
    ASSERT_FALSE(sso::normalizeBase("https://panel.example.com/#x").has_value());
    ASSERT_FALSE(sso::normalizeBase("https://panel.example.com/?a=1").has_value());
    const auto parts = sso::splitHttpsUrl("https://account.dice.zone/api/oauth/token");
    ASSERT_TRUE(parts.has_value());
    ASSERT_EQ(parts->origin, std::string("https://account.dice.zone"));
    ASSERT_EQ(parts->pathQuery, std::string("/api/oauth/token"));
}

TEST(SsoAuth, IdentitySessionsPersistAndCanBeRevokedByIdentity) {
    TempDir t;
    const auto sessions = t.root / "webui_sessions.json";
    WebAuth auth;
    auth.configure(WebAuth::hashPassword("Aa1!aaaa"), sessions);
    const json identity{{"kind", "bdc"}, {"qq", "10001"}, {"label", "10001"}};
    bool persisted = false;
    const auto token = auth.issueIdentityToken(identity, sso::kSessionSeconds, &persisted);
    ASSERT_TRUE(persisted);
    ASSERT_TRUE(auth.validToken(token));
    ASSERT_TRUE(auth.persistentSecondsRemaining(token) > sso::kSessionSeconds - 60);
    const auto password = auth.issueToken();

    WebAuth restarted;
    restarted.configure(WebAuth::hashPassword("Aa1!aaaa"), sessions);
    ASSERT_TRUE(restarted.validToken(token));
    ASSERT_EQ(restarted.identityOf(token).value("qq", std::string()), std::string("10001"));

    ASSERT_EQ(auth.revokeWhere([](const json& id) { return id.value("kind", std::string()) == "bdc"; }), 1);
    ASSERT_FALSE(auth.validToken(token));
    ASSERT_TRUE(auth.validToken(password));
    ASSERT_TRUE(auth.identityOf(password).empty());
}

TEST(SsoAuth, SessionRolesAreReevaluatedFromCurrentConfiguration) {
    TempDir t;
    ConfigManager cfg(t.root.string());
    cfg.load();   // 空目录没有配置文件；只用内存中的设置
    FakeRouter router;
    const json bdcUser{{"kind", "bdc"}, {"qq", "10001"}};
    ASSERT_TRUE(sso::roleOf(json::object(), cfg, router) == sso::Role::Full);
    ASSERT_TRUE(sso::roleOf(bdcUser, cfg, router) == sso::Role::None);   // 未启用

    cfg.set<json>("webui/sso", json{{"bdc", {{"enabled", true}}}, {"providers", json::array()}});
    router.trust = 256;
    int trust = 0;
    ASSERT_TRUE(sso::roleOf(bdcUser, cfg, router, &trust) == sso::Role::Full);
    ASSERT_EQ(trust, 256);
    router.trust = 4;
    ASSERT_TRUE(sso::roleOf(bdcUser, cfg, router) == sso::Role::Admin);
    router.trust = 2;
    ASSERT_TRUE(sso::roleOf(bdcUser, cfg, router) == sso::Role::None);

    const json oidcUser{{"kind", "oidc"}, {"provider", "gh"}, {"sub", "abc"}};
    cfg.set<json>("webui/sso", json{{"bdc", json::object()}, {"providers", json::array({
        {{"id", "gh"}, {"enabled", true}, {"bindings", json::array({{{"sub", "abc"}}})}}})}});
    ASSERT_TRUE(sso::roleOf(oidcUser, cfg, router) == sso::Role::Full);
    ASSERT_TRUE(sso::roleOf({{"kind", "oidc"}, {"provider", "gh"}, {"sub", "other"}}, cfg, router) == sso::Role::None);
    cfg.set<json>("webui/sso", json{{"bdc", json::object()}, {"providers", json::array({
        {{"id", "gh"}, {"enabled", false}, {"bindings", json::array({{{"sub", "abc"}}})}}})}});
    ASSERT_TRUE(sso::roleOf(oidcUser, cfg, router) == sso::Role::None);
}
