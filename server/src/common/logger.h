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
    void fail(std::string reason) { failed_ = true; detail_ = std::move(reason); }
private:
    spdlog::source_loc source_;
    std::string component_, operation_, detail_;
    uint64_t id_;
    int exceptions_;
    bool failed_ = false;
    std::chrono::steady_clock::time_point start_;
};

// ─── Convenience Macros ──────────────────────────────────────

#define DICE_LOG_SOURCE spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}
#define DICE_LOG_TRACE(...)    dice::getLogger()->log(DICE_LOG_SOURCE, spdlog::level::trace, __VA_ARGS__)
#define DICE_LOG_DEBUG(...)    dice::getLogger()->log(DICE_LOG_SOURCE, spdlog::level::debug, __VA_ARGS__)
#define DICE_LOG_INFO(...)     dice::getLogger()->log(DICE_LOG_SOURCE, spdlog::level::info, __VA_ARGS__)
#define DICE_LOG_WARN(...)     dice::getLogger()->log(DICE_LOG_SOURCE, spdlog::level::warn, __VA_ARGS__)
#define DICE_LOG_ERROR(...)    dice::getLogger()->log(DICE_LOG_SOURCE, spdlog::level::err, __VA_ARGS__)
#define DICE_LOG_CRITICAL(...) dice::getLogger()->log(DICE_LOG_SOURCE, spdlog::level::critical, __VA_ARGS__)
#define DICE_LOG_EXCEPTION(context) dice::logCurrentException(DICE_LOG_SOURCE, context)
#define DICE_LOG_WARN_EXCEPTION(context) dice::logCurrentException(DICE_LOG_SOURCE, context, spdlog::level::warn)

}  // namespace dice
