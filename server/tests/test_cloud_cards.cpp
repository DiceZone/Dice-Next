#include "test_framework.h"
#include "service/cloud_card_service.h"
#include "core/character/card_store.h"
#include "config/config_manager.h"
#include "storage/database.h"
#include "adapter/adapter_interface.h"
#include "core/identity/identity_binding.h"
#include <filesystem>
#include <chrono>
#include <future>

using namespace dice;
using namespace dice::cloud_cards;
namespace {
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-cloud-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { if (!std::filesystem::create_directory(path)) throw std::runtime_error("temp directory"); }
    ~Temp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
Json remote(Json raw = {{"力量", 60}}, int rev = 1, std::string name = "云卡") {
    auto doc = toDocument(raw, name, "coc7"); doc["card_id"] = "card-1"; doc["rev"] = rev; return doc;
}
struct Fixture {
    Temp temp;
    Database db;
    ConfigManager cfg{"unused-cloud-card-config"};
    CharacterCardStore cards{db};
    Message msg;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::vector<Request> calls;
    std::function<Response(const Request&)> custom;
    std::unique_ptr<Service> service;
    std::string scope = "cards.read cards.write", pollError;
    Fixture() {
        if (!db.open((temp.path / "main.db").string())) throw std::runtime_error("database");
        cfg.resetDefault();
        cfg.set<Json>("adapters", Json::array({{{"id", "a"}, {"heart_api_key", "bdc_test-secret"}},
            {{"id", "b"}, {"heart_api_key", "bdc_other-secret"}}}));
        msg.type = MessageType::kPrivate; msg.platform = "onebot_v11"; msg.adapterId = "a";
        msg.senderId = "1000"; msg.targetId = "1000"; msg.selfId = "9000"; msg.extra = Json::object();
        resetService();
    }
    ~Fixture() { service.reset(); }
    void resetService() {
        service = std::make_unique<Service>(db, cfg, cards, [this](const Request& r) {
            calls.push_back(r);
            if (custom) { auto out = custom(r); if (out.status) return out; }
            if (r.path == "/.well-known/openid-configuration")
                return Response{200, {{"issuer", "https://account.dice.zone"}, {"token_endpoint", "https://account.dice.zone/api/oauth/token"}}};
            if (r.path == "/api/oauth/device_authorization")
                return Response{200, {{"device_code", "secret-device"}, {"user_code", "ABCD-EFGH"},
                    {"verification_uri", "https://account.dice.zone/device"}, {"expires_in", 600}, {"interval", 5}}};
            if (r.path == "/api/oauth/token") {
                if (!pollError.empty()) return Response{400, {{"error", pollError}}};
                return Response{200, {{"access_token", "secret-access"}, {"token_type", "Bearer"}, {"scope", scope}, {"expires_in", 3600}}};
            }
            if (r.path == "/api/v1/cards") return Response{200, {{"items", Json::array({remote()})}}};
            if (r.path == "/api/v1/cards/card-1") return Response{200, remote()};
            return Response{404, Json::object()};
        }, [this] { return now; });
    }
    Result run(const std::string& args) { return service->handle(msg, args); }
    bool auth(bool write = true) {
        auto a = run(write ? "auth write" : "auth");
        now += std::chrono::seconds(6);
        return a.key == "cloud_card.auth_code" && a.secret && run("confirm").key == "cloud_card.authorized";
    }
};
bool rejects(const Json& doc) { try { (void)fromDocument(doc); return false; } catch (...) { return true; } }
}

TEST(CloudCards, CodecPreservesNumericTextLocksAndUnknownPluginFields) {
    Json raw = {{"力量", 60}, {"修正", -3}, {"enabled", true}, {"formula", "1d6"},
        {"plugin", {{"items", Json::array({1, "x", nullptr})}}},
        {"__meta", {{"locks", "r,w"}, {"texts", {{"职业", "侦探"}}}, {"custom", {{"x", 7}}}}}};
    auto doc = toDocument(raw, "侦探", "coc7");
    ASSERT_EQ(fromDocument(doc), raw);
    ASSERT_FALSE(doc["attrs"].contains("enabled"));
    ASSERT_EQ(doc["meta"]["locks"].size(), size_t(2));
    ASSERT_EQ(fromDocument(toDocument({{"__meta", "legacy"}}, "旧卡")), Json({{"__meta", "legacy"}}));
}

TEST(CloudCards, InvalidSchemaAndOutOfRangeValuesAreRejected) {
    auto doc = remote(); doc["schema_version"] = 2; ASSERT_TRUE(rejects(doc));
    doc = remote(); doc["schema_version"] = 1.5; ASSERT_TRUE(rejects(doc));
    doc = remote(); doc["attrs"]["力量"] = true; ASSERT_TRUE(rejects(doc));
    doc = remote(); doc["attrs"]["力量"] = 1.5; ASSERT_TRUE(rejects(doc));
    doc = remote(); doc["attrs"]["力量"] = 1000001; ASSERT_TRUE(rejects(doc));
    doc = remote(); doc["meta"]["locks"] = "w"; ASSERT_TRUE(rejects(doc));
    doc = remote(); doc["extra"]["力量"] = 20; ASSERT_TRUE(rejects(doc));
}

TEST(CloudCards, AuthorizationIsPrivateReadOnlyByDefaultAndRateLimited) {
    Fixture f;
    f.msg.type = MessageType::kGroup;
    ASSERT_EQ(f.run("auth").key, "cloud_card.private_only"); ASSERT_TRUE(f.calls.empty());
    f.msg.type = MessageType::kPrivate;
    auto r = f.run("auth"); ASSERT_TRUE(r.secret); ASSERT_EQ(r.args.at("code"), "ABCD-EFGH");
    ASSERT_TRUE(f.calls.back().body.find("cards.write") == std::string::npos);
    ASSERT_EQ(f.calls.back().apiKey, "bdc_test-secret");
    auto count = f.calls.size(); ASSERT_EQ(f.run("confirm").key, "cloud_card.wait"); ASSERT_EQ(f.calls.size(), count);
    f.now += std::chrono::seconds(6); f.pollError = "slow_down";
    ASSERT_EQ(f.run("confirm").key, "cloud_card.wait");
    f.now += std::chrono::seconds(6); count = f.calls.size();
    ASSERT_EQ(f.run("confirm").key, "cloud_card.wait"); ASSERT_EQ(f.calls.size(), count);
    f.now += std::chrono::seconds(5); f.pollError.clear();
    ASSERT_EQ(f.run("confirm").key, "cloud_card.authorized");
    ASSERT_EQ(f.run("list").key, "cloud_card.list");
    ASSERT_TRUE(f.calls.back().apiKey.empty()); ASSERT_EQ(f.calls.back().accessToken, "secret-access");
}

TEST(CloudCards, CredentialAndPlayerIsolationAndRevocation) {
    Fixture f; ASSERT_TRUE(f.auth());
    f.msg.senderId = "2000"; ASSERT_EQ(f.run("list").key, "cloud_card.expired");
    f.msg.senderId = "1000"; f.msg.adapterId = "b"; ASSERT_EQ(f.run("list").key, "cloud_card.expired");
    f.msg.adapterId = "a"; f.msg.extra["__identity_native_sender"] = "another-openid";
    ASSERT_EQ(f.run("list").key, "cloud_card.expired"); f.msg.extra.clear();
    ASSERT_EQ(f.run("status").key, "cloud_card.authorized");
    f.cfg.set<Json>("adapters", Json::array({{{"id", "a"}, {"heart_api_key", "bdc_rotated"}}}));
    ASSERT_EQ(f.run("list").key, "cloud_card.expired"); ASSERT_TRUE(f.auth());
    f.custom = [](const Request& r) { return r.path == "/api/v1/cards" ? Response{401, {{"detail", "secret must not be echoed"}}} : Response{}; };
    auto r = f.run("list"); ASSERT_EQ(r.key, "cloud_card.expired"); ASSERT_TRUE(r.args.empty());
    ASSERT_EQ(f.run("status").key, "cloud_card.expired");
}

TEST(CloudCards, ReadOnlyCannotUploadAndRestartDoesNotPersistTokens) {
    Fixture f; f.scope = "cards.read"; ASSERT_TRUE(f.auth(false));
    ASSERT_EQ(f.run("pull card-1").key, "cloud_card.pulled");
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.readonly");
    for (const auto& row : f.db.getStorage()->get_all<UserSettingRow>()) {
        ASSERT_TRUE(row.value.find("secret-access") == std::string::npos);
        ASSERT_TRUE(row.value.find("secret-device") == std::string::npos);
    }
    f.resetService(); ASSERT_EQ(f.run("list").key, "cloud_card.expired");
    ASSERT_TRUE(f.cards.cardExists("1000", "云卡"));
}

TEST(CloudCards, PullNeverOverwritesUnrelatedOrDirtyLocalCards) {
    Fixture f; ASSERT_TRUE(f.auth());
    ASSERT_TRUE(f.cards.createCard("1000", "云卡")); f.cards.setAttrByName("1000", "云卡", "力量", 12);
    ASSERT_EQ(f.run("pull card-1").key, "cloud_card.local_changes");
    ASSERT_EQ(f.cards.getAttrByName("1000", "云卡", "力量").value(), 12);
    ASSERT_EQ(f.run("pull card-1 导入卡").key, "cloud_card.pulled");
    f.cards.setAttrByName("1000", "导入卡", "力量", 70);
    ASSERT_EQ(f.run("pull card-1").key, "cloud_card.local_changes");
    ASSERT_EQ(f.run("pull card-1 云端对照副本").key, "cloud_card.pulled");
    ASSERT_EQ(f.cards.getAttrByName("1000", "导入卡", "力量").value(), 70);
    ASSERT_EQ(f.cards.getAttrByName("1000", "云端对照副本", "力量").value(), 60);
    ASSERT_TRUE(f.cards.boundCard("1000", "group").empty());
}

TEST(CloudCards, SyncUsesOriginalRevisionAndAcceptsServerMerge) {
    Fixture f; ASSERT_TRUE(f.auth()); ASSERT_EQ(f.run("pull card-1").key, "cloud_card.pulled");
    f.cards.setAttrByName("1000", "云卡", "力量", 70);
    Json sent;
    f.custom = [&](const Request& r) {
        if (r.method != "PUT") return Response{};
        sent = Json::parse(r.body); return Response{200, remote({{"力量", 70}, {"敏捷", 80}}, 3)};
    };
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.synced");
    ASSERT_EQ(sent["base_rev"], 1); ASSERT_EQ(sent["attrs"]["力量"], 70);
    ASSERT_EQ(f.cards.getAttrByName("1000", "云卡", "敏捷").value(), 80);
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.synced"); ASSERT_EQ(sent["base_rev"], 3);
}

TEST(CloudCards, ConflictsDoNotRetryOverwriteOrAdvanceBase) {
    Fixture f; ASSERT_TRUE(f.auth()); ASSERT_EQ(f.run("pull card-1").key, "cloud_card.pulled");
    f.cards.setAttrByName("1000", "云卡", "力量", 70);
    int writes = 0;
    f.custom = [&](const Request& r) {
        if (r.method != "PUT") return Response{};
        ++writes; return Response{409, {{"detail", {{"conflicts", Json::array({{{"key", "力量"}}})}}}}};
    };
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.conflict"); ASSERT_EQ(writes, 1);
    ASSERT_EQ(f.cards.getAttrByName("1000", "云卡", "力量").value(), 70);
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.conflict");
    ASSERT_EQ(Json::parse(f.calls.back().body)["base_rev"], 1);
}

TEST(CloudCards, LocalRenameKeepsCloudIdAndBindingsWorkOffline) {
    Fixture f; ASSERT_TRUE(f.auth()); ASSERT_EQ(f.run("pull card-1").key, "cloud_card.pulled");
    f.cards.bindCard("1000", "group", "云卡");
    ASSERT_TRUE(f.cards.renameCard("1000", "云卡", "改名卡"));
    Json sent;
    f.custom = [&](const Request& r) {
        if (r.method != "PUT") return Response{};
        sent = Json::parse(r.body); return Response{200, remote({{"力量", 60}}, 2, "改名卡")};
    };
    ASSERT_EQ(f.run("sync 改名卡").key, "cloud_card.synced");
    ASSERT_EQ(f.calls.back().path, "/api/v1/cards/card-1"); ASSERT_EQ(sent["name"], "改名卡");
    ASSERT_EQ(f.run("logout").key, "cloud_card.logout");
    ASSERT_EQ(f.cards.boundCard("1000", "group"), "改名卡"); ASSERT_EQ(f.cards.getAttr("1000", "group", "力量").value(), 60);
    ASSERT_EQ(f.run("sync 改名卡").key, "cloud_card.expired");
}

TEST(CloudCards, LocalChangesDuringNetworkRequestAreNotOverwritten) {
    Fixture f; ASSERT_TRUE(f.auth()); ASSERT_EQ(f.run("pull card-1").key, "cloud_card.pulled");
    f.cards.setAttrByName("1000", "云卡", "力量", 70);
    f.custom = [&](const Request& r) {
        if (r.method != "PUT") return Response{};
        f.cards.setAttrByName("1000", "云卡", "力量", f.cards.getAttrByName("1000", "云卡", "力量").value() + 20);
        return Response{200, remote({{"力量", 70}}, 2)};
    };
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.remote_saved");
    ASSERT_EQ(f.cards.getAttrByName("1000", "云卡", "力量").value(), 90);
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.remote_saved");
    ASSERT_EQ(Json::parse(f.calls.back().body)["base_rev"], 1);
}

TEST(CloudCards, FirstUploadCreatesCloudLinkWithoutChangingOtherUsers) {
    Fixture f; ASSERT_TRUE(f.auth());
    ASSERT_TRUE(f.cards.createCard("1000", "新卡")); f.cards.setAttrByName("1000", "新卡", "力量", 75);
    f.custom = [&](const Request& r) {
        if (r.method != "POST" || r.path != "/api/v1/cards") return Response{};
        return Response{200, remote({{"力量", 75}}, 1, "新卡")};
    };
    ASSERT_EQ(f.run("push 新卡").key, "cloud_card.synced");
    ASSERT_EQ(Json::parse(f.calls.back().body)["base_rev"], 0);
    ASSERT_TRUE(f.cards.listCards("2000").empty());
    f.cards.lockCardByName("1000", "新卡", "w");
    auto count = f.calls.size(); ASSERT_EQ(f.run("sync 新卡").key, "cloud_card.locked"); ASSERT_EQ(count, f.calls.size());
}

TEST(CloudCards, DiscoveryCannotRedirectCredentialsToOtherOrigins) {
    Fixture f;
    f.custom = [](const Request& r) {
        if (r.path != "/.well-known/openid-configuration") return Response{};
        return Response{200, {{"issuer", "https://account.dice.zone"}, {"token_endpoint", "https://attacker.invalid/token"}}};
    };
    ASSERT_EQ(f.run("auth").key, "cloud_card.network"); ASSERT_EQ(f.calls.size(), size_t(1));
    ASSERT_TRUE(f.calls.front().apiKey.empty());
}

TEST(CloudCards, LiveMetadataSmokeWhenExplicitlyEnabled) {
    if (!std::getenv("DICENEXT_CLOUD_LIVE")) return;
    auto r = probeOfficialMetadata();
    ASSERT_EQ(r.status, 200);
    ASSERT_EQ(r.body.value("issuer", ""), "https://account.dice.zone");
}

TEST(CloudCards, SnapshotRejectsAmbiguousLegacyRowsAndWrongOwner) {
    Fixture f;
    CharacterCardRow row; row.userId = "1000"; row.name = "旧卡"; row.attrs = "{\"力量\":40}";
    row.id = f.db.getCardStorage()->insert(row);
    ASSERT_FALSE(f.cards.snapshotById("2000", row.id).has_value());
    f.db.getCardStorage()->insert(row);
    ASSERT_FALSE(f.cards.snapshot("1000", "旧卡").has_value());
    ASSERT_TRUE(f.auth()); ASSERT_EQ(f.run("push 旧卡").key, "cloud_card.local_missing");
}

TEST(CloudCards, GrantedScopeCannotExceedRequestedScope) {
    Fixture f; ASSERT_TRUE(f.auth(false)); // Mock server returns read+write even though only read was requested.
    ASSERT_EQ(f.run("pull card-1").key, "cloud_card.pulled");
    ASSERT_EQ(f.run("sync 云卡").key, "cloud_card.readonly");
}

TEST(CloudCards, DeniedExpiredAndRemovedKeysClearAuthorization) {
    Fixture f;
    ASSERT_EQ(f.run("auth").key, "cloud_card.auth_code");
    f.now += std::chrono::seconds(6); f.pollError = "access_denied";
    ASSERT_EQ(f.run("confirm").key, "cloud_card.denied");
    ASSERT_EQ(f.run("status").key, "cloud_card.expired");
    ASSERT_EQ(f.run("auth").key, "cloud_card.auth_code");
    f.now += std::chrono::seconds(6); f.pollError = "expired_token";
    ASSERT_EQ(f.run("confirm").key, "cloud_card.expired");
    f.pollError.clear(); ASSERT_TRUE(f.auth());
    f.cfg.set<Json>("adapters", Json::array());
    ASSERT_EQ(f.run("status").key, "cloud_card.no_key");
    f.cfg.set<Json>("adapters", Json::array({{{"id", "a"}, {"heart_api_key", "bdc_test-secret"}}}));
    ASSERT_EQ(f.run("status").key, "cloud_card.expired");
    ASSERT_TRUE(f.auth()); f.now += std::chrono::seconds(3601);
    ASSERT_EQ(f.run("list").key, "cloud_card.expired");
}

TEST(CloudCards, BackgroundQueueIsBoundedAndDoesNotBlockCaller) {
    Fixture f;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future(); auto releaseFuture = release.get_future().share();
    std::promise<Result> completed; auto done = completed.get_future();
    f.custom = [&](const Request& r) {
        if (r.path == "/.well-known/openid-configuration") { entered.set_value(); releaseFuture.wait(); }
        return Response{};
    };
    const bool started = f.service->dispatch(f.msg, "auth", [&](Result r) { completed.set_value(std::move(r)); });
    const bool inTransport = enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool queued = true;
    for (int i = 0; i < 16; ++i) queued = f.service->dispatch(f.msg, "status", [](Result) {}) && queued;
    const bool overflow = f.service->dispatch(f.msg, "status", [](Result) {});
    release.set_value(); // Always release before assertions/destruction, including failure paths.
    const bool finished = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    f.service.reset();
    ASSERT_TRUE(started); ASSERT_TRUE(inTransport); ASSERT_TRUE(queued); ASSERT_FALSE(overflow); ASSERT_TRUE(finished);
    auto r = done.get(); ASSERT_EQ(r.key, "cloud_card.auth_code"); ASSERT_TRUE(r.secret);
}


namespace {
// 带 userinfo 的发现文档 + 一个已验证的 QQ 邮箱：BDC 注册只收纯数字 QQ 邮箱且
// 必须过验证码，所以邮箱前缀就是真实 QQ 号。
std::function<Response(const Request&)> withVerifiedEmail(const std::string& email) {
    return [email](const Request& r) -> Response {
        if (r.path == "/.well-known/openid-configuration")
            return Response{200, {{"issuer", "https://account.dice.zone"},
                {"token_endpoint", "https://account.dice.zone/api/oauth/token"},
                {"userinfo_endpoint", "https://account.dice.zone/api/oauth/userinfo"}}};
        if (r.path == "/api/oauth/userinfo") return Response{200, {{"email", email}}};
        return Response{0, Json::object()};
    };
}
void asOfficialSession(Fixture& f, const std::string& bot, const std::string& openId) {
    auto& bindings = identity::BindingStore::instance();
    bindings.observeOfficial(f.db, bot, openId, identity::Kind::User);
    f.msg.platform = "qq_official";
    f.msg.senderId = bindings.publicForOfficial(f.db, bot, openId, identity::Kind::User);
    f.msg.targetId = f.msg.senderId;
    f.msg.extra = Json{{"__identity_transport", "qq_official"},
                       {"__identity_native_sender", openId},
                       {"official_bot_id", bot}};
}
}   // namespace

TEST(CloudCards, VerifiedEmailBindsOfficialOpenIdToTheRealQQ) {
    Fixture f;
    f.scope = "cards.read email";
    f.custom = withVerifiedEmail("10001@qq.com");
    asOfficialSession(f, "app-bind", "openid-bind");
    auto& bindings = identity::BindingStore::instance();
    // 绑定前是虚拟号：官方 OpenID 背后的真实 QQ 还不知道。
    ASSERT_FALSE(identity::BindingStore::isRealQQ(f.msg.senderId));

    ASSERT_EQ(f.run("auth").key, std::string("cloud_card.auth_code"));
    f.now += std::chrono::seconds(6);
    auto confirmed = f.run("confirm");
    ASSERT_EQ(confirmed.key, std::string("cloud_card.authorized_bound"));
    ASSERT_EQ(confirmed.args.at("qq"), std::string("10001"));
    // 含真实 QQ 号，只能私发，不进普通回复留存。
    ASSERT_TRUE(confirmed.secret);
    ASSERT_EQ(bindings.publicForOfficial(f.db, "app-bind", "openid-bind", identity::Kind::User),
              std::string("10001"));
}

TEST(CloudCards, EmailIsOnlyReadWhenThatScopeWasGranted) {
    Fixture f;
    f.scope = "cards.read cards.write";   // 服务端没给 email
    f.custom = withVerifiedEmail("10002@qq.com");
    asOfficialSession(f, "app-noscope", "openid-noscope");

    ASSERT_EQ(f.run("auth").key, std::string("cloud_card.auth_code"));
    f.now += std::chrono::seconds(6);
    ASSERT_EQ(f.run("confirm").key, std::string("cloud_card.authorized"));
    // 没拿到 scope 就不该去碰 userinfo，更不该凭它绑定。
    for (const auto& call : f.calls) ASSERT_TRUE(call.path != "/api/oauth/userinfo");
    ASSERT_FALSE(identity::BindingStore::isRealQQ(
        identity::BindingStore::instance().publicForOfficial(f.db, "app-noscope", "openid-noscope",
                                                             identity::Kind::User)));
}

TEST(CloudCards, AnAlreadyRealQQSessionIsNeverRepointedAtAnotherAccount) {
    Fixture f;
    f.scope = "cards.read email";
    f.custom = withVerifiedEmail("10003@qq.com");
    // OneBot 会话本来就带着真实 QQ 1000，而 BDC 说的是 10003——两边都"是真的"，
    // 静默合并会把两个人的人物卡搅在一起，所以什么都不做。
    ASSERT_EQ(f.msg.senderId, std::string("1000"));

    ASSERT_EQ(f.run("auth").key, std::string("cloud_card.auth_code"));
    f.now += std::chrono::seconds(6);
    ASSERT_EQ(f.run("confirm").key, std::string("cloud_card.authorized"));
}

TEST(CloudCards, ExplicitBindingChecksTargetAndDiscardsIdentityOnlyTokens) {
    for (const auto* email : {"10004@qq.com", "10005@qq.com", "10004@qq.com.invalid"}) {
        Fixture f;
        f.scope = "cards.read cards.write email";
        f.custom = withVerifiedEmail(email);
        asOfficialSession(f, "app-target", "openid-target");
        ASSERT_EQ(f.service->handle(f.msg, "auth", "10004").key, "cloud_card.auth_code");
        ASSERT_TRUE(f.calls.back().body.find("scope=email&") != std::string::npos);
        ASSERT_TRUE(f.calls.back().body.find("cards.") == std::string::npos);
        f.now += std::chrono::seconds(6);
        ASSERT_EQ(f.service->handle(f.msg, "confirm", "10005").key, "cloud_card.expired");
        auto confirmed = f.service->handle(f.msg, "confirm", "10004");
        ASSERT_TRUE(confirmed.secret);
        const bool matches = std::string(email) == "10004@qq.com";
        ASSERT_EQ(confirmed.key, matches ? "identity_email.success" : "identity_email.oauth_not_verified");
        const auto current = identity::BindingStore::instance().publicForOfficial(
            f.db, "app-target", "openid-target", identity::Kind::User);
        ASSERT_EQ(identity::BindingStore::isRealQQ(current), matches);
        ASSERT_EQ(f.run("status").key, "cloud_card.expired");
    }
}

TEST(CloudCards, BindingRefusesIdentityChangedWhileAuthorizationWasPending) {
    Fixture f;
    f.scope = "email"; f.custom = withVerifiedEmail("10006@qq.com");
    asOfficialSession(f, "app-race", "openid-race");
    ASSERT_EQ(f.service->handle(f.msg, "auth", "10006").key, "cloud_card.auth_code");
    std::string error;
    auto& bindings = identity::BindingStore::instance();
    ASSERT_TRUE(bindings.bindVerifiedOfficialToQQ(f.db, "QQ-Official-app-race:openid-race", "10007", error));
    f.now += std::chrono::seconds(6);
    ASSERT_EQ(f.service->handle(f.msg, "confirm", "10006").key, "identity_email.oauth_not_verified");
    ASSERT_EQ(bindings.publicForOfficial(f.db, "app-race", "openid-race", identity::Kind::User), "10007");
}

TEST(CloudCards, BindingDoesNotReplaceExistingCloudAuthorization) {
    Fixture f;
    f.scope = "cards.read";
    f.custom = withVerifiedEmail("10008@qq.com");
    asOfficialSession(f, "app-separate", "openid-separate");
    ASSERT_TRUE(f.auth(false));
    ASSERT_EQ(f.service->handle(f.msg, "auth", "10009").key, "cloud_card.auth_code");
    ASSERT_EQ(f.run("status").key, "cloud_card.authorized");
    f.now += std::chrono::seconds(6); f.scope = "email";
    ASSERT_EQ(f.service->handle(f.msg, "confirm", "10009").key, "identity_email.oauth_not_verified");
    ASSERT_EQ(f.run("list").key, "cloud_card.list");
}
