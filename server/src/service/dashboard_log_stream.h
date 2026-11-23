#pragma once
#include "../common/logger.h"
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <atomic>

namespace dice {
// All socket work runs on a network event loop, never on the log writer.
inline drogon::HttpResponsePtr dashboardLogStream(std::string after) {
    static std::atomic<unsigned> active{0};
    struct Lease { ~Lease() { --active; } };
    auto lease = std::make_shared<Lease>();
    if (active.fetch_add(1) >= 32) {
        auto busy = drogon::HttpResponse::newHttpResponse();
        busy->setStatusCode(drogon::k429TooManyRequests);
        return busy; // caller can still use the ordinary incremental endpoint
    }
    auto response = drogon::HttpResponse::newAsyncStreamResponse(
        [after = std::move(after), lease](drogon::ResponseStreamPtr raw) {
            std::shared_ptr<drogon::ResponseStream> stream(std::move(raw));
            if (!stream || !stream->send(": ready\n\n")) return;
            auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
            if (!loop) return;
            auto timer = std::make_shared<trantor::TimerId>();
            *timer = loop->runEvery(0.5, [loop, timer, stream, lease, cursor = after, ticks = 0]() mutable {
                // Periodically close and reauthenticate, including on idle streams.
                if (++ticks <= 240) {
                    auto batch = dashboardLogBatch(cursor);
                    auto& rows = batch["entries"];
                    if (!rows.empty()) {
                        cursor = rows.back().at("id").get<std::string>();
                        if (stream->send("data: " + batch.dump() + "\n\n")) return;
                    } else if (ticks % 20 != 0 || stream->send(": heartbeat\n\n")) return;
                } else stream->send("event: rotate\ndata: {}\n\n");
                loop->invalidateTimer(*timer);
                stream->close();
            });
        });
    response->setContentTypeString("text/event-stream");
    response->addHeader("Cache-Control", "no-store, no-transform");
    response->addHeader("X-Accel-Buffering", "no");
    return response;
}
inline drogon::HttpResponsePtr dashboardLogsResponse(const drogon::HttpRequestPtr& req) {
    const auto after = req->getParameter("after");
    if (req->getHeader("accept") == "text/event-stream") return dashboardLogStream(after);
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->addHeader("Cache-Control", "no-store");
    resp->setBody(nlohmann::json{{"code", 0}, {"message", "ok"}, {"data", dashboardLogBatch(after)}}.dump());
    return resp;
}
} // namespace dice
