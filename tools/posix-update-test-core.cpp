// Isolated process fixture: no real bot, credentials, ports or databases.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include "../server/src/platform/instance_guard.h"
int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--update-probe") return 0;
    namespace fs = std::filesystem;
    if (argc > 2 && std::string(argv[2]) == "direct") {
        fs::create_directories("data");
        if (!dice::acquireInstanceLock("data/.instance.lock")) return 5;
        if (!fs::exists("direct-first")) {
            std::ofstream("direct-first") << ::getpid();
            ::execv(argv[0], argv);
            return 6;
        }
    }
    nlohmann::json arguments = nlohmann::json::array();
    for (int i = 1; i < argc; ++i) arguments.push_back(argv[i]);
    std::ifstream version("web/dist/index.html");
    const std::string text((std::istreambuf_iterator<char>(version)), {});
    std::ofstream("last-run.json") << nlohmann::json{
        {"pid", ::getpid()}, {"parent", ::getppid()}, {"cwd", fs::current_path().string()},
        {"version", text}, {"args", arguments}}.dump();
    if (argc > 2 && std::string(argv[2]) == "hold") {
        while (true) ::pause();
    }
    if (!fs::exists("first-run") && argc > 2 && std::string(argv[2]) == "restart") {
        std::ofstream("first-run") << ::getpid();
        fs::remove("updates/pending/install-held");
        return 42;
    }
    return 0;
}
