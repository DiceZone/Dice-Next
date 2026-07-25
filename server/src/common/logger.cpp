#include "logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <vector>
#include <memory>
#include <ctime>
#include <string>

namespace dice {

static std::shared_ptr<spdlog::logger> g_logger = nullptr;

void initLogger(const std::string& level) {
    if (g_logger) {
        return;  // already initialized
    }

    // Create console sink with color
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");

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
    std::string base = std::string("data/logs/app/dice-") + ts + ".log";
    // daily_file_sink：文件名 basename_YYYY-MM-DD.log，每天 00:00 切；启动时间戳保证每次启动新系列。
    auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(base, 0, 0);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");

    std::vector<spdlog::sink_ptr> sinks{ console_sink, file_sink };

    g_logger = std::make_shared<spdlog::logger>("dice", sinks.begin(), sinks.end());

    // Set log level
    auto lvl = spdlog::level::from_str(level);
    g_logger->set_level(lvl);
    g_logger->flush_on(spdlog::level::info);

    // Register globally so drogon can also pick it up
    spdlog::register_logger(g_logger);
    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> getLogger() {
    if (!g_logger) {
        // Fallback: initialize with defaults
        initLogger("info");
    }
    return g_logger;
}

}  // namespace dice
