// 管理后台第三方登录：PKCE、QQ 身份、权限判定与受限管理员的拦截范围。

#include "test_framework.h"
#include "../src/service/sso_auth.h"
#include "../src/service/web_auth.h"
#include "../src/adapter/adapter_interface.h"

#include <chrono>
#include <filesystem>
#include <atomic>
#include <memory>
#include <thread>

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
    // 写操作默认拒绝：骰主名单、第三方登录、系统设置、适配器、插件、规则包、旧版迁移、备份，
    // 以及没有列入白名单的新接口。
    for (const char* path : {"/api/masters", "/api/masters/onebot_v11/10001", "/api/auth/sso/bdc",
                             "/api/auth/sso/providers/gh", "/api/system/webui-auth", "/api/system/server-config",
                             "/api/system/update/install", "/api/system/ai", "/api/system/notice", "/api/system/global",
                             "/api/adapters", "/api/adapters/1", "/api/adapters/1/reconnect",
                             "/api/plugins/js/upload", "/api/plugins/js/config", "/api/mod/lua/toggle", "/api/mods/upload",
                             "/api/rulepacks/upload", "/api/rules/upload", "/api/rules/delete", "/api/rules/save",
                             "/api/rules/toggle", "/api/legacy/import", "/api/backup/restore", "/api/backup/create",
                             "/api/dice/rules", "/api/players/onebot_v11/10001/luavar", "/api/banlist/whitelist-only",
                             "/api/some-future-endpoint"})
        for (const char* method : {"POST", "PUT", "PATCH", "DELETE"})
            ASSERT_TRUE(sso::blockedForRestrictedAdmin(method, path));
    // 日常管理放行（玩家信任等级另在接口内按等级核对）。
    for (const char* path : {"/api/groups/onebot_v11/1001", "/api/groups/onebot_v11/1001/messages",
                             "/api/players/onebot_v11/10001", "/api/players/onebot_v11/10001/card-attr",
                             "/api/decks/upload", "/api/replies/3", "/api/schedules", "/api/banlist", "/api/logs/7",
                             "/api/game-sessions/abc", "/api/help/file", "/api/causal/rules/2", "/api/test/message",
                             "/api/rules/test", "/api/dice/roll", "/api/auth/logout"})
        ASSERT_FALSE(sso::blockedForRestrictedAdmin("POST", path));
    // 读取默认放行（响应会脱敏），插件、Lua 模组列表、规则包文件与备份下载除外。
    for (const char* path : {"/api/masters", "/api/adapters", "/api/system/ai", "/api/auth/sso/config",
                             "/api/backup/list", "/api/rules", "/api/players"})
        ASSERT_FALSE(sso::blockedForRestrictedAdmin("GET", path));
    for (const char* path : {"/api/plugins/js", "/api/plugins/js/config", "/api/plugins/js/storage", "/api/mod/lua",
                             "/api/rules/file", "/api/backup/download", "/api/backup/export"})
        ASSERT_TRUE(sso::blockedForRestrictedAdmin("GET", path));
}

TEST(SsoAuth, RestrictedResponsesAreRedactedOrRefusedNeverPassedThrough) {
    bool refused = false;
    auto body = sso::restrictedResponseBody("application/json", R"({"data":{"api_key":"sk-1","name":"gpt"}})", &refused);
    ASSERT_TRUE(body.has_value());
    ASSERT_FALSE(refused);
    ASSERT_TRUE(body->find("sk-1") == std::string::npos);
    ASSERT_TRUE(body->find("gpt") != std::string::npos);
    // 超过上限：不再原样放行，换成拒绝说明。
    std::string big = R"({"api_key":"sk-big","pad":")" + std::string(sso::kMaxRestrictedResponse, 'x') + "\"}";
    body = sso::restrictedResponseBody("application/json; charset=utf-8", big, &refused);
    ASSERT_TRUE(body.has_value() && refused);
    ASSERT_TRUE(body->find("sk-big") == std::string::npos);
    // 声称是 JSON 却解析不了：同样拒绝。
    body = sso::restrictedResponseBody("application/json", R"({"api_key":"sk-2")", &refused);
    ASSERT_TRUE(body.has_value() && refused);
    // 文本里装着 JSON 也要脱敏；真正的文件下载不动。
    body = sso::restrictedResponseBody("text/plain", R"({"token":"t-3"})", &refused);
    ASSERT_TRUE(body.has_value() && body->find("t-3") == std::string::npos);
    ASSERT_FALSE(sso::restrictedResponseBody("image/png", "PNG", &refused).has_value());
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

TEST(SsoAuth, RestrictedTestSenderIsAlwaysTheLoggedInQq) {
    Message msg;
    msg.senderId = "99999";          // 骰主
    msg.selfId = "10001";            // 试图让发送者等于骰娘自身
    msg.targetId = "100001";
    sso::bindTestSender(msg, "10001", false);
    ASSERT_EQ(msg.senderId, std::string("10001"));
    ASSERT_TRUE(msg.selfId.empty());
    ASSERT_EQ(msg.targetId, std::string("100001"));
    sso::bindTestSender(msg, "10001", true);
    ASSERT_EQ(msg.targetId, std::string("10001"));
}

TEST(SsoAuth, BackgroundTaskStopsPromptlyOnDestruction) {
    std::atomic<bool> ranPastWait{false};
    const auto started = std::chrono::steady_clock::now();
    {
        sso::BackgroundTask task;
        task.start([&](sso::BackgroundTask& self) {
            if (!self.waitFor(std::chrono::seconds(60))) return;
            ranPastWait = true;
        });
    }
    ASSERT_FALSE(ranPastWait.load());
    ASSERT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));

    sso::BackgroundTask quick;
    std::atomic<bool> done{false};
    quick.start([&](sso::BackgroundTask& self) { if (self.waitFor(std::chrono::milliseconds(10))) done = true; });
    for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(done.load());
}

TEST(SsoAuth, SecretsAreRedactedForRestrictedAdmins) {
    json body = {
        {"code", 0},
        {"data", {
            {"adapters", json::array({{{"id", 1}, {"accessToken", "onebot-token"}, {"heart_api_key", "bdc_secret"},
                                       {"appSecret", "qq-secret"}, {"token_tail", "...abcd"}}})},
            {"ai", {{"models", json::array({{{"name", "gpt"}, {"api_key", "sk-123"}, {"max_tokens", 2048}}})}}},
            {"smtp", {{"user", "dice@example.com"}, {"pass", "mail-pass"}}},
            {"webhook", {{"enabled", true}, {"url", "https://hook.example.com/?access_token=abc"}}},
            {"entries", json::array({{{"key", "dice/self_name"}, {"value", "希亚"}}})},
            {"keywords", json::array({"开团"})}, {"has_secret", true}, {"Password", ""},
            {"links", json::array({{{"url", "https://example.com"}}})}
        }}
    };
    sso::redactSecrets(body);
    const auto& d = body["data"];
    ASSERT_EQ(d["adapters"][0]["accessToken"], json(sso::kRedacted));
    ASSERT_EQ(d["adapters"][0]["heart_api_key"], json(sso::kRedacted));
    ASSERT_EQ(d["adapters"][0]["appSecret"], json(sso::kRedacted));
    ASSERT_EQ(d["adapters"][0]["token_tail"], json("...abcd"));
    ASSERT_EQ(d["ai"]["models"][0]["api_key"], json(sso::kRedacted));
    ASSERT_EQ(d["ai"]["models"][0]["max_tokens"], json(2048));
    ASSERT_EQ(d["smtp"]["pass"], json(sso::kRedacted));
    ASSERT_EQ(d["smtp"]["user"], json("dice@example.com"));
    ASSERT_EQ(d["webhook"]["url"], json(sso::kRedacted));
    ASSERT_EQ(d["entries"][0]["key"], json("dice/self_name"));
    ASSERT_EQ(d["keywords"][0], json("开团"));
    ASSERT_EQ(d["has_secret"], json(true));
    ASSERT_EQ(d["Password"], json(""));                       // 空值保持为空，表示未设置
    ASSERT_EQ(d["links"][0]["url"], json("https://example.com"));   // 只遮 Webhook 的地址
    json channels=json::array({{{"kind","webhook"},{"url","https://example.com/private?key=fake"},
        {"headers",{{"Authorization","fixture-private"}}},{"body_template",{{"private","fixture-body"}}},{"sign_secret","fixture-sign"}},{{"kind",1},{"url","https://example.com/public"}}});
    sso::redactSecrets(channels);
    ASSERT_EQ(channels[0]["url"],json(sso::kRedacted));
    ASSERT_EQ(channels[0]["headers"],json(sso::kRedacted));
    ASSERT_EQ(channels[0]["body_template"],json(sso::kRedacted));
    ASSERT_EQ(channels[0]["sign_secret"],json(sso::kRedacted));
    ASSERT_EQ(channels[1]["url"],json("https://example.com/public"));
}
