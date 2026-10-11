#include "test_framework.h"
#include "common/logger.h"
#include "common/http_log.h"
#include "service/http_diagnostics.h"
#include <trantor/utils/Logger.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace dice;

TEST(Logger, RedactsJsonQueriesHeadersAndUrlCredentials) {
    const std::string input = R"({"password":"secret-pass","access_token":"secret-token","count":12})"
        "\nAUTHORIZATION: Bearer secret-auth\nCookie: session=secret-cookie; login=second-cookie\nhttps://alice:secret-smtp@example.org/path?api_key=secret-key&count=12";
    const auto safe = sanitizeLogText(input);
    for (const auto* secret : {"secret-pass", "secret-token", "secret-auth", "secret-smtp", "secret-key", "secret-cookie", "second-cookie"})
        ASSERT_TRUE(safe.find(secret) == std::string::npos);
    ASSERT_TRUE(safe.find("[REDACTED]") != std::string::npos);
    ASSERT_TRUE(safe.find("count=12") != std::string::npos);
    ASSERT_TRUE(safe.find("example.org/path") != std::string::npos);
}

TEST(Logger, ConfiguredSecretsAreHiddenEvenWithoutAFieldName) {
    const auto previous = getLogger()->level();
    configureLogger(nlohmann::json{
        {"server", {{"log_level", "debug"}, {"api_key", "server-key-probe"}}},
        {"adapters", nlohmann::json::array({{{"token", "adapter-token-probe"}}})},
        {"identity_email", {{"pass", "smtp-pass-probe"}}}
    });
    const auto safe = sanitizeLogText("server-key-probe adapter-token-probe smtp-pass-probe");
    setLogSecrets({});
    getLogger()->set_level(previous);
    ASSERT_TRUE(safe.find("-probe") == std::string::npos);
    ASSERT_EQ(safe, std::string("[REDACTED] [REDACTED] [REDACTED]"));
    setLogSecrets({"E", "ED", "long-secret"});
    const auto shortSafe = sanitizeLogText("long-secret ED E");
    const auto shortSafeAgain = sanitizeLogText(shortSafe);
    setLogSecrets({});
    ASSERT_EQ(shortSafe, std::string("[REDACTED] [REDACTED] [REDACTED]"));
    ASSERT_EQ(shortSafeAgain, shortSafe);
    configureLogger(nlohmann::json{{"server", {{"log_level", 123}}}});
    const auto invalidLevel = getLogger()->level();
    getLogger()->set_level(previous);
    ASSERT_EQ(invalidLevel, spdlog::level::info);
}

TEST(Logger, MultilineRecordsCannotForgeHeadersAndControlsAreEscaped) {
    const auto safe = sanitizeLogText("cause\n[2026-10-11 12:00:00.000] [info] fake\r\x1b[31m");
    ASSERT_TRUE(safe.find("\n    | [2026") != std::string::npos);
    ASSERT_TRUE(safe.find("\\r") != std::string::npos);
    ASSERT_TRUE(safe.find("\\x1b") != std::string::npos);
    ASSERT_EQ(sanitizeLogText(safe), safe);
}

TEST(Logger, TruncationPreservesUtf8AndParseErrorsDoNotCopyInputFragments) {
    const auto safe = sanitizeLogText(std::string(16383, 'x') + "错误");
    ASSERT_TRUE(safe.find("[truncated]") != std::string::npos);
    ASSERT_TRUE(safe.find("错误") == std::string::npos);
    const auto error = sanitizeLogText("[json.exception.parse_error.101] invalid literal; last read: 'private-message'");
    ASSERT_TRUE(error.find("private-message") == std::string::npos);
    ASSERT_TRUE(error.find("invalid literal") != std::string::npos);
}

TEST(Logger, NestedAndNonStandardExceptionsRemainVisible) {
    std::exception_ptr nested;
    try {
        try { throw std::runtime_error("database unavailable"); }
        catch (...) { std::throw_with_nested(std::runtime_error("loading configuration failed")); }
    } catch (...) { nested = std::current_exception(); }
    const auto detail = describeException(nested);
    ASSERT_TRUE(detail.find("database unavailable") != std::string::npos);
    ASSERT_TRUE(detail.find("loading configuration failed") != std::string::npos);
    ASSERT_TRUE(detail.find("caused by:") != std::string::npos);
    try { throw 7; } catch (...) {
        ASSERT_EQ(describeException(std::current_exception()), std::string("non-standard C++ exception"));
    }
}

TEST(Logger, HttpDiagnosticsDetectBusinessFailuresWithoutLoggingSuccessfulPayloads) {
    auto failed = httpLogResult(200, R"({"code":1,"message":"cannot read deck"})");
    ASSERT_TRUE(failed.failed);
    ASSERT_EQ(failed.reason, std::string("cannot read deck"));
    failed = httpLogResult(200, R"({"error":"plugin failure"})");
    ASSERT_TRUE(failed.failed);
    ASSERT_EQ(failed.reason, std::string("plugin failure"));
    const auto success = httpLogResult(200, R"({"code":0,"message":"secret response","data":{"password":"private"}})");
    ASSERT_FALSE(success.failed);
    ASSERT_TRUE(success.reason.empty());
    ASSERT_TRUE(httpLogResult(500, "not json").failed);
    ASSERT_FALSE(httpLogResult(200, std::string(9000, 'x')).failed);
}

TEST(Logger, PollingAndCommandPreviewsDoNotGenerateManagementRecords) {
    ASSERT_FALSE(logManagementRequest("/api/dashboard", "GET"));
    ASSERT_FALSE(logManagementRequest("/api/test/message", "POST"));
    ASSERT_FALSE(logManagementRequest("/api/auth/login", "POST"));
    ASSERT_TRUE(logManagementRequest("/api/mod/lua/delete", "POST"));
    ASSERT_TRUE(logManagementRequest("/api/replies", "DELETE"));
}

TEST(Logger, FrameworkWarningsAndSourceLocationsReachTheFile) {
    getLogger();
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto marker = "framework-warning-probe-" + nonce;
    LOG_WARN << marker;
    DICE_LOG_WARN("source-location-probe-{}", nonce);
    getLogger()->flush();
    bool warning = false, source = false;
    for (const auto& entry : std::filesystem::directory_iterator("data/logs/app")) {
        if (entry.path().extension() != ".log") continue;
        std::ifstream file(entry.path());
        std::string line;
        while (std::getline(file, line)) {
            if (line.find(marker) != std::string::npos) warning = line.find("[warning]") != std::string::npos;
            if (line.find("source-location-probe-" + nonce) != std::string::npos)
                source = line.find("test_logger.cpp:") != std::string::npos && line.find("thread=") != std::string::npos
                    && line.find("tz=") != std::string::npos;
        }
    }
    ASSERT_TRUE(warning);
    ASSERT_TRUE(source);
}

TEST(Logger, HttpExceptionBoundaryKeepsNestedCausesPrivateFromClients) {
    auto& app = drogon::app();
    installHttpDiagnostics(app);
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto marker = "http-exception-probe-" + nonce;
    const auto privateBody = "private-body-probe-" + nonce;
    const auto privateHeader = "private-header-probe-" + nonce;
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setPath("/api/diagnostic-probe");
    request->setBody(privateBody);
    request->addHeader("Authorization", privateHeader);
    drogon::HttpResponsePtr response;
    try {
        try { throw std::runtime_error("inner HTTP failure"); }
        catch (...) { std::throw_with_nested(std::runtime_error(marker)); }
    } catch (const std::exception& error) {
        app.getExceptionHandler()(error, request, [&](const auto& result) { response = result; });
    }
    ASSERT_TRUE(response != nullptr);
    ASSERT_EQ(response->statusCode(), drogon::k500InternalServerError);
    ASSERT_EQ(nlohmann::json::parse(response->body())["message"].get<std::string>(), std::string("internal_error"));
    getLogger()->flush();
    bool recorded = false;
    for (const auto& entry : std::filesystem::directory_iterator("data/logs/app")) {
        if (entry.path().extension() != ".log") continue;
        std::ifstream file(entry.path());
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.find(marker) == std::string::npos) continue;
        recorded = content.find("inner HTTP failure") != std::string::npos;
        ASSERT_TRUE(content.find(privateBody) == std::string::npos);
        ASSERT_TRUE(content.find(privateHeader) == std::string::npos);
    }
    ASSERT_TRUE(recorded);
}
