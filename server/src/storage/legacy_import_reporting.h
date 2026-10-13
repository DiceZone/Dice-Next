#pragma once
#include "legacy_import_resources.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <random>

namespace dice::migration_report {
using json = nlohmann::json;
namespace fs = std::filesystem;

inline std::string validateRoot(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return "旧版数据目录不存在或不是目录";
    bool found = false;
    const char* files[] = {"user/PlayerCards.RDconf", "user/PlayerCards.RDconf.bak", "user/PlayerCards.bak",
        "user/UserConf.dat", "user/UserConf.dat.bak", "user/UserConf.bak", "user/UserConf.RDconf",
        "user/ChatConf.dat", "user/ChatConf.dat.bak", "user/ChatConf.bak", "user/ChatConf.RDconf",
        "conf/BlackList.json", "conf/CustomMsgReply.json", "conf/CustomReply.json", "conf/CustomRegexReply.json",
        "conf/CustomHelp.json", "conf/CustomMsg.json", "conf/Console.xml", "conf/console.yaml",
        "conf/LinkList.json", "conf/NoticeList.json", "conf/CustomCensor.json", "conf/BlackListEx.json", "com.w4123.dice/BlackMarks.json"};
    for (const auto* name : files) {
        const auto file = root / name;
        if (!fs::exists(file, ec)) continue;
        if (!migration_resources::safePath(root, file)) return "旧数据文件是链接或超出所选目录，未导入";
        if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) > 0 && !ec) found = true;
    }
    for (const auto* name : {"PublicDeck", "image", "mod", "plugin", "selfdata", "CardTemp", "user/session", "user/log"}) {
        const auto directory = root / name;
        if (!fs::exists(directory, ec)) continue;
        if (!migration_resources::safePath(root, directory)) return "旧数据资源目录是链接或超出所选目录，未导入";
        fs::directory_iterator it(directory, ec), end;
        if (ec) return "无法读取旧数据资源目录";
        if (it != end) found = true;
    }
    return found ? std::string() : "所选目录没有可识别的旧版数据；请选择包含 conf、user 或旧资源的那一层目录";
}

inline void validateJsonSource(const fs::path& file, bool array) {
    if (!fs::exists(file)) return;
    if (!fs::is_regular_file(file) || fs::file_size(file) > 64ULL * 1024 * 1024)
        throw std::runtime_error("旧 JSON 文件不是普通文件或超过 64 MiB");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取旧 JSON 文件");
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("读取旧 JSON 文件失败");
    if (text.empty()) return;
    // Historical raw control characters are repaired by the stage itself.
    std::string clean; bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted && !escaped && (c == '\r' || c == '\n' || c == '\t')) {
            clean += c == '\r' ? "\\r" : c == '\n' ? "\\n" : "\\t"; continue;
        }
        clean += c;
        if (escaped) escaped = false;
        else if (quoted && c == '\\') escaped = true;
        else if (c == '"') quoted = !quoted;
    }
    const auto value = json::parse(clean);
    if (array ? !value.is_array() : !value.is_object()) throw std::runtime_error("旧 JSON 顶层结构不符合预期");
}

inline std::mutex& importMutex() { static std::mutex value; return value; }

inline void save(json& report) {
    const auto token = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())
        + "-" + std::to_string(std::random_device{}());
    const fs::path target = fs::path("data/legacy-import-reports") / (token + ".json");
    report["reportPath"] = migration_resources::utf8(target);
    const auto error = migration_resources::writeFile(target, report.dump(2));
    if (!error.empty()) throw std::runtime_error("无法保存迁移报告: " + error);
}
} // namespace dice::migration_report
