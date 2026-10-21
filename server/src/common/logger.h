#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <exception>
#include <nlohmann/json_fwd.hpp>

namespace dice {

// ─── Logger Initialization ───────────────────────────────────

void initLogger(const std::string& level = "info");

// ─── Shared Logger Instance ──────────────────────────────────

std::shared_ptr<spdlog::logger> getLogger();

// Applied before every sink (including error notifications). Do not log request
// bodies/headers in the first place; redaction is defense in depth, not consent.
std::string sanitizeLogText(std::string_view text);
void setLogSecrets(std::vector<std::string> secrets);
void configureLogger(const nlohmann::json& config);
// SUCCESS is deliberately independent from INFO. The spdlog transport is an
// implementation detail; UI/file filtering uses this six-level ordering.
enum class LogLevel { Trace, Debug, Info, Success, Warn, Error, Off };
LogLevel parseLogLevel(std::string_view text);
std::string_view logLevelName(LogLevel level);
bool validLogLevel(std::string_view text);
LogLevel logRecordingLevel();
bool shouldRecordLog(LogLevel level);
std::string_view defaultLogModule(spdlog::source_loc source);
void writeLog(spdlog::source_loc source, LogLevel level, std::string_view module, std::string_view text);
void writeLog(spdlog::source_loc source, LogLevel level, std::string_view module,
              std::string_view text, const nlohmann::json& details);
// Bounded in-memory records for the current process; no daily-file rescan on
// dashboard polling. Structured diagnostics are also saved beside the text log.
nlohmann::json recentLogRecords(size_t limit = 100);
// A sanitized immutable snapshot of the record currently being delivered to
// sinks on this thread. Empty outside sink delivery; never searches global logs.
nlohmann::json currentLogRecord();
std::string newLogCorrelationId();

template<class... Args>
void logFormatted(spdlog::source_loc source, LogLevel level, std::string_view module,
                  spdlog::format_string_t<Args...> format, Args&&... args) {
    if (shouldRecordLog(level))
        writeLog(source, level, module, fmt::format(format, std::forward<Args>(args)...));
}
std::string describeException(std::exception_ptr error);
void logCurrentException(spdlog::source_loc source, std::string_view context,
                         spdlog::level::level_enum level = spdlog::level::err) noexcept;

// Begin/phase/end records make a stalled operation distinguishable from a crash.
class LogOperation {
public:
    LogOperation(spdlog::source_loc source, std::string_view component, std::string_view operation,
                 std::string_view detail = {});
    ~LogOperation() noexcept;
    LogOperation(const LogOperation&) = delete;
    LogOperation& operator=(const LogOperation&) = delete;
    void phase(std::string_view name);
    void detail(std::string text) { detail_ = std::move(text); }
    void fail(std::string reason, bool errorAlreadyReported = false) {
        failed_ = true;
        detail_ = std::move(reason);
        errorReported_ = errorAlreadyReported;
    }
    void partial(std::string reason) { partial_ = true; detail_ = std::move(reason); }
private:
    spdlog::source_loc source_;
    std::string component_, operation_, detail_;
    uint64_t id_;
    int exceptions_;
    bool failed_ = false;
    bool errorReported_ = false;
    bool partial_ = false;
    std::chrono::steady_clock::time_point start_;
};

// ─── Convenience Macros ──────────────────────────────────────

#define DICE_LOG_SOURCE spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}
#define DICE_LOG_TRACE_M(module, ...)   dice::logFormatted(DICE_LOG_SOURCE, dice::LogLevel::Trace, module, __VA_ARGS__)
#define DICE_LOG_DEBUG_M(module, ...)   dice::logFormatted(DICE_LOG_SOURCE, dice::LogLevel::Debug, module, __VA_ARGS__)
#define DICE_LOG_INFO_M(module, ...)    dice::logFormatted(DICE_LOG_SOURCE, dice::LogLevel::Info, module, __VA_ARGS__)
#define DICE_LOG_SUCCESS_M(module, ...) dice::logFormatted(DICE_LOG_SOURCE, dice::LogLevel::Success, module, __VA_ARGS__)
#define DICE_LOG_WARN_M(module, ...)    dice::logFormatted(DICE_LOG_SOURCE, dice::LogLevel::Warn, module, __VA_ARGS__)
#define DICE_LOG_ERROR_M(module, ...)   dice::logFormatted(DICE_LOG_SOURCE, dice::LogLevel::Error, module, __VA_ARGS__)
#define DICE_LOG_TRACE(...)    DICE_LOG_TRACE_M(dice::defaultLogModule(DICE_LOG_SOURCE), __VA_ARGS__)
#define DICE_LOG_DEBUG(...)    DICE_LOG_DEBUG_M(dice::defaultLogModule(DICE_LOG_SOURCE), __VA_ARGS__)
#define DICE_LOG_INFO(...)     DICE_LOG_INFO_M(dice::defaultLogModule(DICE_LOG_SOURCE), __VA_ARGS__)
#define DICE_LOG_SUCCESS(...)  DICE_LOG_SUCCESS_M(dice::defaultLogModule(DICE_LOG_SOURCE), __VA_ARGS__)
#define DICE_LOG_WARN(...)     DICE_LOG_WARN_M(dice::defaultLogModule(DICE_LOG_SOURCE), __VA_ARGS__)
#define DICE_LOG_ERROR(...)    DICE_LOG_ERROR_M(dice::defaultLogModule(DICE_LOG_SOURCE), __VA_ARGS__)
// Source compatibility for fatal boundaries, without exposing a seventh level.
#define DICE_LOG_CRITICAL(...) DICE_LOG_ERROR(__VA_ARGS__)
#define DICE_LOG_EXCEPTION(context) dice::logCurrentException(DICE_LOG_SOURCE, context)
#define DICE_LOG_WARN_EXCEPTION(context) dice::logCurrentException(DICE_LOG_SOURCE, context, spdlog::level::warn)

}  // namespace dice
