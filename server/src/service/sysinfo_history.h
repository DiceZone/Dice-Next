#pragma once
// 后端持续采样主机 CPU / 内存（每 5 秒，保留 1 小时），首页曲线直接读取这份历史，
// 不再依赖浏览器停留在首页；接口也改为返回缓存快照，不在请求线程里阻塞约 120ms 测 CPU。
#include "../platform/system_info.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace dice::sysinfo {

struct Sample {
    int64_t t = 0;      // unix seconds
    double cpu = -1;    // -1 = unavailable on this host
    int mem = 0;        // percent
};

/// Fixed-capacity history; pure logic so it is testable without threads.
class SampleRing {
public:
    explicit SampleRing(size_t capacity) : capacity_(capacity) {}
    void push(const Sample& sample) {
        std::lock_guard lock(mu_);
        // A clock step backwards would otherwise make "since" skip new samples.
        if (!samples_.empty() && sample.t <= samples_.back().t) samples_.clear();
        samples_.push_back(sample);
        while (samples_.size() > capacity_) samples_.pop_front();
    }
    /// Samples strictly newer than @p t (0 = everything kept).
    std::vector<Sample> since(int64_t t) const {
        std::lock_guard lock(mu_);
        std::vector<Sample> out;
        for (const auto& sample : samples_) if (sample.t > t) out.push_back(sample);
        return out;
    }
    size_t size() const { std::lock_guard lock(mu_); return samples_.size(); }
    size_t capacity() const { return capacity_; }
private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::deque<Sample> samples_;
};

class HistorySampler {
public:
    static constexpr int kIntervalSeconds = 5;
    static constexpr size_t kCapacity = 720;      // one hour of 5-second samples

    static HistorySampler& instance() { static HistorySampler sampler; return sampler; }

    /// Starts the background sampler once; later calls are no-ops.
    void start(const std::filesystem::path& dataPath) {
        std::lock_guard lock(mu_);
        if (thread_.joinable()) return;
        dataPath_ = dataPath;
        thread_ = std::thread([this] { run(); });
    }
    /// The newest snapshot; gathers once synchronously before the first sample exists.
    SysInfo latest() {
        {
            std::lock_guard lock(mu_);
            if (latest_) return *latest_;
        }
        return sample();
    }
    std::vector<Sample> since(int64_t t) const { return ring_.since(t); }

    ~HistorySampler() {
        { std::lock_guard lock(mu_); stopping_ = true; }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    HistorySampler(const HistorySampler&) = delete;
    HistorySampler& operator=(const HistorySampler&) = delete;

private:
    HistorySampler() : ring_(kCapacity) {}

    SysInfo sample() {
        std::lock_guard gatherLock(gatherMu_);     // gather() keeps static caches
        std::filesystem::path path;
        { std::lock_guard lock(mu_); path = dataPath_; }
        SysInfo info = gather(path.empty() ? std::filesystem::path(".") : path);
        ring_.push({static_cast<int64_t>(std::time(nullptr)), info.cpuLoadPct, info.memLoadPct});
        std::lock_guard lock(mu_);
        latest_ = info;
        return info;
    }
    void run() {
        for (;;) {
            try { sample(); } catch (...) { /* keep sampling; one failed probe is skipped */ }
            std::unique_lock lock(mu_);
            if (wake_.wait_for(lock, std::chrono::seconds(kIntervalSeconds), [this] { return stopping_; })) return;
        }
    }

    SampleRing ring_;
    std::mutex mu_, gatherMu_;
    std::condition_variable wake_;
    std::thread thread_;
    std::filesystem::path dataPath_;
    std::optional<SysInfo> latest_;
    bool stopping_ = false;
};

} // namespace dice::sysinfo
