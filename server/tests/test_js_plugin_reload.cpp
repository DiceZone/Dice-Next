#include "test_framework.h"
#include "../src/core/mod/js_plugin_manager.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dice;
namespace fs = std::filesystem;

TEST(JsPluginManager, ReloadAfterInitializationDoesNotSelfDeadlock) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_js_reload_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(static_cast<bool>(ec));

    {
        JsPluginManager manager;
        ASSERT_TRUE(manager.init());
        ASSERT_EQ(manager.loadDir(root.string()), 0);
        // WebUI upload/toggle/delete all finish by calling this reload path.
        std::ofstream plugin(root / "upload-regression.js", std::ios::binary);
        plugin << "const ext = seal.ext.new('upload-regression', 'Dice!Next', '1.0.0');\n"
                  "seal.ext.register(ext);\n";
        plugin.close();
        ASSERT_EQ(manager.reload(root.string()), 1);
    }

    fs::remove_all(root, ec);
}
