#pragma once
#include "service/webui_versions.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpAppFramework.h>

namespace dice::webui {
inline drogon::HttpResponsePtr entryResponse(const ConfigManager& config,
        const std::filesystem::path& root, const std::string& query = "") {
    const auto state = versions(config, root);
    const auto version = state["effective"].get<std::string>();
    drogon::HttpResponsePtr response;
    if (state["legacy"].get<bool>()) {
        response = drogon::HttpResponse::newFileResponse((root / "index.html").string());
    } else if (!version.empty()) {
        // Relative Location retains reverse-proxy prefixes; an absent fragment
        // inherits the original browser hash route according to HTTP semantics.
        auto location = "./" + version + "/";
        if (!query.empty()) location += "?" + query;
        response = drogon::HttpResponse::newRedirectionResponse(location);
    } else {
        response = drogon::HttpResponse::newNotFoundResponse();
    }
    response->addHeader("Cache-Control", "no-store");
    return response;
}

inline void registerEntryRoutes(drogon::HttpAppFramework& app,
        const ConfigManager& config, const std::filesystem::path& documentRoot) {
    const auto root = std::filesystem::absolute(documentRoot);
    for (const char* route : {"/", "/index.html"}) {
        // Drogon preserves the deduced handler type, including references.
        // Pass a fresh temporary so each route OWNS its callable after this
        // function returns. A local lvalue lambda would leave a dangling reference.
        app.registerHandler(route,
            [&config, root](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                cb(entryResponse(config, root, req->query()));
            }, {drogon::Get, drogon::Head});
    }
}
}
