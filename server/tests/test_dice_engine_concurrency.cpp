#include "test_framework.h"
#include "core/dice/dice_engine.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using namespace dice;

namespace {
std::string utf8Path(const std::filesystem::path& p) { const auto s = p.u8string(); return {s.begin(), s.end()}; }
}

// Every adapter thread, the HTTP API, plugins and the AI worker share one
// engine. Unsynchronized parsing corrupted concurrent rolls, which .coc then
// printed as an attribute of 0.
TEST(DiceEngineConcurrency, ParallelRollsNeverFailOrLeaveRange) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("dicenext-engine-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    {
        ConfigManager cfg{utf8Path(dir / "config")};
        cfg.resetDefault();
        DiceEngine engine{cfg};
        std::atomic<int> failures{0}, outOfRange{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&, t] {
                const char* expr = t % 2 ? "2d6+6" : "3d6";
                const int low = t % 2 ? 8 : 3, high = 18;
                for (int i = 0; i < 3000; ++i) {
                    const auto r = engine.roll(expr);
                    if (!r.ok()) ++failures;
                    else if (r.modifiedTotal < low || r.modifiedTotal > high) ++outOfRange;
                }
            });
        for (auto& th : threads) th.join();
        ASSERT_EQ(failures.load(), 0);
        ASSERT_EQ(outOfRange.load(), 0);
    }
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
