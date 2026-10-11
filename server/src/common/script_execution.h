#pragma once
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>

namespace dice {
class ScriptBusy : public std::runtime_error {
public:
    ScriptBusy() : std::runtime_error("插件运行时忙碌，请稍后重试") {}
};

// One budget covers the complete turn, including getters, Promise jobs and
// nested host calls. Entering a second runtime must not renew the first budget.
class ScriptExecution {
public:
    explicit ScriptExecution(std::chrono::milliseconds timeout)
        : previous_(current_), deadline_(std::chrono::steady_clock::now() + timeout) {
        if (previous_) deadline_ = std::min(deadline_, previous_->deadline_);
        current_ = this;
    }
    ~ScriptExecution() { current_ = previous_; }
    ScriptExecution(const ScriptExecution&) = delete;
    static bool expired() {
        if (!current_) return false;
        if (current_->interrupted_ || std::chrono::steady_clock::now() >= current_->deadline_)
            current_->interrupted_ = true;
        return current_->interrupted_;
    }
    static bool active() { return current_ != nullptr; }
private:
    inline static thread_local ScriptExecution* current_ = nullptr;
    ScriptExecution* previous_;
    std::chrono::steady_clock::time_point deadline_;
    bool interrupted_ = false;
};

// BasicLockable, with bounded admission. In particular, four busy plugin API
// calls must not occupy all four HTTP threads indefinitely. Recursive entry is
// needed by native bridges (WebSocket construction and legacy eventMsg).
class ScriptMutex {
public:
    explicit ScriptMutex(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
        : timeout_(std::clamp(timeout, std::chrono::milliseconds(10), std::chrono::milliseconds(60000))) {}
    void lock() {
        if (!mutex_.try_lock_for(std::chrono::milliseconds(100))) throw ScriptBusy();
        if (depth_++ == 0) execution_.emplace(timeout_);
    }
    void unlock() {
        if (--depth_ == 0) execution_.reset();
        mutex_.unlock();
    }
private:
    std::recursive_timed_mutex mutex_;
    unsigned depth_ = 0; // accessed only while holding mutex_
    std::chrono::milliseconds timeout_;
    std::optional<ScriptExecution> execution_;
};

// Keep a repeatedly failing callback from delaying every subsequent message.
// Reloading clears this in-memory circuit breaker; files are never renamed.
class ScriptFaultScope {
public:
    ScriptFaultScope(std::set<std::string>& faults, const std::string& source)
        : faults_(faults), source_(source) {}
    ~ScriptFaultScope() { if (ScriptExecution::expired()) faults_.insert(source_); }
private:
    std::set<std::string>& faults_;
    const std::string source_;
};
} // namespace dice
