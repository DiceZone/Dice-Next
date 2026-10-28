#pragma once

#include "config/config_manager.h"
#include <filesystem>
#include <string>

namespace dice::webui {
inline bool validVersion(const std::string& version) {
    return version == "v1" || version == "v2";
}

// Keep the document root fixed: old tabs must still be able to load their own
// chunks after another browser changes the instance-wide preference.
inline json versions(const ConfigManager& config, const std::filesystem::path& root) {
    const auto file = [&](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(root / path, error);
    };
    const bool v1 = file("v1/index.html"), v2 = file("v2/index.html");
    const bool legacy = !v1 && !v2 && file("index.html");
    auto selected = config.get<std::string>("webui/ui_version", "v1");
    if (!validVersion(selected)) selected = "v1";
    const auto effective = (selected == "v2" && v2) ? "v2" :
        ((v1 || legacy) ? "v1" : (v2 ? "v2" : ""));
    return {{"selected", selected}, {"effective", effective}, {"legacy", legacy},
            {"available", {{"v1", v1 || legacy}, {"v2", v2}}}};
}

inline bool selectVersion(ConfigManager& config, const std::filesystem::path& root,
                          const json& body, std::string& error) {
    if (!body.is_object() || body.size() != 1 || !body.contains("version") ||
        !body["version"].is_string() || !validVersion(body["version"].get<std::string>())) {
        error = "请选择有效的 WebUI 版本（v1 / v2）。"; return false;
    }
    const auto version = body["version"].get<std::string>();
    if (!versions(config, root)["available"][version].get<bool>()) {
        error = "安装包中缺少此 WebUI 版本，请安装包含两个版本的完整程序包。"; return false;
    }
    if (!config.setAndSave("webui/ui_version", version)) {
        error = "无法保存 WebUI 版本，请检查 config/webui.json 的写入权限。"; return false;
    }
    return true;
}
} // namespace dice::webui
