#pragma once
#include "../common/logger.h"
#include "../common/http_log.h"
#include "../common/script_execution.h"
#include <drogon/drogon.h>
#include <atomic>

namespace dice {
inline void installHttpDiagnostics(drogon::HttpAppFramework& app) {
    struct RequestTrace { uint64_t id{}; std::chrono::steady_clock::time_point start; };
    static std::atomic<uint64_t> nextRequest{0};
    app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req) {
        if (req->path().rfind("/api/", 0) != 0) return;
        RequestTrace trace{++nextRequest, std::chrono::steady_clock::now()};
        req->attributes()->insert("dice.diagnostic", trace);
        if (logManagementRequest(req->path(), req->methodString()))
            DICE_LOG_INFO("[http] request={} {} {} phase=begin", trace.id, req->methodString(), req->path());
    });
    app.setExceptionHandler([](const std::exception& error, const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        const auto& trace = req->attributes()->get<RequestTrace>("dice.diagnostic");
        // No request body, query string, cookies or authorization headers.
        const auto current = std::current_exception();
        DICE_LOG_ERROR("[http] request={} {} {} unhandled exception: {}", trace.id, req->methodString(), req->path(),
            current ? describeException(current) : sanitizeLogText(error.what()));
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
        response->addHeader("X-Dice-Request-Id", std::to_string(trace.id));
        try {
            const auto result = httpLogResult(static_cast<int>(response->statusCode()), response->body());
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - trace.start).count();
            const auto status = static_cast<int>(response->statusCode());
            const bool denied = status == 401 || status == 403;
            auto severity = status >= 500 ? spdlog::level::err : result.failed && !denied ? spdlog::level::warn
                : logManagementRequest(req->path(), req->methodString()) ? spdlog::level::info : spdlog::level::debug;
            // Browser polling is debug-only. Slow calls remain visible at warn.
            if (ms >= 2000 && !denied && severity < spdlog::level::warn) severity = spdlog::level::warn;
            getLogger()->log(DICE_LOG_SOURCE, severity,
                "[http] request={} {} {} phase=end status={} code={} result={} elapsed_ms={} {}",
                trace.id, req->methodString(), req->path(), status, result.code,
                result.failed ? "failed" : "completed", ms,
                req->path().rfind("/api/auth/", 0) == 0 ? std::string() : result.reason);
        } catch (...) { DICE_LOG_EXCEPTION("HTTP response diagnostics"); }
    });
}
} // namespace dice
