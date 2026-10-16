#pragma once
#include "../../adapter/adapter_interface.h"
#include <filesystem>
#include <ctime>

namespace dice::legacy_log {
inline std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string(); return {text.begin(), text.end()};
}
// log_file is the actual exported filename, not the display title. The host
// chooses exportPath; a plugin/user-authored title must never become a path.
inline json context(const Message& message, int id, const std::string& name,
                    const std::filesystem::path& exportPath, size_t count, std::time_t now) {
    return {{"uid", message.senderId}, {"gid", message.targetId}, {"platform", message.platform},
        {"adapterId", message.adapterId}, {"selfId", message.selfId}, {"fromMsg", message.content},
        {"fromUser", message.senderId}, {"fromQQ", message.senderId}, {"fromGroup", message.targetId},
        {"nick", message.senderName}, {"msgid", message.id}, {"time", now},
        {"log_id", id}, {"log_file", utf8(exportPath.filename())},
        {"log_path", utf8(std::filesystem::absolute(exportPath).lexically_normal())},
        {"log_name", name.empty() ? "log" + std::to_string(id) : name}, {"log_count", count}};
}
} // namespace dice::legacy_log
