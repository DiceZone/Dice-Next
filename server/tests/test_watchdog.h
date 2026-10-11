#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

// Infinite-loop regressions must fail CI instead of hanging its entire job.
class TestWatchdog {
public:
    explicit TestWatchdog(std::chrono::seconds limit = std::chrono::seconds(5))
        : worker_([this, limit] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!changed_.wait_for(lock, limit, [this] { return finished_; })) {
                std::cerr << "runtime reliability test exceeded its watchdog\n";
                std::_Exit(2);
            }
        }) {}
    ~TestWatchdog() {
        { std::lock_guard<std::mutex> lock(mutex_); finished_ = true; }
        changed_.notify_one();
        worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool finished_ = false;
    std::thread worker_;
};
