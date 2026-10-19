#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <atomic>
#include <string>
#include <string_view>

namespace dice {
// A prepared reply belongs to the exact connection/account and permission
// snapshot that produced it. A replacement with the same adapter ID is not it.
struct DeferredReplyAuthority {
    std::weak_ptr<const void> connection;
    std::string account, permissions;
    template<class Connection>
    bool matches(const std::shared_ptr<Connection>& current, std::string_view currentAccount,
                 std::string_view currentPermissions) const {
        const auto original = connection.lock();
        return original && current && original.get() == current.get()
            && account == currentAccount && permissions == currentPermissions;
    }
};

// In-memory, bounded delayed replies. No sleeping threads and no persisted
// replay after a restart. Destruction invalidates callbacks before host teardown.
class DeferredReplyQueue {
    struct State {
        std::mutex mutex;
        size_t count = 0, bytes = 0;
        std::atomic<bool> active{true};
    };
    struct Ticket {
        std::weak_ptr<State> state;
        size_t bytes = 0;
        ~Ticket() {
            if (const auto owner = state.lock()) {
                std::lock_guard lock(owner->mutex); --owner->count; owner->bytes -= bytes;
            }
        }
    };
public:
    using Schedule = std::function<void(double,std::function<void()>)>;
    explicit DeferredReplyQueue(Schedule schedule) : schedule_(std::move(schedule)) {}
    ~DeferredReplyQueue() { cancel(); }
    DeferredReplyQueue(const DeferredReplyQueue&) = delete;
    DeferredReplyQueue& operator=(const DeferredReplyQueue&) = delete;
    bool post(unsigned milliseconds, size_t bytes, std::function<void()> callback) {
        if (!schedule_ || !callback || !milliseconds || milliseconds > 600000 || bytes > 16 * 1024 * 1024) return false;
        const auto state = state_.load();
        if (!state) return false;
        auto ticket = std::make_shared<Ticket>();
        {
            std::lock_guard lock(state->mutex);
            if (!state->active || state->count >= 256 || state->bytes > 16 * 1024 * 1024 - bytes) return false;
            ++state->count; state->bytes += bytes;
            ticket->state = state; ticket->bytes = bytes;
        }
        try {
            schedule_(milliseconds / 1000.0, [ticket,callback=std::move(callback)] {
                if (const auto owner = ticket->state.lock(); owner && owner->active) callback();
            });
            return true;
        } catch (...) { return false; }
    }
    size_t pending() const {
        const auto state = state_.load(); if (!state) return 0;
        std::lock_guard lock(state->mutex); return state->count;
    }
    void cancel() { if (const auto state = state_.exchange({})) state->active = false; }
private:
    Schedule schedule_;
    std::atomic<std::shared_ptr<State>> state_{std::make_shared<State>()};
};
} // namespace dice
