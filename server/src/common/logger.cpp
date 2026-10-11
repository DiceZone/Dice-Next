#include "logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <vector>
#include <memory>
#include <ctime>
#include <string>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <trantor/utils/Logger.h>
#include <spdlog/details/os.h>

namespace dice {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
std::mutex initMutex;
std::shared_ptr<const std::vector<std::string>> logSecrets = std::make_shared<const std::vector<std::string>>();
std::atomic<uint64_t> nextOperation{0};

bool secretKey(std::string_view key) {
    std::string normalized;
    for (unsigned char c : key) if (std::isalnum(c)) normalized += static_cast<char>(std::tolower(c));
    return normalized == "password" || normalized == "passwd" || normalized == "pass"
        || normalized == "token" || normalized == "secret" || normalized == "apikey"
        || normalized == "accesskey" || normalized == "accesssecret" || normalized == "appsecret"
        || normalized == "clientsecret" || normalized == "accesstoken" || normalized == "refreshtoken"
        || normalized == "heartapikey" || normalized == "pluginverifykey"
        || normalized == "authorization" || normalized == "cookie";
}

// Redact quoted JSON values, key=value query/config values and auth headers.
std::string redactKeys(std::string_view text) {
    std::string out;
    size_t copied = 0;
    for (size_t i = 0; i < text.size();) {
        const auto word = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; };
        if (!word(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        const size_t begin = i;
        while (i < text.size() && word(static_cast<unsigned char>(text[i]))) ++i;
        const auto key = text.substr(begin, i - begin);
        if (!secretKey(key)) continue;
        size_t value = i;
        if (value < text.size() && (text[value] == '"' || text[value] == '\'')) ++value;
        while (value < text.size() && (text[value] == ' ' || text[value] == '\t')) ++value;
        if (value == text.size() || (text[value] != ':' && text[value] != '=')) continue;
        ++value;
        while (value < text.size() && (text[value] == ' ' || text[value] == '\t')) ++value;
        const char quote = value < text.size() && (text[value] == '"' || text[value] == '\'') ? text[value++] : 0;
        size_t end = value;
        if (quote) {
            while (end < text.size() && text[end] != quote) {
                if (text[end] == '\\' && end + 1 < text.size()) ++end;
                ++end;
            }
        } else {
            std::string lower;
            for (unsigned char c : key) lower += static_cast<char>(std::tolower(c));
            const bool header = lower == "authorization" || lower == "cookie";
            while (end < text.size() && text[end] != '\r' && text[end] != '\n'
                && (header || (text[end] != ',' && text[end] != ';' && text[end] != '&'
                    && text[end] != '}' && text[end] != ' ' && text[end] != '\t'))) ++end;
        }
        if (end == value) continue;
        out.append(text.substr(copied, value - copied));
        out += "[REDACTED]";
        copied = end;
        i = end;
    }
    out.append(text.substr(copied));
    return out;
}

class SafeLogger final : public spdlog::logger {
public:
    using spdlog::logger::logger;
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const auto safe = sanitizeLogText({msg.payload.data(), msg.payload.size()});
        auto cleaned = msg;
        cleaned.payload = spdlog::string_view_t(safe.data(), safe.size());
        spdlog::logger::sink_it_(cleaned);
    }
};

void connectFrameworkLogger(const std::shared_ptr<spdlog::logger>& logger) {
    if (trantor::Logger::hasSpdLogSupport()) {
        trantor::Logger::enableSpdLog(logger);
        return;
    }
    trantor::Logger::setOutputFunction([logger](const char* data, uint64_t size) {
        std::string_view line(data, static_cast<size_t>(size));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        auto level = spdlog::level::info;
        const std::pair<std::string_view, spdlog::level::level_enum> levels[] = {
            {" TRACE ", spdlog::level::trace}, {" DEBUG ", spdlog::level::debug}, {" INFO  ", spdlog::level::info},
            {" WARN  ", spdlog::level::warn}, {" ERROR ", spdlog::level::err}, {" FATAL ", spdlog::level::critical}};
        for (const auto& [tag, severity] : levels) {
            auto at = line.substr(0, 64).find(tag);
            if (at != std::string_view::npos) { level = severity; line.remove_prefix(at + tag.size()); break; }
        }
        logger->log(level, "[http-framework] {}", line);
    }, [logger] { logger->flush(); });
}
} // namespace

void setLogSecrets(std::vector<std::string> secrets) {
    secrets.erase(std::remove_if(secrets.begin(), secrets.end(), [](const auto& s) { return s.empty(); }), secrets.end());
    std::sort(secrets.begin(), secrets.end(), [](const auto& a, const auto& b) {
        return a.size() == b.size() ? a < b : a.size() > b.size();
    });
    secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
    std::atomic_store(&logSecrets, std::make_shared<const std::vector<std::string>>(std::move(secrets)));
}

std::string sanitizeLogText(std::string_view text) {
    // JSON parse exceptions can include an arbitrary fragment of request/config data.
    if (text.find("json.exception.parse_error") != std::string_view::npos) {
        if (auto at = text.find("last read:"); at != std::string_view::npos) text = text.substr(0, at);
    }
    std::string safe;
    const auto secrets = std::atomic_load(&logSecrets);
    for (size_t at = 0; at < text.size();) {
        // Never reprocess our marker, including when a legacy password is short.
        if (text.substr(at, 10) == "[REDACTED]") { safe += "[REDACTED]"; at += 10; continue; }
        size_t matched = 0;
        for (const auto& secret : *secrets) {
            if (text.substr(at, secret.size()) == secret) { matched = secret.size(); break; }
        }
        if (matched) { safe += "[REDACTED]"; at += matched; }
        else safe += text[at++];
    }
    safe = redactKeys(safe);
    // URL userinfo must never expose SMTP/proxy credentials.
    for (size_t at = 0; (at = safe.find("://", at)) != std::string::npos;) {
        const size_t begin = at + 3;
        auto end = safe.find_first_of("/ \t\r\n?#", begin);
        auto auth = safe.find('@', begin);
        if (auth != std::string::npos && (end == std::string::npos || auth < end)) {
            safe.replace(begin, auth - begin, "[REDACTED]");
        }
        at = begin;
    }
    constexpr size_t limit = 16384;
    const bool truncated = safe.size() > limit;
    if (truncated) {
        size_t end = limit;
        while (end > 0 && (static_cast<unsigned char>(safe[end]) & 0xc0) == 0x80) --end;
        safe.resize(end);
    }
    std::string out;
    for (size_t i = 0; i < safe.size(); ++i) {
        const auto c = static_cast<unsigned char>(safe[i]);
        if (c == '\n') out += safe.compare(i + 1, 6, "    | ") == 0 ? "\n" : "\n    | ";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32 || c == 127) { const char hex[] = "0123456789abcdef"; out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    if (truncated) out += " ... [truncated]";
    return out;
}

void configureLogger(const nlohmann::json& config) {
    std::vector<std::string> secrets;
    const auto visit = [&](const auto& self, const nlohmann::json& value) -> void {
        if (value.is_object()) for (const auto& [key, child] : value.items()) {
            if (secretKey(key) && child.is_string()) secrets.push_back(child.get<std::string>());
            else if (child.is_structured()) self(self, child);
        }
        else if (value.is_array()) for (const auto& child : value) self(self, child);
    };
    visit(visit, config);
    setLogSecrets(std::move(secrets));
    auto level = std::string("info");
    if (config.contains("server") && config["server"].is_object()
        && config["server"].contains("log_level") && config["server"]["log_level"].is_string())
        level = config["server"]["log_level"].get<std::string>();
    auto parsed = spdlog::level::from_str(level);
    if (parsed == spdlog::level::off && level != "off") parsed = spdlog::level::info;
    getLogger()->set_level(parsed);
}

std::string describeException(std::exception_ptr error) {
    if (!error) return "unknown exception";
    std::string detail;
    for (int depth = 0; error && depth < 8; ++depth) {
        try { std::rethrow_exception(error); }
        catch (const std::exception& e) {
            if (!detail.empty()) detail += "\ncaused by: ";
            detail += e.what();
            const auto* nested = dynamic_cast<const std::nested_exception*>(&e);
            error = nested ? nested->nested_ptr() : std::exception_ptr{};
        }
        catch (...) {
            if (!detail.empty()) detail += "\ncaused by: ";
            detail += "non-standard C++ exception"; error = {};
        }
    }
    return sanitizeLogText(detail);
}

void logCurrentException(spdlog::source_loc source, std::string_view context, spdlog::level::level_enum level) noexcept {
    try { getLogger()->log(source, level, "[exception] {}: {}", context, describeException(std::current_exception())); }
    catch (...) { std::fputs("Dice!Next: failed to record an exception\n", stderr); }
}

LogOperation::LogOperation(spdlog::source_loc source, std::string_view component, std::string_view operation,
                           std::string_view detail)
    : source_(source), component_(component), operation_(operation), detail_(detail), id_(++nextOperation),
      exceptions_(std::uncaught_exceptions()), start_(std::chrono::steady_clock::now()) {
    getLogger()->log(source_, spdlog::level::info, "[{}] op={} action={} phase=begin {}", component_, id_, operation_, detail_);
}
void LogOperation::phase(std::string_view name) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    getLogger()->log(source_, spdlog::level::info, "[{}] op={} action={} phase={} elapsed_ms={}", component_, id_, operation_, name, ms);
}
LogOperation::~LogOperation() noexcept {
    try {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
        const bool failed = failed_ || std::uncaught_exceptions() > exceptions_;
        getLogger()->log(source_, failed ? spdlog::level::err : spdlog::level::info,
            "[{}] op={} action={} phase=end result={} elapsed_ms={} {}", component_, id_, operation_,
            failed ? "failed" : "completed", ms, detail_);
    } catch (...) {}
}

void initLogger(const std::string& level) {
    std::lock_guard lock(initMutex);
    if (std::atomic_load(&g_logger)) return;

    // Create console sink with color
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread=%t] [%s:%#] %v");

    // 日志文件：每次启动新建一份（文件名带启动时间戳），并按天分割（跨午夜自动新文件）。
    // 存 data/logs/app/ 下，与跑团 txt 日志（data/logs/*.txt）分开。
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32]; std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);
    std::string base = std::string("data/logs/app/dice-") + ts + "-" + std::to_string(spdlog::details::os::pid()) + ".log";
    // daily_file_sink：文件名 basename_YYYY-MM-DD.log，每天 00:00 切；启动时间戳保证每次启动新系列。
    std::vector<spdlog::sink_ptr> sinks{console_sink};
    std::string fileError;
    try {
        auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(base, 0, 0);
        // Keep the first three fields compatible with existing WebUI log readers.
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [tz=%z thread=%t] [%s:%#] %v");
        sinks.push_back(file_sink);
    } catch (const std::exception& e) { fileError = e.what(); }
    std::shared_ptr<spdlog::logger> logger = std::make_shared<SafeLogger>("dice", sinks.begin(), sinks.end());

    // Set log level
    auto lvl = spdlog::level::from_str(level);
    if (lvl == spdlog::level::off && level != "off") lvl = spdlog::level::info;
    logger->set_level(lvl);
    logger->flush_on(spdlog::level::info);
    logger->set_error_handler([](const std::string& error) {
        const auto safe = sanitizeLogText(error);
        std::fprintf(stderr, "Dice!Next logging failure: %s\n", safe.c_str());
    });

    // Register globally so drogon can also pick it up
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
    std::atomic_store(&g_logger, logger);
    connectFrameworkLogger(logger);
    if (!fileError.empty()) logger->error("[logging] file output unavailable; continuing with console only: {}", fileError);
}

std::shared_ptr<spdlog::logger> getLogger() {
    if (!std::atomic_load(&g_logger)) {
        // Fallback: initialize with defaults
        initLogger("info");
    }
    return std::atomic_load(&g_logger);
}

}  // namespace dice
