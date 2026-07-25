#pragma once

#include <drogon/HttpSimpleController.h>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

json ok(const json& d) { return {{"code", 0}, {"message", "ok"}, {"data", d}}; }
json err(const std::string& msg, int code = 1) { return {{"code", code}, {"message", msg}, {"data", nullptr}}; }

class MockController : public drogon::HttpSimpleController<MockController> {
public:
    void asyncHandleHttpRequest(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& cb) override
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(dispatch(req));
        cb(resp);
    }

    PATH_LIST_BEGIN
    PATH_ADD("/api/dashboard/stats");
    PATH_ADD("/api/adapters");
    PATH_ADD("/api/adapters/{1}");
    PATH_ADD("/api/adapters/{1}/test");
    PATH_ADD("/api/dice/rules");
    PATH_ADD("/api/dice/roll");
    PATH_ADD("/api/replies");
    PATH_ADD("/api/replies/{1}");
    PATH_ADD("/api/system/status");
    PATH_ADD("/api/system/settings");
    PATH_LIST_END

private:
    json dispatch(const drogon::HttpRequestPtr& req) {
        auto p = req->path();
        auto m = req->method();

        if (p == "/api/system/status")
            return ok({{"status","running"},{"version","3.0.0"},{"uptime",3600}});

        if (p == "/api/system/settings")
            return m == drogon::Put ? ok(nullptr) : ok({{"host","0.0.0.0"},{"port",18088},{"log_level","info"}});

        if (p == "/api/dashboard/stats")
            return ok({{"uptime_seconds", 3600}, {"active_connections", 2}, {"total_rules", 5},
                       {"active_sessions", 1}, {"recent_logs", json::array({
                           {{"timestamp","2026-06-14T02:00:00Z"},{"level","info"},{"message","系统启动"}},
                           {{"timestamp","2026-06-14T02:01:00Z"},{"level","info"},{"message","OneBot 适配器已连接"}}
                       })}});

        if (p == "/api/adapters" && m == drogon::Get)
            return ok(json::array({{{"id","adapter-1"},{"name","我的QQ机器人"},{"type","onebot_v11"},
                        {"connectionMode","forward_ws"},{"endpoint","ws://localhost:6700"},
                        {"accessToken",""},{"enabled",true},{"connected",true}}));

        if (p == "/api/adapters" && m == drogon::Post)
            return ok({{"id","adapter-new"},{"name","新适配器"}});

        if (p.find("/api/adapters/") == 0 && p.find("/test") != std::string::npos)
            return ok({{"success",true},{"message","连接测试成功"}});

        if (p.find("/api/adapters/") == 0)
            return m == drogon::Delete ? ok(nullptr) : ok({{"success",true}});

        if (p == "/api/dice/rules" && m == drogon::Get)
            return ok({{"coc_enabled",true},{"coc_critical_range",1},{"coc_fumble_range",95},
                       {"dnd_enabled",true},{"fate_enabled",false},{"l5r_enabled",false},
                       {"default_dice_sides",100},{"command_prefix","."}});

        if (p == "/api/dice/rules" && m == drogon::Put)
            return ok(nullptr);

        if (p == "/api/dice/roll")
            return ok({{"expression","3d6+2"},{"result","3d6+2 = [4,3,1]+2 = 10"}});

        if (p == "/api/replies" && m == drogon::Get)
            return ok(json::array({
                {{"id",1},{"matchType","keyword"},{"matchContent",".help"},
                 {"replyContent","欢迎使用 Dice!Next!\n命令列表: .r .coc .dnd .help"},
                 {"priority",10},{"enabled",true}},
                {{"id",2},{"matchType","prefix"},{"matchContent","/roll"},
                 {"replyContent","请使用 .r 指令掷骰，如 .r 3d6+2"},
                 {"priority",50},{"enabled",true}},
                {{"id",3},{"matchType","regex"},{"matchContent","^早上好|早安"},
                 {"replyContent","早上好！今天也要元气满满地跑团哦~"},
                 {"priority",100},{"enabled",true}}
            }));

        if (p == "/api/replies" && m == drogon::Post)
            return ok({{"id",99},{"matchType","keyword"}});

        if (p.find("/api/replies/") == 0)
            return m == drogon::Delete ? ok(nullptr) : ok({{"success",true}});

        return err("Not Found", 404);
    }
};
