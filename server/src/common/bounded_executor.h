#pragma once
#include "logger.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace dice {
// A plugin timer must never run script or wait for its runtime on the I/O loop.
// One worker per language bounds concurrency; a finite queue bounds memory.
class BoundedExecutor {
public:
    explicit BoundedExecutor(size_t capacity = 128) : capacity_(capacity), worker_([this] { run(); }) {}
    ~BoundedExecutor() { stop(); }
    bool submit(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || tasks_.size() >= capacity_) return false;
        try { tasks_.push_back(std::move(task)); }
        catch (...) { DICE_LOG_EXCEPTION("plugin callback queue allocation"); return false; }
        changed_.notify_one();
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            tasks_.clear(); // don't execute stale callbacks during teardown
            changed_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }
private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
                if (stopped_) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try { task(); }
            catch (...) { DICE_LOG_EXCEPTION("plugin background callback"); }
        }
    }
    const size_t capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    bool stopped_ = false;
    std::thread worker_;
};
} // namespace dice
