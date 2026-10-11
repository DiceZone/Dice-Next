#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dice {
struct HttpLogResult {
    bool failed = false;
    int code = 0;
    std::string reason;
};

inline bool logManagementRequest(std::string_view path, std::string_view method) {
    return path.rfind("/api/", 0) == 0 && method != "GET" && method != "HEAD" && method != "OPTIONS"
        && path.rfind("/api/auth/", 0) != 0 && path.rfind("/api/test/", 0) != 0
        && path != "/api/replies/test";
}

inline HttpLogResult httpLogResult(int status, std::string_view body) {
    HttpLogResult result;
    result.failed = status >= 400;
    // Never parse big exports/files or log successful response data.
    if (body.empty() || body.size() > 8192 || body.front() != '{') return result;
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object()) return result;
    if (json.contains("code") && json["code"].is_number_integer()) {
        result.code = json["code"].get<int>();
        result.failed = result.failed || result.code != 0;
    }
    if (json.contains("error") && json["error"].is_string() && !json["error"].get_ref<const std::string&>().empty()) {
        result.failed = true;
        result.reason = json["error"].get<std::string>();
    }
    if (result.failed && result.reason.empty() && json.contains("message") && json["message"].is_string())
        result.reason = json["message"].get<std::string>();
    return result;
}
} // namespace dice
