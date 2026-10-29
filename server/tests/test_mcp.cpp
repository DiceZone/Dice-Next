#include "test_framework.h"
#include "../src/service/mcp_service.h"
#include "../src/config/config_manager.h"
#include "../src/service/sso_auth.h"
#include <filesystem>
#include <atomic>
#include <fstream>
#include <thread>
#include <ctime>
#include <future>

namespace {
using namespace dice;
using J = nlohmann::json;
struct McpFixture {
    static inline std::atomic<unsigned> counter{0};
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("dicenext-mcp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter));
    ConfigManager config{(root / "config").string()};
    int reads = 0;
    bool unavailable = false;
    std::string owner = "user-a";
    mcp::Queries callbacks() {
        return {[](const mcp::QueryContext&) { return J{{"status", "running"}, {"version", "test"}}; },
            [](const std::string& q, int limit, const mcp::QueryContext&) { return J{{"query", q}, {"limit", limit}}; },
            [&](int id, const mcp::QueryContext&) { ++reads; if (id == 99 || unavailable) throw std::invalid_argument("人物卡不存在或已锁定读取"); return J{{"card_id", id}, {"user_id", owner}, {"name", "测试卡"}, {"attributes", {{"理智", 70}}}}; },
            [](const std::string&, const mcp::QueryContext&) { return J::array(); }};
    }
    std::unique_ptr<mcp::Service> service;
    McpFixture() { std::filesystem::create_directories(root); config.load(); service = std::make_unique<mcp::Service>(config, (root / "audit.db").string(), callbacks()); }
    ~McpFixture() { service.reset(); std::error_code ec; std::filesystem::remove_all(root, ec); }
    void enable() { service->configure({{"enabled", true}}); }
    J grant(J tools = J::array({"get_status", "roll_dice", "search_help"}), J cards = J::array()) {
        return service->createGrant({{"name", "测试客户端"}, {"tools", tools}, {"card_ids", cards}});
    }
    mcp::Request request(const J& grant, const std::string& method = "tools/call", J params = {{"name", "get_status"}}) {
        mcp::Request r;
        r.headers = {{"host", "127.0.0.1:18088"}, {"authorization", "Bearer " + grant.at("token").get<std::string>()},
            {"content-type", "application/json"}, {"accept", "application/json, text/event-stream"}, {"mcp-protocol-version", "2025-11-25"}};
        r.body = J{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump(); return r;
    }
};
template<class F> bool throwsInvalid(F fn) { try { fn(); } catch (const std::invalid_argument&) { return true; } return false; }
template<class F> bool queryCancelled(F fn, bool timedOut) {
    try { fn(); } catch (const mcp::QueryCancelled& e) { return e.timedOut == timedOut; }
    return false;
}
}
TEST(Mcp, QueryCancellationBeforeWaitIsPersistentAndIndependent) {
    mcp::QueryCancellation cancelled, independent;
    mcp::QueryContext context(cancelled);
    const auto shared = cancelled;
    shared.requestStop(); cancelled.requestStop();
    ASSERT_TRUE(cancelled.stopRequested());
    ASSERT_TRUE(queryCancelled([&] { context.checkpoint(); }, false));
    ASSERT_TRUE(queryCancelled([&] { context.waitFor(std::chrono::seconds(30)); }, false));
    mcp::QueryContext createdAfterStop(shared);
    ASSERT_TRUE(queryCancelled([&] { createdAfterStop.checkpoint(); }, false));
    ASSERT_FALSE(independent.stopRequested());
    mcp::QueryContext(independent).waitFor(std::chrono::milliseconds(1));
    mcp::QueryContext().checkpoint();
}
TEST(Mcp, QueryCancellationWakesAllWaitersWithoutWaitingForTheirDeadlines) {
    mcp::QueryCancellation cancellation;
    std::vector<std::future<bool>> results;
    std::vector<std::future<void>> started;
    for (int i = 0; i < 4; ++i) {
        std::promise<void> ready;
        started.push_back(ready.get_future());
        results.push_back(std::async(std::launch::async, [cancellation, ready = std::move(ready)]() mutable {
            mcp::QueryContext context(cancellation);
            ready.set_value();
            return queryCancelled([&] { context.waitFor(std::chrono::seconds(30)); }, false);
        }));
    }
    for (auto& ready : started) ready.wait();
    cancellation.requestStop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (auto& result : results) {
        EXPECT_TRUE(result.wait_until(deadline) == std::future_status::ready);
        EXPECT_TRUE(result.get());
    }
}
TEST(Mcp, QueryCancellationRacingWithWaitDoesNotLoseWakeup) {
    for (int i = 0; i < 32; ++i) {
        mcp::QueryCancellation cancellation;
        std::promise<void> go;
        auto gate = go.get_future();
        auto result = std::async(std::launch::async, [cancellation, gate = std::move(gate)]() mutable {
            mcp::QueryContext context(cancellation);
            gate.wait();
            return queryCancelled([&] { context.waitFor(std::chrono::seconds(30)); }, false);
        });
        go.set_value();
        cancellation.requestStop();
        EXPECT_TRUE(result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        ASSERT_TRUE(result.get());
    }
}
TEST(Mcp, QueryContextKeepsCancellationAliveAfterOriginalHandleIsDestroyed) {
    auto context = [] {
        mcp::QueryCancellation cancellation;
        mcp::QueryContext context(cancellation);
        cancellation.requestStop();
        return context;
    }();
    ASSERT_TRUE(queryCancelled([&] { context.waitFor(std::chrono::seconds(30)); }, false));
    auto active = [] { mcp::QueryCancellation cancellation; return mcp::QueryContext(cancellation); }();
    active.waitFor(std::chrono::milliseconds(1));
    active.checkpoint();
}
TEST(Mcp, QueryWaitHonorsDurationAndBudgetWithoutCancellingOtherContexts) {
    mcp::QueryCancellation cancellation;
    mcp::QueryContext active(cancellation);
    const auto start = std::chrono::steady_clock::now();
    active.waitFor(std::chrono::milliseconds(20));
    ASSERT_TRUE(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(20));
    active.waitFor(std::chrono::milliseconds(0));
    mcp::QueryContext expired(cancellation, std::chrono::milliseconds(30));
    ASSERT_TRUE(queryCancelled([&] { expired.waitFor(std::chrono::seconds(30)); }, true));
    ASSERT_FALSE(cancellation.stopRequested());
    active.checkpoint();
    // If shutdown and timeout are both visible, shutdown keeps its existing priority.
    cancellation.requestStop();
    ASSERT_TRUE(queryCancelled([&] { expired.checkpoint(); }, false));
}
TEST(Mcp, DisabledUntilExplicitlyEnabledAndNoPasswordOrApiKeyBypass) {
    McpFixture f; const auto grant = f.grant(); auto request = f.request(grant);
    ASSERT_EQ(f.service->handle(request).status, 404);
    f.enable(); request.headers.erase("authorization"); request.headers["x-api-key"] = "other-secret";
    request.headers["cookie"] = "dice_session=other-secret";
    ASSERT_EQ(f.service->handle(request).status, 401);
    request = f.request(grant); ASSERT_EQ(f.service->handle(request).status, 200);
}
TEST(Mcp, ManagementShowsServerClockAndOnlyRetainedSuccessfulToolUsage) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_status"});
    auto state = f.service->settings();
    ASSERT_TRUE(state["server_time"].get<int64_t>() >= std::time(nullptr) - 2);
    ASSERT_TRUE(state["usage_available"].get<bool>());
    ASSERT_TRUE(state["grants"][0]["last_used_at"].is_null());
    ASSERT_EQ(f.service->handle(f.request(grant, "tools/list", J::object())).status, 200);
    ASSERT_TRUE(f.service->settings()["grants"][0]["last_used_at"].is_null());
    ASSERT_EQ(f.service->handle(f.request(grant)).status, 200);
    const auto called = f.service->settings()["grants"][0]["last_used_at"].get<int64_t>();
    ASSERT_TRUE(called >= std::time(nullptr) - 2);
    f.service->rotateGrant(grant["id"]);
    ASSERT_EQ(f.service->settings()["grants"][0]["last_used_at"].get<int64_t>(), called);
}
TEST(Mcp, LocalSelfCheckUsesExistingProtocolWithoutExecutingToolsOrKeepingFakeRows) {
    McpFixture f; const auto grant = f.grant({"get_status"});
    auto check = f.service->selfCheck(J::object());
    ASSERT_EQ(check["scope"].get<std::string>(), "local_protocol");
    ASSERT_EQ(check["checks"][0]["detail"].get<std::string>(), "not_enabled");
    f.enable(); const auto count = f.service->audit(1, 50)["total"];
    check = f.service->selfCheck({{"token", grant["token"]}, {"tool", "get_status"}});
    for (const auto& row : check["checks"]) ASSERT_EQ(row["state"].get<std::string>(), "pass");
    ASSERT_EQ(check["tools"], J::array({"get_status"}));
    ASSERT_EQ(f.service->audit(1, 50)["total"], count);
    ASSERT_TRUE(f.service->settings()["grants"][0]["last_used_at"].is_null());
    ASSERT_EQ(f.reads, 0);
    ASSERT_TRUE(check.dump().find(grant["token"].get<std::string>()) == std::string::npos);
    check = f.service->selfCheck({{"token", grant["token"]}, {"tool", "roll_dice"}});
    ASSERT_EQ(check["checks"].back()["detail"].get<std::string>(), "tool_denied");
    ASSERT_TRUE(throwsInvalid([&] { f.service->selfCheck({{"url", "http://internal/"}}); }));
    ASSERT_TRUE(throwsInvalid([&] { f.service->selfCheck({{"token", std::string(71, 'x')}}); }));
    check = f.service->selfCheck({{"token", "invalid"}});
    ASSERT_EQ(check["checks"].back()["detail"].get<std::string>(), "credential_invalid");
    f.service->revokeGrant(grant["id"]);
    check = f.service->selfCheck({{"token", grant["token"]}});
    ASSERT_EQ(check["checks"][2]["detail"].get<std::string>(), "no_usable_grants");
    ASSERT_EQ(check["checks"].back()["detail"].get<std::string>(), "credential_invalid");
    f.service->stop();
    ASSERT_EQ(f.service->selfCheck(J::object())["checks"][0]["detail"].get<std::string>(), "stopping");
}
TEST(Mcp, AuditFaultIsExplicitInUsageAndLocalSelfCheckButDoesNotBlockRevocation) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    const auto blocked = f.root / "audit-directory"; std::filesystem::create_directory(blocked);
    mcp::Service service(f.config, blocked.string(), f.callbacks());
    const auto settings = service.settings();
    ASSERT_FALSE(settings["usage_available"].get<bool>());
    ASSERT_TRUE(settings["grants"][0]["last_used_at"].is_null());
    const auto check = service.selfCheck({{"token", grant["token"]}});
    ASSERT_EQ(check["checks"][1]["detail"].get<std::string>(), "audit_unavailable");
    ASSERT_EQ(check["checks"].back()["detail"].get<std::string>(), "service_not_ready");
    service.revokeGrant(grant["id"]);
    ASSERT_TRUE(service.settings()["grants"].empty());
}
TEST(Mcp, SelectedCardSummariesIncludeOwnershipAndInvalidStateButNeverAttributes) {
    McpFixture f; const auto grant = f.grant({"get_character_card"}, {7});
    auto cards = f.service->cardDetails({{"card_ids", {7, 7, 99}}, {"grant_id", grant["id"]}});
    ASSERT_EQ(cards.size(), size_t(2));
    ASSERT_EQ(cards[0]["name"].get<std::string>(), "测试卡");
    ASSERT_EQ(cards[0]["user_id"].get<std::string>(), "user-a");
    ASSERT_EQ(cards[0]["state"].get<std::string>(), "available");
    ASSERT_FALSE(cards[0].contains("attributes"));
    ASSERT_EQ(cards[1]["state"].get<std::string>(), "unavailable");
    ASSERT_FALSE(cards[1].contains("user_id"));
    f.owner = "new-owner";
    cards = f.service->cardDetails({{"card_ids", {7}}, {"grant_id", grant["id"]}});
    ASSERT_EQ(cards[0]["state"].get<std::string>(), "owner_changed");
    f.unavailable = true;
    ASSERT_EQ(f.service->cardDetails({{"card_ids", {7}}})[0]["state"].get<std::string>(), "unavailable");
    for (const auto& ids : {J::array({0}), J::array({1.5}), J::array({2147483648LL}), J::array({"7"}), J::object()})
        ASSERT_TRUE(throwsInvalid([&] { f.service->cardDetails({{"card_ids", ids}}); }));
    ASSERT_TRUE(throwsInvalid([&] { f.service->cardDetails({{"card_ids", {7}}, {"grant_id", "missing"}}); }));
    J ids = J::array(); for (int id = 1; id <= 101; ++id) ids.push_back(id);
    ASSERT_TRUE(throwsInvalid([&] { f.service->cardDetails({{"card_ids", ids}}); }));
}
TEST(Mcp, ManagementWritesRejectAmbientCrossSiteCredentialsButSupportSameOriginProxies) {
    mcp::Request r;
    r.method = "PUT"; r.headers = {{"host", "192.0.2.1:18088"}, {"content-type", "application/json"}};
    ASSERT_EQ(mcp::Service::validateManagement(r, true).status, 204);
    ASSERT_EQ(mcp::Service::validateManagement(r, false).status, 403);
    r.headers["origin"] = "http://192.0.2.1:18088";
    ASSERT_EQ(mcp::Service::validateManagement(r, true).status, 403);
    r.headers["x-dice-mcp-request"] = "webui";
    ASSERT_EQ(mcp::Service::validateManagement(r, false).status, 204);
    r.headers["origin"] = "https://evil.example";
    ASSERT_EQ(mcp::Service::validateManagement(r, false).status, 403);
    r.headers["sec-fetch-site"] = "cross-site";
    ASSERT_EQ(mcp::Service::validateManagement(r, true).status, 403);
    r.headers["sec-fetch-site"] = "same-site";
    ASSERT_EQ(mcp::Service::validateManagement(r, true).status, 403);
    r.headers["sec-fetch-site"] = "same-origin"; r.headers["origin"] = "https://public.example";
    ASSERT_EQ(mcp::Service::validateManagement(r, false).status, 204);
    for (const char* bad : {"null", "", "https://public.example/path", "https://user@public.example"}) {
        r.headers["origin"] = bad; ASSERT_EQ(mcp::Service::validateManagement(r, true).status, 403);
    }
    r.headers["origin"] = "https://public.example"; r.headers["content-type"] = "text/plain";
    ASSERT_EQ(mcp::Service::validateManagement(r, true).status, 415);
    r.method = "GET"; ASSERT_EQ(mcp::Service::validateManagement(r, false).status, 204);
}
TEST(Mcp, CommittedCredentialsRemainAvailableEvenIfAuditStorageIsUnavailable) {
    McpFixture f;
    const auto blocked = f.root / "blocked-audit"; std::filesystem::create_directory(blocked);
    mcp::Service service(f.config, blocked.string(), f.callbacks());
    const auto configured = service.configure({{"enabled", true}});
    ASSERT_TRUE(configured.contains("warnings"));
    const auto grant = service.createGrant({{"name", "audit failure"}, {"tools", {"get_status"}}});
    ASSERT_TRUE(grant.contains("token")); ASSERT_TRUE(grant.contains("warnings"));
    ASSERT_EQ(service.settings()["grants"].size(), size_t(1));
    const auto rotated = service.rotateGrant(grant["id"]);
    ASSERT_NE(rotated["token"].get<std::string>(), grant["token"].get<std::string>());
    ASSERT_TRUE(rotated.contains("warnings"));
    ConfigManager reloaded((f.root / "config").string()); ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.get<J>("server/mcp/grants").size(), size_t(1));
    ASSERT_TRUE(service.revokeGrant(grant["id"]).contains("warnings"));
    ASSERT_EQ(service.settings()["grants"].size(), size_t(0));
}
TEST(Mcp, AtomicSettingsFailurePreservesMemoryAndDiskWithoutRewritingOtherSections) {
    McpFixture f; const auto before = f.config.get<J>("server/mcp");
    const auto blocked = f.root / "config" / "server.json.tmp";
    std::filesystem::create_directory(blocked);
    bool failed = false;
    try { f.enable(); } catch (const std::runtime_error&) { failed = true; }
    ASSERT_TRUE(failed); ASSERT_EQ(f.config.get<J>("server/mcp"), before);
    ConfigManager reloaded((f.root / "config").string()); ASSERT_TRUE(reloaded.load());
    ASSERT_EQ(reloaded.get<J>("server/mcp"), before);
    std::filesystem::remove(blocked);
    const auto other = f.root / "config" / "dice.json.tmp";
    std::filesystem::create_directory(other);
    f.enable(); ASSERT_TRUE(f.service->settings()["enabled"].get<bool>());
    ASSERT_TRUE(std::filesystem::is_directory(other));
    f.config.setSnapshotWriter([](const J&) { throw std::runtime_error("fixture snapshot failure"); });
    const auto grant = f.grant(); ASSERT_TRUE(grant.contains("token"));
}
TEST(Mcp, RevocationDoesNotWaitForSlowQueriesAndInFlightResultsAreDiscarded) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_status"});
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    auto queries = f.callbacks();
    queries.status = [&](const mcp::QueryContext&) { started.set_value(); gate.wait(); return J{{"private_result", "must not escape"}}; };
    f.service = std::make_unique<mcp::Service>(f.config, (f.root / "audit.db").string(), queries);
    auto result = std::async(std::launch::async, [&] { return f.service->handle(f.request(grant)); });
    started.get_future().wait();
    auto revoke = std::async(std::launch::async, [&] { f.service->revokeGrant(grant["id"]); });
    const bool immediate = revoke.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    release.set_value(); revoke.get(); const auto response = result.get();
    ASSERT_TRUE(immediate); ASSERT_EQ(response.status, 403);
    ASSERT_TRUE(response.body.dump().find("must not escape") == std::string::npos);
}
TEST(Mcp, CooperativeDeadlineDiscardsResultsAndReleasesTheCredentialSlot) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    auto queries = f.callbacks();
    queries.help = [](const std::string&, int, const mcp::QueryContext& context) {
        context.waitFor(std::chrono::seconds(2)); return J{{"secret", "must not escape"}};
    };
    f.service = std::make_unique<mcp::Service>(f.config, (f.root / "audit.db").string(), queries, std::chrono::milliseconds(60));
    const auto start = std::chrono::steady_clock::now();
    const auto response = f.service->handle(f.request(grant, "tools/call", {{"name", "search_help"}, {"arguments", {{"query", "slow"}}}}));
    ASSERT_TRUE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    ASSERT_TRUE(response.body["result"]["isError"].get<bool>());
    ASSERT_TRUE(response.body["result"]["structuredContent"]["timed_out"].get<bool>());
    ASSERT_TRUE(response.body.dump().find("must not escape") == std::string::npos);
    ASSERT_FALSE(f.service->handle(f.request(grant)).body["result"]["isError"].get<bool>());
}
TEST(Mcp, PerCredentialLimitPreservesOtherClientsAndShutdownCancelsActiveWork) {
    McpFixture f; f.enable(); const auto first = f.grant(), other = f.grant();
    std::promise<void> started;
    auto queries = f.callbacks();
    queries.help = [&](const std::string&, int, const mcp::QueryContext& context) {
        started.set_value(); context.waitFor(std::chrono::seconds(30)); return J{{"secret", "must not escape"}};
    };
    f.service = std::make_unique<mcp::Service>(f.config, (f.root / "audit.db").string(), queries);
    auto response = std::async(std::launch::async, [&] { return f.service->handle(f.request(first, "tools/call", {{"name", "search_help"}, {"arguments", {{"query", "slow"}}}})); });
    started.get_future().wait();
    const int limited = f.service->handle(f.request(first)).status;
    const int independent = f.service->handle(f.request(other)).status;
    f.service->stop();
    const bool prompt = response.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    const auto result = response.get();
    ASSERT_EQ(limited, 429); ASSERT_EQ(independent, 200); ASSERT_TRUE(prompt);
    ASSERT_TRUE(result.body["result"]["structuredContent"]["cancelled"].get<bool>());
    ASSERT_FALSE(result.body["result"]["structuredContent"]["timed_out"].get<bool>());
    ASSERT_EQ(f.service->handle(f.request(other)).status, 503);
}
TEST(Mcp, SecretsAreReturnedOnlyOnceAndPersistedAsHash) {
    McpFixture f; const auto grant = f.grant(); const auto token = grant.at("token").get<std::string>();
    ASSERT_EQ(token.size(), size_t(70));
    ASSERT_TRUE(f.service->settings().dump().find(token) == std::string::npos);
    const auto stored = f.config.get<J>("server/mcp");
    ASSERT_TRUE(stored.dump().find(token) == std::string::npos);
    ASSERT_EQ(stored["grants"][0]["token_hash"].get<std::string>().size(), size_t(64));
    ASSERT_FALSE(f.service->settings()["grants"][0].contains("token_hash"));
    ConfigManager reloaded((f.root / "config").string()); ASSERT_TRUE(reloaded.load());
    mcp::Service next(reloaded, (f.root / "audit.db").string(), f.callbacks());
    next.configure({{"enabled", true}}); ASSERT_EQ(next.handle(f.request(grant)).status, 200);
}
TEST(Mcp, TokenRevocationAndExpiryApplyOnEveryRequest) {
    McpFixture f; f.enable(); const auto grant = f.grant(); const auto req = f.request(grant);
    ASSERT_EQ(f.service->handle(req).status, 200);
    f.service->revokeGrant(grant["id"]); ASSERT_EQ(f.service->handle(req).status, 401);
    const auto second = f.grant(); auto conf = f.config.get<J>("server/mcp"); conf["grants"][0]["expires_at"] = 0;
    f.config.set<J>("server/mcp", conf); ASSERT_EQ(f.service->handle(f.request(second)).status, 401);
}
TEST(Mcp, HostOriginPeerAndForwardingAreIndependentBoundaries) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    auto req = f.request(grant); req.loopback = false; req.peer = "192.0.2.1";
    ASSERT_EQ(f.service->handle(req).status, 403);
    req = f.request(grant); req.forwarded = true; ASSERT_EQ(f.service->handle(req).status, 403);
    req = f.request(grant); req.headers["host"] = "evil.example:18088"; ASSERT_EQ(f.service->handle(req).status, 403);
    req = f.request(grant); req.headers["origin"] = "https://evil.example"; ASSERT_EQ(f.service->handle(req).status, 403);
    req = f.request(grant); req.headers["origin"] = "null"; ASSERT_EQ(f.service->handle(req).status, 403);
}
TEST(Mcp, RemoteRequiresExplicitHostAndTlsAndDoesNotTrustProxyTlsHeaders) {
    McpFixture f;
    ASSERT_TRUE(throwsInvalid([&] { f.service->configure({{"enabled", true}, {"allow_remote", true}}); }));
    f.service->configure({{"enabled", true}, {"allow_remote", true}, {"allowed_hosts", {"dice.example"}}});
    const auto grant = f.grant(); auto req = f.request(grant);
    req.loopback = false; req.peer = "192.0.2.1"; req.headers["host"] = "dice.example"; req.headers["x-forwarded-proto"] = "https";
    ASSERT_EQ(f.service->handle(req).status, 403);
    req.secure = true; ASSERT_EQ(f.service->handle(req).status, 200);
}
TEST(Mcp, RestrictedSsoCannotReadOrWriteMcpManagement) {
    for (const char* method : {"GET", "HEAD", "POST", "PUT", "DELETE"})
        for (const char* path : {"/api/system/mcp", "/api/system/mcp/grants", "/api/system/mcp/audit", "/api/system/mcp/card-options"})
            ASSERT_TRUE(sso::blockedForRestrictedAdmin(method, path));
    for (const char* path : {"/API/SYSTEM/MCP", "/api/System/Mcp/Audit", "/API/PLUGINS/JS"})
        ASSERT_TRUE(sso::blockedForRestrictedAdmin("GET", path));
}
TEST(Mcp, ToolListAndDirectCallsUseTheSameCredentialPermissions) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_status"});
    const auto list = f.service->handle(f.request(grant, "tools/list", J::object()));
    ASSERT_EQ(list.body["result"]["tools"].size(), size_t(1));
    ASSERT_EQ(list.body["result"]["tools"][0]["name"].get<std::string>(), std::string("get_status"));
    ASSERT_EQ(f.service->handle(f.request(grant, "tools/call", {{"name", "roll_dice"}, {"arguments", {{"expression", "d100"}}}})).status, 403);
    ASSERT_EQ(f.service->handle(f.request(grant, "tools/call", {{"name", "run_command"}, {"arguments", {{"command", ".bot on"}}}})).status, 400);
}
TEST(Mcp, ExactCardGrantsCannotBeSpoofedByUserIdOrOtherCardOrReusedOwner) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_character_card"}, {7}); const int baseline = f.reads;
    auto invoke = [&](J args) { return f.service->handle(f.request(grant, "tools/call", {{"name", "get_character_card"}, {"arguments", args}})); };
    ASSERT_EQ(invoke({{"card_id", 8}}).status, 403); ASSERT_EQ(f.reads, baseline);
    auto spoof = invoke({{"card_id", 7}, {"user_id", "admin"}}); ASSERT_TRUE(spoof.body["result"]["isError"].get<bool>());
    ASSERT_EQ(f.reads, baseline);
    auto good = invoke({{"card_id", 7}}); ASSERT_EQ(good.body["result"]["structuredContent"]["attributes"]["理智"].get<int>(), 70);
    f.owner = "another-user"; auto replaced = invoke({{"card_id", 7}}); ASSERT_TRUE(replaced.body["result"]["isError"].get<bool>());
    ASSERT_FALSE(replaced.body["result"].dump().find("attributes") != std::string::npos);
}
TEST(Mcp, CardGrantMustBeExplicitAndExistingAndRespectsReadLock) {
    McpFixture f;
    ASSERT_TRUE(throwsInvalid([&] { f.grant({"get_character_card"}); }));
    ASSERT_TRUE(throwsInvalid([&] { f.grant({"get_character_card"}, {99}); }));
    ASSERT_TRUE(throwsInvalid([&] { f.grant({"set_attr"}); }));
}
TEST(Mcp, LegacyInitializationAndStatelessToolsWorkWithoutSessionCookie) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    auto req = f.request(grant, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", J::object()}, {"clientInfo", {{"name", "test"}, {"version", "1"}}}});
    req.headers.erase("mcp-protocol-version"); auto init = f.service->handle(req);
    ASSERT_EQ(init.body["result"]["protocolVersion"].get<std::string>(), std::string("2025-11-25"));
    ASSERT_TRUE(init.headers.find("Mcp-Session-Id") == init.headers.end());
    req = f.request(grant, "notifications/initialized", J::object()); auto body = J::parse(req.body); body.erase("id"); req.body = body.dump();
    const auto note = f.service->handle(req); ASSERT_EQ(note.status, 202); ASSERT_TRUE(note.body.is_null());
    ASSERT_EQ(f.service->handle(f.request(grant)).status, 200);
}
TEST(Mcp, ModernDiscoveryRequiresPerRequestMetadataAndMatchingHeaders) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    auto req = f.request(grant, "server/discover", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", J::object()}}}});
    req.headers["mcp-protocol-version"] = "2026-07-28"; req.headers["mcp-method"] = "server/discover";
    const auto result = f.service->handle(req); ASSERT_EQ(result.status, 200);
    ASSERT_EQ(result.body["result"]["resultType"].get<std::string>(), std::string("complete"));
    ASSERT_EQ(result.body["result"]["cacheScope"].get<std::string>(), std::string("private"));
    ASSERT_EQ(result.body["result"]["ttlMs"].get<int>(), 0);
    req.headers["mcp-method"] = "tools/call"; auto mismatch = f.service->handle(req);
    ASSERT_EQ(mismatch.status, 400); ASSERT_EQ(mismatch.body["error"]["code"].get<int>(), -32020);
    req.headers["mcp-protocol-version"] = "2099-01-01";
    ASSERT_EQ(f.service->handle(req).body["error"]["code"].get<int>(), -32020);
    auto body = J::parse(req.body); body["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01"; req.body = body.dump();
    const auto unsupported = f.service->handle(req);
    ASSERT_EQ(unsupported.body["error"]["code"].get<int>(), -32022);
    ASSERT_EQ(unsupported.body["error"]["data"]["requested"].get<std::string>(), std::string("2099-01-01"));
    req = f.request(grant, "tools/list", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", J::object()}}}});
    req.headers["mcp-protocol-version"] = "2026-07-28"; req.headers["mcp-method"] = "tools/list";
    const auto listed = f.service->handle(req);
    ASSERT_EQ(listed.body["result"]["cacheScope"].get<std::string>(), std::string("private"));
    ASSERT_EQ(listed.body["result"]["ttlMs"].get<int>(), 0);
}
TEST(Mcp, ActualRollsReturnStructuredRandomResultsAndBlockExecutionAndHugeWork) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    auto call = [&](const std::string& expr) { return f.service->handle(f.request(grant, "tools/call", {{"name", "roll_dice"}, {"arguments", {{"expression", expr}}}})); };
    const auto ok = call("2d6+3"); const auto data = ok.body["result"]["structuredContent"];
    ASSERT_FALSE(ok.body["result"]["isError"].get<bool>());
    ASSERT_TRUE(data["result"].get<int>() >= 5 && data["result"].get<int>() <= 15);
    ASSERT_EQ(data["individual_results"].size(), size_t(2));
    ASSERT_TRUE(call(".bot on").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call("{api:https://example.com}").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call("100000d100").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call("2147483647+1").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call("(-2147483647-1)/-1").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call("1000000*1000000").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call("d2147483647").body["result"]["isError"].get<bool>());
    ASSERT_TRUE(call(std::string(30, '(') + "d6" + std::string(30, ')')).body["result"]["isError"].get<bool>());
}
TEST(Mcp, StrictHttpAndRpcValidationNeverExecutesMalformedRequests) {
    McpFixture f; f.enable(); const auto grant = f.grant(); auto req = f.request(grant);
    req.method = "GET"; ASSERT_EQ(f.service->handle(req).status, 405);
    req = f.request(grant); req.headers["content-type"] = "text/plain"; ASSERT_EQ(f.service->handle(req).status, 415);
    req = f.request(grant); req.headers["accept"] = "application/json"; ASSERT_EQ(f.service->handle(req).status, 406);
    req = f.request(grant); req.body = "{"; ASSERT_EQ(f.service->handle(req).body["error"]["code"].get<int>(), -32700);
    req.body = "[]"; ASSERT_EQ(f.service->handle(req).status, 400);
    req.body = std::string(32769, ' '); ASSERT_EQ(f.service->handle(req).status, 413);
    req = f.request(grant); auto body = J::parse(req.body); body.erase("id"); req.body = body.dump();
    ASSERT_EQ(f.service->handle(req).status, 400);
    body["id"] = nullptr; req.body = body.dump(); ASSERT_EQ(f.service->handle(req).status, 400);
}
TEST(Mcp, AuditIsPersistentPaginatedAndNeverContainsTokensArgumentsOrCardContents) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    ASSERT_EQ(f.service->handle(f.request(grant)).status, 200);
    auto page = f.service->audit(999, 1); ASSERT_EQ(page["items"].size(), size_t(1)); ASSERT_EQ(page["pageSize"].get<int>(), 1);
    const auto all = f.service->audit(1, 50).dump();
    ASSERT_TRUE(all.find(grant["token"].get<std::string>()) == std::string::npos);
    ASSERT_TRUE(all.find("token_hash") == std::string::npos);
    mcp::Service next(f.config, (f.root / "audit.db").string(), f.callbacks()); ASSERT_TRUE(next.audit(1, 50)["total"].get<int>() >= 3);
}
TEST(Mcp, PerCredentialRateLimitBoundsRequests) {
    McpFixture f; f.enable(); const auto grant = f.grant(); const auto req = f.request(grant, "ping", J::object());
    for (int i = 0; i < 60; ++i) ASSERT_EQ(f.service->handle(req).status, 200);
    const auto limited = f.service->handle(req); ASSERT_EQ(limited.status, 429); ASSERT_TRUE(limited.headers.count("Retry-After") == 1);
}
TEST(Mcp, ExplicitBrowserOriginsSupportPreflightButNeverCookieCredentials) {
    McpFixture f;
    f.service->configure({{"enabled", true}, {"allowed_origins", {"https://client.example"}}});
    const auto grant = f.grant(); auto req = f.request(grant);
    req.method = "OPTIONS"; req.headers.erase("authorization"); req.headers["origin"] = "https://client.example";
    const auto preflight = f.service->handle(req); ASSERT_EQ(preflight.status, 204);
    ASSERT_EQ(preflight.headers.at("Access-Control-Allow-Origin"), std::string("https://client.example"));
    ASSERT_TRUE(preflight.headers.count("Access-Control-Allow-Credentials") == 0);
    req.method = "POST"; ASSERT_EQ(f.service->handle(req).status, 401);
    req = f.request(grant); req.headers["origin"] = "https://client.example";
    ASSERT_EQ(f.service->handle(req).headers.at("Access-Control-Allow-Origin"), std::string("https://client.example"));
}
TEST(Mcp, OriginAndHostValidationRejectsWildcardsPathsAndLocalhostLookalikes) {
    McpFixture f;
    for (const char* host : {"*.example.com", "https://dice.example", "dice.example/path", "dice.example:999999", "user@dice.example"})
        ASSERT_TRUE(throwsInvalid([&] { f.service->configure({{"allowed_hosts", {host}}}); }));
    for (const char* origin : {"http://localhost.evil.example", "http://127.0.0.11", "https://client.example/path", "https://user@client.example", "null"})
        ASSERT_TRUE(throwsInvalid([&] { f.service->configure({{"allowed_origins", {origin}}}); }));
    f.service->configure({{"allowed_origins", {"http://localhost:3000", "https://client.example"}}});
}
TEST(Mcp, AuditBackupIncludesLiveWalAndCannotOverwriteSource) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    ASSERT_EQ(f.service->handle(f.request(grant)).status, 200);
    const auto before = f.service->audit(1, 50)["total"].get<int>();
    const auto target = f.root / "snapshot" / "audit.db";
    f.service->snapshotAudit(target);
    mcp::Service restored(f.config, target.string(), f.callbacks());
    ASSERT_EQ(restored.audit(1, 50)["total"].get<int>(), before);
    ASSERT_TRUE(throwsInvalid([&] { f.service->snapshotAudit(f.root / "audit.db"); }));
    ASSERT_TRUE(throwsInvalid([&] { f.service->snapshotAudit(f.root / "same-directory.db"); }));
    ASSERT_EQ(f.service->audit(1, 50)["total"].get<int>(), before);
}
TEST(Mcp, SerializedResultLimitIncludesBothContentRepresentations) {
    McpFixture f; f.enable(); const auto grant = f.grant();
    auto queries = f.callbacks();
    queries.help = [](const std::string&, int, const mcp::QueryContext&) { return J{{"content", std::string(40000, 'x')}}; };
    mcp::Service large(f.config, (f.root / "large.db").string(), std::move(queries));
    const auto result = large.handle(f.request(grant, "tools/call", {{"name", "search_help"}, {"arguments", {{"query", "roll"}}}}));
    ASSERT_TRUE(result.body["result"]["isError"].get<bool>());
    ASSERT_TRUE(result.body.dump().size() < size_t(65536));
}
TEST(Mcp, RegenerationImmediatelyInvalidatesOnlyTheOldTokenWithoutChangingScopeOrExpiry) {
    McpFixture f; f.enable(); const auto old = f.grant({"get_character_card"}, {7}); const auto other = f.grant({"get_status"});
    const auto before = f.config.get<J>("server/mcp")["grants"][0];
    const auto next = f.service->rotateGrant(old["id"]);
    ASSERT_NE(next["token"].get<std::string>(), old["token"].get<std::string>());
    ASSERT_TRUE(next["id"] == old["id"] && next["expires_at"] == old["expires_at"] && next["tools"] == old["tools"]);
    ASSERT_TRUE(next["card_owners"] == before["card_owners"] && next["enabled"] == before["enabled"]);
    ASSERT_EQ(f.service->handle(f.request(old, "tools/list", J::object())).status, 401);
    ASSERT_EQ(f.service->handle(f.request(next, "tools/list", J::object())).status, 200);
    ASSERT_EQ(f.service->handle(f.request(other)).status, 200);
    ASSERT_FALSE(f.service->settings().dump().find(next["token"].get<std::string>()) != std::string::npos);
    ASSERT_FALSE(f.service->audit(1, 50).dump().find(next["token"].get<std::string>()) != std::string::npos);
}
TEST(Mcp, EditingAndRotatingExpiredCredentialsNeverSilentlyRenewsThem) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_status"});
    auto config = f.config.get<J>("server/mcp"); config["grants"][0]["expires_at"] = 17; f.config.set<J>("server/mcp", config);
    const auto edited = f.service->updateGrant(grant["id"], {{"name", "新标签"}, {"tools", {"get_status"}}});
    ASSERT_EQ(edited["expires_at"].get<int>(), 17);
    const auto rotated = f.service->rotateGrant(grant["id"]);
    ASSERT_EQ(rotated["expires_at"].get<int>(), 17); ASSERT_EQ(f.service->handle(f.request(rotated)).status, 401);
    const auto renewed = f.service->updateGrant(grant["id"], {{"name", "新标签"}, {"tools", {"get_status"}}, {"expires_days", 3}});
    ASSERT_TRUE(renewed["expires_at"].get<int64_t>() > int64_t(std::time(nullptr)) + 2 * 86400);
    ASSERT_EQ(f.service->handle(f.request(rotated)).status, 200);
}
TEST(Mcp, StaleCardsCannotPreventDisablingOrRemovingTheirPermissions) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_character_card"}, {7}); f.unavailable = true;
    auto request = f.request(grant, "tools/call", {{"name", "get_character_card"}, {"arguments", {{"card_id", 7}}}});
    ASSERT_TRUE(f.service->handle(request).body["result"]["isError"].get<bool>());
    const int reads = f.reads;
    f.service->updateGrant(grant["id"], {{"name", "disabled"}, {"tools", {"get_character_card"}}, {"card_ids", {7}}, {"enabled", false}});
    ASSERT_EQ(f.reads, reads); ASSERT_EQ(f.service->handle(request).status, 401);
    const auto rotated = f.service->rotateGrant(grant["id"]);
    ASSERT_EQ(f.service->handle(f.request(rotated)).status, 401);
    ASSERT_TRUE(throwsInvalid([&] { f.service->updateGrant(grant["id"], {{"name", "retry"}, {"tools", {"get_character_card"}}, {"card_ids", {7}}, {"enabled", true}}); }));
    const auto removed = f.service->updateGrant(grant["id"], {{"name", "status only"}, {"tools", {"get_status"}}, {"card_ids", {7}}, {"enabled", true}});
    ASSERT_TRUE(removed["card_ids"].empty()); ASSERT_EQ(f.service->handle(f.request(rotated)).status, 200);
    f.service->revokeGrant(grant["id"]); ASSERT_EQ(f.service->handle(f.request(rotated)).status, 401);
}
TEST(Mcp, EditingDoesNotSilentlyReauthorizeACardTransferredToAnotherOwner) {
    McpFixture f; f.enable(); const auto grant = f.grant({"get_character_card"}, {7}); f.owner = "user-b";
    const J unchanged{{"name", "rename only"}, {"tools", {"get_character_card"}}, {"card_ids", {7}}};
    ASSERT_TRUE(throwsInvalid([&] { f.service->updateGrant(grant["id"], unchanged); }));
    auto disabled = unchanged; disabled["enabled"] = false; f.service->updateGrant(grant["id"], disabled);
    ASSERT_EQ(f.service->settings()["grants"][0]["card_owners"]["7"].get<std::string>(), std::string("user-a"));
    ASSERT_TRUE(throwsInvalid([&] { f.service->updateGrant(grant["id"], unchanged); }));
    f.service->updateGrant(grant["id"], {{"name", "explicit removal"}, {"tools", {"get_status"}}});
    f.service->updateGrant(grant["id"], unchanged);
    ASSERT_EQ(f.service->settings()["grants"][0]["card_owners"]["7"].get<std::string>(), std::string("user-b"));
}
TEST(Mcp, AuditFiltersApplyBeforeCountingAndPaginationAndIncludeRevokedClients) {
    McpFixture f; f.enable(); const auto one = f.grant({"get_status"}), two = f.grant({"get_status"});
    f.service->handle(f.request(one)); f.service->handle(f.request(two));
    f.service->handle(f.request(one, "tools/call", {{"name", "roll_dice"}, {"arguments", {{"expression", "d6"}}}}));
    f.service->handle(f.request(one, "tools/call", {{"name", "get_status"}, {"arguments", {{"unexpected", true}}}}));
    const auto id = one["id"].get<std::string>();
    const auto first = f.service->audit(99, 1, id, "denied");
    ASSERT_EQ(first["total"].get<int>(), 1); ASSERT_EQ(first["page"].get<int>(), 1);
    ASSERT_EQ(first["items"][0]["client_id"].get<std::string>(), id);
    ASSERT_EQ(f.service->audit(1, 50, id, "error")["total"].get<int>(), 1);
    const int total = f.service->audit(1, 50, id)["total"].get<int>();
    f.service->revokeGrant(id); ASSERT_EQ(f.service->audit(1, 50, id)["total"].get<int>(), total + 1);
    const auto filtered = f.service->audit(1, 50, id);
    for (const auto& item : filtered["items"]) ASSERT_EQ(item["client_id"].get<std::string>(), id);
    ASSERT_TRUE(throwsInvalid([&] { f.service->audit(1, 20, "' OR 1=1 --"); }));
    ASSERT_TRUE(throwsInvalid([&] { f.service->audit(1, 20, "", "unexpected"); }));
}
TEST(Mcp, CaseAndDefaultOriginPortsAreNormalizedWithoutAllowingOtherHosts) {
    McpFixture f;
    const auto saved = f.service->configure({{"enabled", true}, {"allow_remote", true}, {"allowed_hosts", {"DICE.Example"}}, {"allowed_origins", {"HTTPS://CLIENT.Example:443"}}});
    ASSERT_EQ(saved["allowed_hosts"][0].get<std::string>(), std::string("dice.example"));
    ASSERT_EQ(saved["allowed_origins"][0].get<std::string>(), std::string("https://client.example"));
    const auto grant = f.grant(); auto request = f.request(grant); request.secure = true; request.loopback = false;
    request.headers["host"] = "Dice.Example"; request.headers["origin"] = "https://CLIENT.example";
    ASSERT_EQ(f.service->handle(request).status, 200);
    request.headers["origin"] = "https://client.example:444"; ASSERT_EQ(f.service->handle(request).status, 403);
    request.headers["origin"] = ""; ASSERT_EQ(f.service->handle(request).status, 403);
    for (const char* host : {"a..example", "a.-example", "[::::]", "[1:2:3]"})
        ASSERT_TRUE(throwsInvalid([&] { f.service->configure({{"allowed_hosts", {host}}}); }));
    f.service->configure({{"enabled", true}}); f.config.set<int>("server/port", 80);
    request = f.request(grant); request.headers["host"] = "LOCALHOST";
    ASSERT_EQ(f.service->handle(request).status, 200);
}
TEST(Mcp, HttpMediaTypesHonorCaseTokensAndQualityRatherThanSubstringMatching) {
    McpFixture f; f.enable(); const auto grant = f.grant(); auto request = f.request(grant);
    request.headers["content-type"] = "Application/JSON ; Charset=UTF-8";
    request.headers["accept"] = "Application/JSON;q=0.8, Text/Event-Stream; q=1.000";
    ASSERT_EQ(f.service->handle(request).status, 200);
    for (const char* accept : {"xapplication/json,text/event-stream", "application/json,text/event-stream;q=0", "application/json;q=1.1,text/event-stream", "application/json;q=0;q=1,text/event-stream"}) {
        request.headers["accept"] = accept; ASSERT_EQ(f.service->handle(request).status, 406);
    }
}
