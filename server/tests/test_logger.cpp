#include "test_framework.h"
#include "common/logger.h"
#include "common/log_context.h"
#include "adapter/outbound_logging.h"
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

TEST(Logger, HumanRecordsHaveFourFieldsAndDiagnosticsAreSeparate) {
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
            if (line.find(marker) != std::string::npos) warning = line.find("[WARN]") != std::string::npos;
            if (line.find("source-location-probe-" + nonce) != std::string::npos)
                source = line.find("[WARN] [系统]") != std::string::npos && line.find("thread=") == std::string::npos
                    && line.find("tz=") == std::string::npos && line.find("test_logger.cpp:") == std::string::npos;
        }
    }
    ASSERT_TRUE(warning);
    ASSERT_TRUE(source);
    bool detail = false;
    for (const auto& record : recentLogRecords(1000))
        if (record["message"].get<std::string>().find("source-location-probe-" + nonce) != std::string::npos)
            detail = record["details"]["cpp_file"] == "test_logger.cpp" && record["details"].contains("thread") && record["module"] == "系统";
    ASSERT_TRUE(detail);
}

TEST(Logger, SixLevelsUseIndependentSuccessThresholdAndLegacyAliases) {
    const auto old = std::string(logLevelName(logRecordingLevel()));
    configureLogger(nlohmann::json{{"server", {{"log_level", "success"}}}});
    ASSERT_FALSE(shouldRecordLog(LogLevel::Info));
    ASSERT_TRUE(shouldRecordLog(LogLevel::Success));
    ASSERT_TRUE(shouldRecordLog(LogLevel::Warn));
    const auto nonce = newLogCorrelationId();
    DICE_LOG_INFO_M("测试", "hidden-info-{}", nonce);
    DICE_LOG_SUCCESS_M("测试", "visible-success-{}", nonce);
    bool success = false, info = false;
    for (const auto& record : recentLogRecords(1000)) {
        const auto text = record["message"].get<std::string>();
        if (text == "visible-success-" + nonce) success = record["level"] == "success";
        if (text == "hidden-info-" + nonce) info = true;
    }
    configureLogger(nlohmann::json{{"server", {{"log_level", old}}}});
    ASSERT_TRUE(success); ASSERT_FALSE(info);
    ASSERT_TRUE(parseLogLevel("warning") == LogLevel::Warn);
    ASSERT_TRUE(parseLogLevel("critical") == LogLevel::Error);
    ASSERT_TRUE(parseLogLevel("invalid") == LogLevel::Info);
    ASSERT_FALSE(validLogLevel("critical"));
}

TEST(Logger, PreparedActualAndFailureRecordsShareIdsAndRetainOrigins) {
    const auto old = std::string(logLevelName(logRecordingLevel()));
    configureLogger(nlohmann::json{{"server", {{"log_level", "debug"}}}});
    auto plan = std::make_shared<PreparedOutput>("群 1001", "**上签**",
        ProcessingContext{{{"Lua 插件", "求签", "求签.lua", "draw"}}, "用户指令 .求签", "处理完成，生成 1 条回复"});
    {
        OutboundScope scope(plan, 1, 2);
        auto attempt = OutboundAttempt::prepare("OneBot 测试", "群 1001", "上签");
        attempt.submitted(); attempt.accepted("platform-id"); attempt.failed("test failure");
    }
    int count = 0; bool planned = false, actual = false, failed = false;
    for (const auto& record : recentLogRecords(1000)) {
        if (record.value("msg_id", std::string()) != plan->id) continue;
        ++count;
        const auto text = record["message"].get<std::string>();
        if (record["module"] == "处理") planned = text.find("**上签**") != std::string::npos && text.find("求签") != std::string::npos;
        if (record["module"] == "消息" && record["level"] == "info") actual = text.find("骰娘：上签") != std::string::npos && text.find("**") == std::string::npos;
        if (record["level"] == "error") failed = record["details"]["processing"]["sources"][0]["name"] == "求签";
    }
    configureLogger(nlohmann::json{{"server", {{"log_level", old}}}});
    ASSERT_EQ(count, 4); ASSERT_TRUE(planned); ASSERT_TRUE(actual); ASSERT_TRUE(failed);
    ASSERT_TRUE(newLogCorrelationId() != plan->id);
}

TEST(Logger, ReportedFailureSummaryDoesNotHideANewExceptionOrAnUnreportedFailure) {
    const auto marker = newLogCorrelationId();
    { LogOperation op(DICE_LOG_SOURCE,"js","load_plugin",marker + "-reported");
      op.fail(marker + "-reported",true); }
    { LogOperation op(DICE_LOG_SOURCE,"js","load_plugin",marker + "-unreported");
      op.fail(marker + "-unreported"); }
    try {
        LogOperation op(DICE_LOG_SOURCE,"js","load_plugin",marker + "-exception");
        op.fail(marker + "-exception",true);
        throw std::runtime_error("new failure after an already reported error");
    } catch (const std::exception&) {}
    int reported = 0, unreported = 0, exceptional = 0;
    for (const auto& record : recentLogRecords(1000)) {
        if (record["level"] != "error") continue;
        const auto text = record["message"].get<std::string>();
        if (text.find(marker + "-reported") != std::string::npos) ++reported;
        if (text.find(marker + "-unreported") != std::string::npos) ++unreported;
        if (text.find(marker + "-exception") != std::string::npos) ++exceptional;
    }
    ASSERT_EQ(reported,0);
    ASSERT_EQ(unreported,1);
    ASSERT_EQ(exceptional,1);
}

TEST(Logger, ScopesRestoreAcrossNestedAndAsynchronousWork) {
    ProcessingContext saved;
    {
        ProcessingScope first(ProcessingSource{"JS 插件", "DailyNews", "news.js", "dailynews"}, "定时任务");
        saved = activeProcessingContext;
        { ProcessingScope second(ProcessingSource{"Lua 插件", "求签", "签.lua", "draw"});
          ASSERT_EQ(activeProcessingContext.sources[0].name, std::string("求签")); }
        ASSERT_EQ(activeProcessingContext.sources[0].name, std::string("DailyNews"));
    }
    ASSERT_TRUE(activeProcessingContext.sources.empty());
    ProcessingScope timer(saved);
    ASSERT_EQ(activeProcessingContext.sources[0].name, std::string("DailyNews"));
    ASSERT_EQ(activeProcessingContext.trigger, std::string("定时任务"));
}

TEST(Logger, ProtocolPayloadLoggingDoesNotDumpFileBodiesOrPollAsMessages) {
    ASSERT_FALSE(prepareActionLog("test", "get_group_list", nlohmann::json::object()).has_value());
    const auto text = logPayloadText(nlohmann::json::array({
        {{"type", "text"}, {"data", {{"text", "结果"}}}},
        {{"type", "image"}, {"data", {{"file", "base64://private-file-body"}}}}
    }));
    ASSERT_TRUE(text.find("结果") != std::string::npos);
    ASSERT_TRUE(text.find("图片") != std::string::npos);
    ASSERT_TRUE(text.find("private-file-body") == std::string::npos);
    const auto raw = sanitizeLogText("结果 [CQ:image,file=base64://private-file-body] data:image/png;base64,private-inline-body");
    ASSERT_TRUE(raw.find("private-file-body") == std::string::npos);
    ASSERT_TRUE(raw.find("private-inline-body") == std::string::npos);
    ASSERT_TRUE(raw.find("结果") != std::string::npos);
    ASSERT_EQ(logPayloadText(nlohmann::json{{"type", "at"}, {"data", nullptr}}), std::string("[提及：未提供]"));
    ASSERT_EQ(logPayloadText(nlohmann::json{{"type", "text"}, {"data", {{"text", 123}}}}), std::string("123"));
    ASSERT_EQ(logPayloadText(nlohmann::json{{"type", "image"}, {"data", {{"file", 42}}}}), std::string("[图片：42]"));
    ASSERT_FALSE(prepareRestEventLog("Discord", "GET", "/api/v10/guilds/123/members/456", nullptr).has_value());
    const auto kick = prepareRestEventLog("KOOK", "POST", "/api/v3/guild/kickout", {{"guild_id", "123"}, {"target_id", "456"}});
    ASSERT_TRUE(kick && kick->module == "事件" && kick->content.find("456") != std::string::npos);
    const auto unmute = prepareRestEventLog("Discord", "PATCH", "/api/v10/guilds/123/members/456", {{"communication_disabled_until", nullptr}});
    ASSERT_TRUE(unmute && unmute->content.find("解除") != std::string::npos);
}

TEST(Logger, MalformedPluginBytesAndSecretDetailsCannotBreakDashboardJson) {
    writeLog({}, LogLevel::Error, "插件", std::string("bad-byte-") + char(0xba), {{"password", "probe-secret"}, {"detail", "token=probe-token"}});
    const auto last = recentLogRecords(1)[0];
    ASSERT_TRUE(last.dump().find("probe-secret") == std::string::npos);
    ASSERT_TRUE(last.dump().find("probe-token") == std::string::npos);
    ASSERT_EQ(last["details"]["password"].get<std::string>(), std::string("[REDACTED]"));
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
    request->attributes()->insert("dn_qq",std::string("12345"));
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
    bool actorRecorded=false;
    for(const auto& record:recentLogRecords(1000)) if(record["message"].get<std::string>().find(marker)!=std::string::npos)
        actorRecorded=record["details"]["processing"]["origin"]["user_id"]=="12345";
    ASSERT_TRUE(actorRecorded);
}
