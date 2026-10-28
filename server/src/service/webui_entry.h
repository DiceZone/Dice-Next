#pragma once
#include "service/webui_versions.h"
#include <drogon/HttpResponse.h>

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
}
