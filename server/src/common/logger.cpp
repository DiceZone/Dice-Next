#include "logger.h"
#include "log_context.h"
#include "utils.h"

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
#include <spdlog/sinks/base_sink.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <random>

namespace dice {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
std::mutex initMutex;
std::shared_ptr<const std::vector<std::string>> logSecrets = std::make_shared<const std::vector<std::string>>();
std::atomic<uint64_t> nextOperation{0};
std::atomic<LogLevel> recordingLevel{LogLevel::Info};
std::mutex recordsMutex;
std::deque<nlohmann::json> records;
struct Emission { LogLevel level; std::string_view module; const nlohmann::json* details; };
thread_local const Emission* currentEmission = nullptr;
thread_local const nlohmann::json* currentRecord = nullptr;
bool secretKey(std::string_view key);
class EmissionScope {
public:
    explicit EmissionScope(const Emission& emission) : previous_(currentEmission) { currentEmission = &emission; }
    ~EmissionScope() { currentEmission = previous_; }
private:
    const Emission* previous_;
};
LogLevel transportLevel(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace: return LogLevel::Trace;
        case spdlog::level::debug: return LogLevel::Debug;
        case spdlog::level::info: return LogLevel::Info;
        case spdlog::level::warn: return LogLevel::Warn;
        case spdlog::level::off: return LogLevel::Off;
        default: return LogLevel::Error;
    }
}
spdlog::level::level_enum toTransport(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info: case LogLevel::Success: return spdlog::level::info;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Off: return spdlog::level::off;
        default: return spdlog::level::err;
    }
}
std::string recordTime(spdlog::log_clock::time_point time) {
    const auto seconds = spdlog::log_clock::to_time_t(time);
    const auto tm = utils::timezoneTm(seconds);
    char date[32]; std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % 1000;
    return fmt::format("{}.{:03}", date, millis);
}
std::string baseFile(std::string_view file) {
    const auto slash = file.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? file : file.substr(slash + 1));
}
nlohmann::json safeDetails(const nlohmann::json& value, int depth = 0) {
    if (depth > 12) return "[详情层级过深]";
    if (value.is_string()) return sanitizeLogText(value.get_ref<const std::string&>());
    if (value.is_object()) {
        auto out = nlohmann::json::object();
        for (const auto& [key, child] : value.items())
            out[sanitizeLogText(key)] = secretKey(key) ? nlohmann::json("[REDACTED]") : safeDetails(child, depth + 1);
        return out;
    }
    if (value.is_array()) {
        auto out = nlohmann::json::array();
        for (const auto& child : value) { if (out.size() >= 128) break; out.push_back(safeDetails(child, depth + 1)); }
        return out;
    }
    return value;
}

bool secretKey(std::string_view key) {
    std::string normalized;
    for (unsigned char c : key) if (std::isalnum(c)) normalized += static_cast<char>(std::tolower(c));
    return normalized == "password" || normalized == "passwd" || normalized == "pass"
        || normalized == "token" || normalized == "secret" || normalized == "apikey"
        || normalized == "accesskey" || normalized == "accesssecret" || normalized == "appsecret"
        || normalized == "clientsecret" || normalized == "accesstoken" || normalized == "refreshtoken"
        || normalized == "heartapikey" || normalized == "pluginverifykey"
        || normalized == "authorization" || normalized == "cookie"
        || normalized == "signature" || normalized == "sig" || normalized == "xamzsignature"
        || normalized == "xamzcredential" || normalized == "xamzsecuritytoken";
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

class HumanLogFormatter final : public spdlog::formatter {
public:
    explicit HumanLogFormatter(bool color = false) : color_(color) {}
    std::unique_ptr<spdlog::formatter> clone() const override { return std::make_unique<HumanLogFormatter>(color_); }
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        const auto level = currentEmission ? currentEmission->level : transportLevel(msg.level);
        const auto module = currentEmission ? currentEmission->module : defaultLogModule(msg.source);
        fmt::format_to(std::back_inserter(dest), "[{}] [", recordTime(msg.time));
        if (color_) msg.color_range_start = dest.size();
        fmt::format_to(std::back_inserter(dest), "{}", logLevelName(level));
        if (color_) msg.color_range_end = dest.size();
        fmt::format_to(std::back_inserter(dest), "] [{}] {}\n", module,
            std::string_view(msg.payload.data(), msg.payload.size()));
    }
private:
    bool color_;
};

// Persist the same typed record as the UI sees. Technical diagnostics never
// need to be stuffed into every human-readable text line.
class StructuredLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit StructuredLogSink(std::string stem) : stem_(std::move(stem)) { open(recordTime(spdlog::log_clock::now()).substr(0, 10)); }
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (!currentRecord) return;
        open(recordTime(msg.time).substr(0, 10));
        file_ << currentRecord->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        if (!file_) throw spdlog::spdlog_ex("无法写入结构化诊断日志");
    }
    void flush_() override { file_.flush(); }
private:
    void open(const std::string& date) {
        if (date == date_) return;
        file_.close(); file_.clear();
        file_.open(stem_ + "_" + date + ".jsonl", std::ios::app | std::ios::binary);
        if (!file_) throw spdlog::spdlog_ex("无法创建结构化诊断日志文件");
        date_ = date;
    }
    std::string stem_, date_;
    std::ofstream file_;
};

class SafeLogger final : public spdlog::logger {
public:
    using spdlog::logger::logger;
    void emit(spdlog::source_loc source, LogLevel level, std::string_view module,
              std::string_view text, const nlohmann::json& details) {
        if (!shouldRecordLog(level)) return;
        const Emission emission{level, module, &details};
        EmissionScope scope(emission);
        spdlog::details::log_msg msg(source, spdlog::string_view_t(module.data(), module.size()),
            toTransport(level), spdlog::string_view_t(text.data(), text.size()));
        log_it_(msg, true, false);
    }
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const auto level = currentEmission ? currentEmission->level : transportLevel(msg.level);
        if (!shouldRecordLog(level)) return;
        auto safe = sanitizeLogText({msg.payload.data(), msg.payload.size()});
        const auto module = currentEmission ? currentEmission->module : defaultLogModule(msg.source);
        if ((module == "Lua 插件" || module == "JS 插件") && !activeProcessingContext.sources.empty())
            safe = sanitizeLogText(activeProcessingContext.label()) + "：" + safe;
        auto details = currentEmission && currentEmission->details ? *currentEmission->details : nlohmann::json::object();
        if (!details.is_object()) details = {{"detail", details}};
        if (!activeProcessingContext.sources.empty() && !details.contains("processing"))
            details["processing"] = activeProcessingContext.json();
        if (activeOutboundContext.output && !details.contains("msg_id")) {
            details["msg_id"] = activeOutboundContext.output->id;
            details["processing"] = activeOutboundContext.output->processing.json();
        }
        if (msg.source.filename && *msg.source.filename) {
            details["cpp_file"] = baseFile(msg.source.filename);
            details["cpp_line"] = msg.source.line;
            details["cpp_function"] = msg.source.funcname ? msg.source.funcname : "";
        }
        details["thread"] = msg.thread_id;
        auto record = nlohmann::json{{"id", newLogCorrelationId()}, {"timestamp", recordTime(msg.time)},
            {"level", std::string(logLevelName(level))}, {"module", sanitizeLogText(module)},
            {"message", safe}, {"details", safeDetails(details)}};
        auto& levelText = record["level"].get_ref<std::string&>();
        for (auto& c : levelText) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (details.contains("msg_id") && details["msg_id"].is_string()) record["msg_id"] = sanitizeLogText(details["msg_id"].get<std::string>());
        // Normalize malformed plugin bytes before handing strings to JSON APIs.
        record = nlohmann::json::parse(record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        safe = record["message"].get<std::string>();
        const auto normalizedModule = record["module"].get<std::string>();
        {
            std::lock_guard lock(recordsMutex);
            records.push_back(record);
            while (records.size() > 1000) records.pop_front();
        }
        const Emission normalized{level, normalizedModule, &details};
        EmissionScope scope(normalized);
        auto cleaned = msg;
        cleaned.payload = spdlog::string_view_t(safe.data(), safe.size());
        cleaned.logger_name = spdlog::string_view_t(normalizedModule.data(), normalizedModule.size());
        const auto* previous = currentRecord;
        currentRecord = &record;
        struct Restore { const nlohmann::json* value; ~Restore() { currentRecord = value; } } restore{previous};
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
        logger->log(level, "HTTP 框架：{}", line);
    }, [logger] { logger->flush(); });
}
} // namespace

LogLevel parseLogLevel(std::string_view text) {
    std::string key(text);
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key == "trace") return LogLevel::Trace;
    if (key == "debug") return LogLevel::Debug;
    if (key == "success") return LogLevel::Success;
    if (key == "warn" || key == "warning") return LogLevel::Warn;
    if (key == "error" || key == "err" || key == "critical" || key == "fatal") return LogLevel::Error;
    if (key == "off") return LogLevel::Off;
    return LogLevel::Info;
}
std::string_view logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Success: return "SUCCESS";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
        default: return "INFO";
    }
}
bool validLogLevel(std::string_view text) {
    return text == "trace" || text == "debug" || text == "info" || text == "success" || text == "warn" || text == "error";
}
LogLevel logRecordingLevel() { return recordingLevel.load(); }
bool shouldRecordLog(LogLevel level) { return level != LogLevel::Off && level >= recordingLevel.load(); }
std::string_view defaultLogModule(spdlog::source_loc source) {
    const std::string_view path = source.filename ? source.filename : "";
    const auto file = baseFile(path);
    // Classified from the emitting subsystem, never guessed from its text.
    if (file == "lua_plugin_manager.cpp") return "Lua 插件";
    if (file == "js_plugin_manager.cpp") return "JS 插件";
    if (file.find("adapter") != std::string::npos) return "适配器";
    if (file == "config_manager.cpp" || file == "hot_reload.cpp") return "配置";
    if (file == "http_diagnostics.h" || file == "api_service.h" || path.empty()) return "管理面板";
    if (file.find("import") != std::string::npos) return "数据导入";
    if (file == "database.cpp" || file == "migration.cpp") return "数据库";
    if (file == "card_deck.cpp") return "牌堆";
    if (file == "card_store.cpp") return "人物卡";
    if (file == "persona_manager.cpp") return "人格";
    if (file == "reply_manager.cpp" || file == "reply_matcher.cpp") return "高级回复";
    if (file == "causal_rule_manager.cpp" || file == "counter_store.cpp") return "因果规则";
    if (file == "command_router.h" || file == "dice_engine.cpp") return "指令";
    if (file == "i18n.cpp" || file == "locale_resolver.cpp") return "回复翻译";
    if (file == "update_service.cpp") return "自动更新";
    if (file == "heart_service.h") return "云心跳";
    if (file == "cloudban_service.h") return "云黑名单";
    if (file == "ai_gateway.h") return "AI 服务";
    if (file == "ai_translate.h") return "AI 翻译";
    if (file == "ai_polish.h") return "AI 润色";
    if (file == "ai_worker.h" || file == "bounded_executor.h") return "后台任务";
    if (file == "notice_manager.h") return "通知";
    if (file == "cq_code.cpp") return "消息";
    return "系统";
}
void writeLog(spdlog::source_loc source, LogLevel level, std::string_view module, std::string_view text) {
    writeLog(source, level, module, text, nlohmann::json::object());
}
void writeLog(spdlog::source_loc source, LogLevel level, std::string_view module, std::string_view text,
              const nlohmann::json& details) {
    auto logger = std::static_pointer_cast<SafeLogger>(getLogger());
    logger->emit(source, level, module, text, details);
}
nlohmann::json currentLogRecord() {
    return currentRecord ? *currentRecord : nlohmann::json::object();
}

nlohmann::json recentLogRecords(size_t limit) {
    std::lock_guard lock(recordsMutex);
    auto out = nlohmann::json::array();
    const size_t start = records.size() - (std::min)(limit, records.size());
    for (size_t i = start; i < records.size(); ++i) out.push_back(records[i]);
    return out;
}
std::string newLogCorrelationId() {
    static const auto session = [] {
        std::random_device random;
        return fmt::format("{:08x}{:08x}", random(), random());
    }();
    static std::atomic<uint64_t> sequence{0};
    return session + "-" + std::to_string(++sequence);
}

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
    // Visible message text belongs in logs; encoded attachment bytes do not.
    const auto hideInlineMedia = [&](std::string_view prefix) {
        for (size_t at = 0; (at = safe.find(prefix, at)) != std::string::npos;) {
            size_t begin = at;
            if (prefix == ";base64,") {
                const auto uri = safe.rfind("data:", at);
                if (uri == std::string::npos || safe.find_first_of(" \t\r\n\"'[]", uri) < at) { at += prefix.size(); continue; }
                begin = uri;
            }
            auto end = safe.find_first_of(" \t\r\n\"'[],}", at + prefix.size());
            if (end == std::string::npos) end = safe.size();
            safe.replace(begin, end - begin, "[内嵌媒体数据]");
            at = begin + std::string_view("[内嵌媒体数据]").size();
        }
    };
    hideInlineMedia("base64://");
    hideInlineMedia(";base64,");
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
    const auto parsed = parseLogLevel(level);
    getLogger()->set_level(toTransport(parsed));
    recordingLevel.store(parsed);
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
    try { writeLog(source, transportLevel(level), defaultLogModule(source),
        std::string(context) + "：" + describeException(std::current_exception())); }
    catch (...) { std::fputs("Dice!Next: failed to record an exception\n", stderr); }
}

LogOperation::LogOperation(spdlog::source_loc source, std::string_view component, std::string_view operation,
                           std::string_view detail)
    : source_(source), component_(component), operation_(operation), detail_(detail), id_(++nextOperation),
      exceptions_(std::uncaught_exceptions()), start_(std::chrono::steady_clock::now()) {
    const auto module = component_ == "lua" ? "Lua 插件" : component_ == "js" ? "JS 插件"
        : component_ == "rules" ? "规则包" : "配置";
    writeLog(source_, LogLevel::Debug, module, "开始执行操作；具体对象见诊断详情。",
        {{"operation_id", id_}, {"operation", operation_}, {"phase", "begin"}, {"context", detail_}});
}
void LogOperation::phase(std::string_view name) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    writeLog(source_, LogLevel::Debug, defaultLogModule(source_), "操作进入下一阶段：" + std::string(name),
        {{"operation_id", id_}, {"operation", operation_}, {"phase", name}, {"elapsed_ms", ms}});
}
LogOperation::~LogOperation() noexcept {
    try {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
        const bool failed = failed_ || std::uncaught_exceptions() > exceptions_;
        const auto action = operation_ == "reload" ? "重载" : operation_ == "load_plugin" ? "插件加载"
            : operation_ == "on_load" ? "插件初始化回调" : operation_ == "file_change" ? "配置变更处理"
            : operation_ == "sync_js_templates" ? "JS 规则模板同步" : "操作";
        writeLog(source_, failed ? LogLevel::Error : partial_ ? LogLevel::Warn : operation_ == "reload" ? LogLevel::Success : LogLevel::Debug,
            defaultLogModule(source_), fmt::format("{}{}，耗时 {} 毫秒{}", action, failed ? "失败" : partial_ ? "部分完成" : "完成", ms,
                (failed || partial_) && !detail_.empty() ? "；原因：" + detail_ : "。"),
            {{"operation_id", id_}, {"operation", operation_}, {"phase", "end"},
                {"result", failed ? "failed" : partial_ ? "partial" : "completed"}, {"elapsed_ms", ms}, {"context", detail_}});
    } catch (...) {}
}

void initLogger(const std::string& level) {
    std::lock_guard lock(initMutex);
    if (std::atomic_load(&g_logger)) return;

    // Create console sink with color
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_formatter(std::make_unique<HumanLogFormatter>(true));

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
        file_sink->set_formatter(std::make_unique<HumanLogFormatter>());
        sinks.push_back(file_sink);
        sinks.push_back(std::make_shared<StructuredLogSink>(base.substr(0, base.size() - 4)));
    } catch (const std::exception& e) { fileError = e.what(); }
    std::shared_ptr<spdlog::logger> logger = std::make_shared<SafeLogger>("dice", sinks.begin(), sinks.end());

    // Set log level
    const auto lvl = parseLogLevel(level);
    recordingLevel.store(lvl);
    logger->set_level(toTransport(lvl));
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
    if (!fileError.empty()) logger->error("日志文件无法创建，继续使用控制台输出；请检查日志目录写入权限：{}", fileError);
}

std::shared_ptr<spdlog::logger> getLogger() {
    if (!std::atomic_load(&g_logger)) {
        // Fallback: initialize with defaults
        initLogger("info");
    }
    return std::atomic_load(&g_logger);
}

}  // namespace dice
