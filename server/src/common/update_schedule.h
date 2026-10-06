#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace dice::update {

// The launcher needs no JSON/runtime dependencies. New packages stay held until
// the core authorizes installation, including the auto-install-on-restart policy.
// Legacy packages remain compatible.
inline constexpr const char* kUpdateHoldFile = "install-held";
inline constexpr const char* kUpdateInstallOnRestartFile = "install-on-restart";

inline bool pendingUpdateHeld(const std::filesystem::path& stage) {
    std::error_code error;
    const bool held = std::filesystem::exists(stage / kUpdateHoldFile, error);
    return held || bool(error); // Fail closed if the gate cannot be inspected.
}

inline bool pendingUpdateMayApply(const std::filesystem::path& stage) {
    if (!pendingUpdateHeld(stage)) return true; // Explicit install or legacy stage.
    std::error_code error;
    const bool authorized = std::filesystem::is_regular_file(stage / kUpdateInstallOnRestartFile, error);
    return authorized && !error; // Automatic install policy: every restart is an update window.
}

inline bool validInstallTime(const std::string& time) {
    return time.size() == 5 && time[2] == ':' &&
        time[0] >= '0' && time[0] <= '2' && time[1] >= '0' && time[1] <= '9' &&
        time[3] >= '0' && time[3] <= '5' && time[4] >= '0' && time[4] <= '9' &&
        (time[0] - '0') * 10 + time[1] - '0' < 24;
}

// First occurrence strictly after download completion, in the server's timezone.
inline std::int64_t nextInstallTime(std::int64_t now, int offsetMinutes,
                                    const std::string& time) {
    if (!validInstallTime(time)) return 0;
    const auto local = now + static_cast<std::int64_t>(offsetMinutes) * 60;
    const auto secondsOfDay = (local % 86400 + 86400) % 86400;
    const auto target = ((time[0] - '0') * 10 + time[1] - '0') * 3600 +
        ((time[3] - '0') * 10 + time[4] - '0') * 60;
    auto wait = static_cast<std::int64_t>(target) - secondsOfDay;
    if (wait <= 0) wait += 86400;
    return now + wait;
}

} // namespace dice::update
