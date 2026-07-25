#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>

namespace dice {

// ─── Logger Initialization ───────────────────────────────────

void initLogger(const std::string& level = "info");

// ─── Shared Logger Instance ──────────────────────────────────

std::shared_ptr<spdlog::logger> getLogger();

// ─── Convenience Macros ──────────────────────────────────────

#define DICE_LOG_TRACE(...)    dice::getLogger()->trace(__VA_ARGS__)
#define DICE_LOG_DEBUG(...)    dice::getLogger()->debug(__VA_ARGS__)
#define DICE_LOG_INFO(...)     dice::getLogger()->info(__VA_ARGS__)
#define DICE_LOG_WARN(...)     dice::getLogger()->warn(__VA_ARGS__)
#define DICE_LOG_ERROR(...)    dice::getLogger()->error(__VA_ARGS__)
#define DICE_LOG_CRITICAL(...) dice::getLogger()->critical(__VA_ARGS__)

}  // namespace dice
