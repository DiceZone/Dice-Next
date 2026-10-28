// Isolated transport fixture. Never starts a bot, adapters, plugins or updater.
#include "../src/service/mcp_service.h"
#include "../src/service/http_auth_path.h"
#include "../src/config/config_manager.h"
#include "../src/common/logger.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <atomic>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const int port = std::stoi(argv[2]);
        if (port < 1024 || port > 65535) return 2;
        dice::initLogger("error");
        dice::ConfigManager config((std::filesystem::path(argv[1]) / "config").string());
        if (!config.load()) return 3;
        config.set<int>("server/port", port);
        std::atomic<int> activeQueries{0};
        dice::mcp::Queries queries{
            [](const dice::mcp::QueryContext&) { return dice::mcp::Json{{"status", "running"}, {"version", "transport-fixture"}}; },
            [&](const std::string& q, int limit, const dice::mcp::QueryContext& context) {
                ++activeQueries;
                struct Scope { std::atomic<int>& value; ~Scope() { --value; } } scope{activeQueries};
                if (q == "fixture-slow") context.waitFor(std::chrono::milliseconds(1800));
                if (q == "fixture-cancel") context.waitFor(std::chrono::seconds(30));
                return dice::mcp::Json{{"query", q}, {"limit", limit}};
            },
            [](int id, const dice::mcp::QueryContext& context) {
                if (id == 42) context.waitFor(std::chrono::milliseconds(500));
                return dice::mcp::Json{{"card_id", id}, {"user_id", "fixture-user"}, {"name", "卡片"}, {"attributes", {{"理智", 70}}}};
            },
            [](const std::string&, const dice::mcp::QueryContext&) { return dice::mcp::Json::array({{{"card_id", 7}, {"user_id", "fixture-user"}, {"name", "卡片"}}}); }
        };
        queries.version = "transport-fixture";
        auto service = std::make_shared<dice::mcp::Service>(config, (std::filesystem::path(argv[1]) / "audit.db").string(), std::move(queries));
        service->configure({{"enabled", true}, {"allowed_origins", {"https://fixture.example"}}});
        auto grant = service->createGrant({{"name", "HTTP fixture"}, {"tools", {"roll_dice", "get_status", "search_help", "get_character_card"}}, {"card_ids", {7}}});
        auto& app = drogon::app();
        app.setLogLevel(trantor::Logger::kError);
        app.setClientMaxBodySize(65536).setClientMaxMemoryBodySize(65536);
        app.addListener("127.0.0.1", static_cast<uint16_t>(port));
        app.setThreadNum(2);
        // Mirrors the production management boundary; MCP's Bearer does not
        // authenticate an /api request. This fixture key is not a product key.
        app.registerPreHandlingAdvice([](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& stop, drogon::AdviceChainCallback&& next) {
            // Test-only proof that a missed middleware match never grants MCP
            // management access. This fixture executable is never shipped.
            if (req->getHeader("X-Fixture-Skip-Auth") == "1") { next(); return; }
            const bool key = req->getHeader("X-API-Key") == "transport-fixture-owner";
            const bool session = req->getCookie("fixture-session") == "owner";
            if (dice::authRoutePath(req->path()).rfind("/api/", 0) == 0 && !key && !session) {
                auto response = drogon::HttpResponse::newHttpResponse(); response->setStatusCode(drogon::k401Unauthorized); stop(response); return;
            }
            if (key || session) {
                req->attributes()->insert("dn_api_authenticated", true);
                req->attributes()->insert("dn_auth_type", std::string(key ? "api_key" : "session"));
            }
            if (req->getHeader("X-Fixture-Role") == "admin") req->attributes()->insert("dn_role", std::string("admin"));
            next();
        });
        auto transport = dice::mcp::registerRoutes(service);
        app.registerHandler("/fixture-health", [&](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto response = drogon::HttpResponse::newHttpResponse(); response->setBody("ok");
            response->addHeader("X-Fixture-Active", std::to_string(activeQueries.load())); cb(response);
        }, {drogon::Get});
        app.registerBeginningAdvice([grant] {
            // Captured by the test driver, never written to CI output/log files.
            std::cout << "MCP_FIXTURE_READY " << grant.dump() << std::endl;
        });
        std::thread control([&] { std::string command; std::getline(std::cin, command); app.quit(); });
        app.run(); transport.reset(); control.join(); return 0;
    } catch (const std::exception&) { std::cerr << "MCP transport fixture failed\n"; return 1; }
}
