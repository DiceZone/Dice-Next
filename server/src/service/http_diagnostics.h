#pragma once
#include "../common/logger.h"
#include "../common/log_context.h"
#include "../common/http_log.h"
#include "../common/script_execution.h"
#include <drogon/drogon.h>
#include <atomic>

namespace dice {
inline ProcessingContext httpDiagnosticContext(const drogon::HttpRequestPtr& req) {
    ProcessingOrigin origin;origin.entry="网页后台";
    try {origin.userId=req->attributes()->get<std::string>("dn_qq");} catch (...) {}
    if(!origin.userId.empty())origin.platform="onebot_v11";
    else origin.entry+="（未提供可识别的 QQ 身份）";
    return ProcessingContext{{{"网页后台", "管理请求", "", ""}},
        std::string(req->methodString())+" "+req->path(),"请求处理",origin};
}
inline void installHttpDiagnostics(drogon::HttpAppFramework& app) {
    struct RequestTrace { uint64_t id{}; std::chrono::steady_clock::time_point start; };
    static std::atomic<uint64_t> nextRequest{0};
    app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req) {
        if (req->path().rfind("/api/", 0) != 0) return;
        RequestTrace trace{++nextRequest, std::chrono::steady_clock::now()};
        req->attributes()->insert("dice.diagnostic", trace);
        if (logManagementRequest(req->path(), req->methodString()))
            writeLog(DICE_LOG_SOURCE, LogLevel::Debug, "管理面板", fmt::format("开始处理管理请求：{} {}", req->methodString(), req->path()),
                {{"request_id", trace.id}, {"phase", "begin"}});
    });
    app.setExceptionHandler([](const std::exception& error, const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        ProcessingScope requestScope(httpDiagnosticContext(req));
        const auto& trace = req->attributes()->get<RequestTrace>("dice.diagnostic");
        // No request body, query string, cookies or authorization headers.
        const auto current = std::current_exception();
        writeLog(DICE_LOG_SOURCE, LogLevel::Error, "管理面板", fmt::format("{} {} 处理失败：{}；已返回错误响应。",
            req->methodString(), req->path(), current ? describeException(current) : sanitizeLogText(error.what())),
            {{"request_id", trace.id}});
        auto response = drogon::HttpResponse::newHttpResponse();
        const bool busy = dynamic_cast<const ScriptBusy*>(&error) != nullptr;
        response->setStatusCode(busy ? drogon::k503ServiceUnavailable : drogon::k500InternalServerError);
        response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        response->setBody(busy ? "{\"code\":503,\"message\":\"插件运行时忙碌，请稍后重试\"}"
                              : "{\"code\":500,\"message\":\"internal_error\"}");
        if (busy) response->addHeader("Retry-After", "1");
        callback(response);
    });
    app.registerPreSendingAdvice([](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& response) {
        if (req->path().rfind("/api/", 0) != 0) return;
        const auto& trace = req->attributes()->get<RequestTrace>("dice.diagnostic");
        if (!trace.id) return;
        ProcessingScope requestScope(httpDiagnosticContext(req));
        response->addHeader("X-Dice-Request-Id", std::to_string(trace.id));
        try {
            const auto result = httpLogResult(static_cast<int>(response->statusCode()), response->body());
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - trace.start).count();
            const auto status = static_cast<int>(response->statusCode());
            const bool denied = status == 401 || status == 403;
            auto severity = status >= 500 ? LogLevel::Error : result.failed && !denied ? LogLevel::Info
                : logManagementRequest(req->path(), req->methodString()) ? LogLevel::Info : LogLevel::Debug;
            // Browser polling is debug-only. Slow calls remain visible at warn.
            if (ms >= 2000 && !denied && severity < LogLevel::Warn) severity = LogLevel::Warn;
            writeLog(DICE_LOG_SOURCE, severity, "管理面板",
                fmt::format("{} {} {}；HTTP {}，耗时 {} 毫秒{}", req->methodString(), req->path(),
                    result.failed ? "未完成" : "处理完成", status, ms,
                    req->path().rfind("/api/auth/", 0) == 0 || result.reason.empty() ? "。" : "；原因：" + result.reason),
                {{"request_id", trace.id}, {"http_status", status}, {"business_code", result.code},
                    {"phase", "end"}, {"elapsed_ms", ms}});
        } catch (...) { DICE_LOG_EXCEPTION("HTTP response diagnostics"); }
    });
}
} // namespace dice
