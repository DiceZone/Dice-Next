#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace dice {
// Advisory only: an egress IP does not prove inbound reachability. This service
// never opens a listener or changes firewall/adapter settings.
class NetworkInfoService {
public:
    using Cancelled = std::function<bool()>;
    using Detector = std::function<std::vector<std::string>(const Cancelled&)>;
    struct AddressState {
        std::string status = "failed"; // never claim a check is running before start()
        std::vector<std::string> addresses;
        bool stale = false;
        int64_t checkedAt = 0;
    };
    struct Snapshot {
        AddressState lan, wan;
        bool refreshing = false;
        int64_t attemptedAt = 0;
        nlohmann::json json() const;
    };
    explicit NetworkInfoService(Detector local = {}, Detector publicIp = {},
        std::chrono::milliseconds refreshEvery = std::chrono::minutes(15));
    ~NetworkInfoService();
    NetworkInfoService(const NetworkInfoService&) = delete;
    void start();
    void stop();
    bool refresh(); // coalesces requests; never detects on the HTTP thread
    Snapshot snapshot() const; // cached read; no I/O, even during a check
    static std::vector<std::string> normalize(std::vector<std::string> values, bool publicOnly = false);
    static std::vector<std::string> localAddresses(const Cancelled& cancelled);
private:
    static std::vector<std::string> publicAddress(const Cancelled& cancelled);
    void run();
    void detect(bool publicIp);
    Detector local_, public_;
    std::chrono::milliseconds refreshEvery_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Snapshot state_;
    bool started_ = false, requested_ = false;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};
} // namespace dice
