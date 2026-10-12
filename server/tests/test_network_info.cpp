#include "test_framework.h"
#include "test_watchdog.h"
#include "../src/service/network_info_service.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>
using namespace dice;
using namespace std::chrono_literals;
namespace {
bool until(const std::function<bool()>& ready) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!ready()) { if (std::chrono::steady_clock::now() > end) return false; std::this_thread::sleep_for(1ms); }
    return true;
}
}
TEST(NetworkInfo, ValidatedAddressesAreSeparateAndPrivateInterfacesComeFirst) {
    auto local = NetworkInfoService::normalize({"8.8.8.8", " 192.168.1.8\n", "10.0.0.5", "192.168.1.8",
        "127.0.0.1", "169.254.1.1", "0.0.0.0", "224.1.1.1", "256.1.1.1", "01.2.3.4", "<html>8.8.8.8</html>"});
    ASSERT_EQ(local.size(), size_t(3)); ASSERT_EQ(local[0], std::string("192.168.1.8"));
    ASSERT_EQ(local[2], std::string("8.8.8.8"));
    auto publicIp = NetworkInfoService::normalize({"10.0.0.5", "172.16.1.1", "192.168.0.1", "100.64.1.1", "8.8.4.4\n"}, true);
    ASSERT_EQ(publicIp.size(), size_t(1)); ASSERT_EQ(publicIp[0], std::string("8.8.4.4"));
}
TEST(NetworkInfo, StartupPublishesLanWithoutWaitingForWanAndReadsNeverDetect) {
    TestWatchdog watchdog;
    std::atomic<int> localCalls{0}, publicCalls{0}; std::atomic<bool> release{false};
    NetworkInfoService service([&](const auto&) { ++localCalls; return std::vector<std::string>{"192.168.1.8"}; },
        [&](const auto& cancelled) { ++publicCalls; while (!release && !cancelled()) std::this_thread::sleep_for(1ms); return std::vector<std::string>{"8.8.8.8"}; });
    ASSERT_FALSE(service.snapshot().refreshing);
    ASSERT_EQ(service.snapshot().wan.status, std::string("failed"));
    service.start(); service.start();
    ASSERT_TRUE(until([&] { return publicCalls == 1; }));
    const auto started = std::chrono::steady_clock::now();
    const auto initial = service.snapshot();
    ASSERT_TRUE(std::chrono::steady_clock::now() - started < 500ms);
    ASSERT_EQ(initial.lan.status, std::string("ready")); ASSERT_EQ(initial.wan.status, std::string("detecting"));
    ASSERT_TRUE(initial.refreshing); ASSERT_FALSE(service.refresh());
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(service.snapshot().json().contains("lan"));
    ASSERT_EQ(localCalls.load(), 1); ASSERT_EQ(publicCalls.load(), 1);
    release = true; ASSERT_TRUE(until([&] { return !service.snapshot().refreshing; }));
    const auto done = service.snapshot();
    ASSERT_EQ(done.wan.status, std::string("ready"));
    ASSERT_TRUE(done.lan.addresses[0] != done.wan.addresses[0]); ASSERT_TRUE(done.wan.checkedAt > 0);
}
TEST(NetworkInfo, FailureFinishesAndRetainedCacheIsExplicitlyStaleThenRecovers) {
    std::atomic<int> calls{0};
    NetworkInfoService service([](const auto&) { return std::vector<std::string>{"10.0.0.1"}; },
        [&](const auto&) { const int n = ++calls; return n == 2 ? std::vector<std::string>{} : std::vector<std::string>{n == 1 ? "8.8.8.8" : "1.1.1.1"}; });
    service.start(); ASSERT_TRUE(until([&] { return !service.snapshot().refreshing; }));
    const auto first = service.snapshot().wan.checkedAt;
    ASSERT_TRUE(service.refresh()); ASSERT_TRUE(until([&] { return !service.snapshot().refreshing; }));
    const auto failed = service.snapshot();
    ASSERT_EQ(failed.wan.status, std::string("failed")); ASSERT_TRUE(failed.wan.stale);
    ASSERT_EQ(failed.wan.addresses[0], std::string("8.8.8.8")); ASSERT_EQ(failed.wan.checkedAt, first);
    ASSERT_TRUE(service.refresh()); ASSERT_TRUE(until([&] { return !service.snapshot().refreshing; }));
    ASSERT_FALSE(service.snapshot().wan.stale); ASSERT_EQ(service.snapshot().wan.addresses[0], std::string("1.1.1.1"));
}
TEST(NetworkInfo, OfflineAndDetectorExceptionsDoNotStayDetecting) {
    NetworkInfoService service([](const auto&) { return std::vector<std::string>{}; },
        [](const auto&) -> std::vector<std::string> { throw std::runtime_error("isolated-network-probe"); });
    service.start(); ASSERT_TRUE(until([&] { return !service.snapshot().refreshing; }));
    const auto state = service.snapshot();
    ASSERT_EQ(state.lan.status, std::string("failed")); ASSERT_EQ(state.wan.status, std::string("failed"));
    ASSERT_TRUE(state.wan.addresses.empty()); ASSERT_FALSE(state.wan.stale);
}
TEST(NetworkInfo, ShutdownCancelsBackgroundWorkAndPeriodicRefreshNeedsNoBrowser) {
    TestWatchdog watchdog;
    std::atomic<int> calls{0};
    NetworkInfoService periodic([](const auto&) { return std::vector<std::string>{"10.1.1.1"}; },
        [&](const auto&) { ++calls; return std::vector<std::string>{"8.8.8.8"}; }, 20ms);
    periodic.start(); ASSERT_TRUE(until([&] { return calls >= 2; })); periodic.stop();
    std::atomic<bool> entered{false};
    NetworkInfoService slow([](const auto&) { return std::vector<std::string>{}; },
        [&](const auto& cancelled) { entered = true; while (!cancelled()) std::this_thread::sleep_for(1ms); return std::vector<std::string>{}; });
    slow.start(); ASSERT_TRUE(until([&] { return entered.load(); }));
    const auto start = std::chrono::steady_clock::now(); slow.stop();
    ASSERT_TRUE(std::chrono::steady_clock::now() - start < 500ms);
    ASSERT_FALSE(slow.snapshot().refreshing); ASSERT_FALSE(slow.refresh());
}
TEST(NetworkInfo, NativeInterfaceEnumerationIsReadOnlyAndFiltersUnusableAddresses) {
    const auto addresses = NetworkInfoService::localAddresses([] { return false; });
    ASSERT_TRUE(NetworkInfoService::normalize(addresses) == addresses);
    ASSERT_TRUE(NetworkInfoService::localAddresses([] { return true; }).empty());
}
