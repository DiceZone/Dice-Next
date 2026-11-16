#pragma once
#include "../adapter/adapter_interface.h"
#include "../common/logger.h"
#include <algorithm>
#include <array>
#include <memory>
#include <vector>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace dice::aichat {

enum class TurnAction { Reply, Wait, Skip, Recheck, DropFirst };

// The first message owns the reply target, identity and tools. Later speakers
// contribute context but never lend their permissions to that first message.
class ConversationTurns {
public:
    using Clock = std::chrono::steady_clock;
    using Key = std::array<std::string, 4>; // platform, adapter, group, role (empty = regular chat)
    using Task = std::function<TurnAction(const std::vector<Message>&, bool, const std::function<bool()>&)>;
    using Schedule = std::function<bool(Clock::time_point, std::function<void()>)>;
    explicit ConversationTurns(Schedule schedule, std::function<Clock::time_point()> now = Clock::now)
        : schedule_(std::move(schedule)), now_(std::move(now)) {}

    std::optional<std::string> currentRole(const Key& room) {
        std::lock_guard lock(mutex_);
        prune();
        std::optional<std::string> role;
        Clock::time_point last = Clock::time_point::min();
        for (const auto& [key, state] : states_) {
            if (key[0] == room[0] && key[1] == room[1] && key[2] == room[2] && state.touched >= last) {
                role = state.role;
                last = state.touched;
            }
        }
        return role;
    }

    bool offer(const Key& key, const Message& message, Task task) {
        std::lock_guard lock(mutex_);
        prune();
        if (!states_.contains(key) && states_.size() >= 128) return false;
        auto& state = states_[key];
        if (!message.id.empty())
            for (const auto& entry : state.lines) if (entry.message.id == message.id) return true;
        if (state.lines.size() >= 24) return false;
        auto time = now_();
        bool first = state.lines.empty();
        state.lines.push_back({message, std::make_shared<Task>(std::move(task)), time});
        state.touched = time;
        if (!first) return true;
        state.waited = false;
        state.role = key[3];
        return arm(key, time + quiet);
    }

    void resolvedRole(const Key& key, const std::string& role) {
        std::lock_guard lock(mutex_);
        if (auto it = states_.find(key); it != states_.end()) it->second.role = role;
    }

private:
    struct Entry { Message message; std::shared_ptr<Task> task; Clock::time_point at; };
    struct State {
        std::deque<Entry> lines;
        std::string role;
        Clock::time_point touched{}, expires{};
        bool waited = false;
    };
    static constexpr auto quiet = std::chrono::milliseconds(1300);
    void prune() {
        auto time = now_();
        std::erase_if(states_, [&](const auto& kv) { return kv.second.lines.empty() && kv.second.expires <= time; });
    }
    bool arm(const Key& key, Clock::time_point at) {
        if (schedule_(at, [this, key] { flush(key); })) return true;
        states_.erase(key);
        DICE_LOG_WARN_M("AI 对话", "群 {} 的 AI 任务队列已满或正在关闭，本轮结束。", key[2]);
        return false;
    }
    void flush(const Key& key) {
        std::vector<Message> messages;
        std::shared_ptr<Task> task;
        bool canWait;
        {
            std::lock_guard lock(mutex_);
            auto& s = states_.at(key);
            auto time = now_();
            auto start = s.lines.front().at;
            if (time - start > std::chrono::seconds(90)) { states_.erase(key); return; }
            auto due = std::min(s.lines.back().at + quiet, start + std::chrono::seconds(5));
            if (due > time && s.lines.size() < 8) { arm(key, due); return; }
            for (auto& e : s.lines) { messages.push_back(e.message); if (messages.size() == 8) break; }
            task = s.lines.front().task;
            canWait = !s.waited && messages.size() < 8 && time - start < std::chrono::seconds(12);
        }
        auto newer = [&] {
            std::lock_guard lock(mutex_);
            return canWait && states_.at(key).lines.size() > messages.size();
        };
        TurnAction action = TurnAction::Skip;
        try { action = (*task)(messages, canWait, newer); }
        catch (...) { DICE_LOG_EXCEPTION("AI conversation turn"); }
        std::lock_guard lock(mutex_);
        auto& s = states_.at(key);
        if (canWait && (action == TurnAction::Wait || action == TurnAction::Recheck)) {
            s.waited = true;
            auto delay = s.lines.size() > messages.size() ? quiet : std::chrono::milliseconds(6000);
            arm(key, now_() + delay);
            return;
        }
        if (s.waited) s.expires = std::max(s.expires, s.lines.front().at + std::chrono::seconds(40));
        // An unrecognized voice trigger or newly disabled sender does not consume
        // independent messages already waiting behind it.
        auto consumed = action == TurnAction::DropFirst ? 1 : messages.size();
        s.lines.erase(s.lines.begin(), s.lines.begin() + consumed);
        s.waited = false;
        if (action == TurnAction::Reply) s.expires = now_() + std::chrono::seconds(40);
        if (!s.lines.empty()) arm(key, now_() + quiet);
        else prune();
    }

    std::mutex mutex_;
    std::map<Key, State> states_;
    Schedule schedule_;
    std::function<Clock::time_point()> now_;
};
} // namespace dice::aichat
