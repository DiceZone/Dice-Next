#pragma once
// ─── Dice!Next — in-memory group chat buffer ─────────────────
// A small per-group ring buffer of recent messages, used by the web admin's
// "模拟聊天" (simulated chat) view. Fed from the main message loop (incoming
// messages + bot replies) and read/written by the REST API. Not persisted —
// it's a live window, capped per group.

#include <string>
#include <deque>
#include <map>
#include <mutex>
#include <ctime>
#include <nlohmann/json.hpp>

namespace dice {

class GroupChatLog {
public:
    static GroupChatLog& instance() { static GroupChatLog g; return g; }

    /// key = "<platform>:<groupId>". `self` marks the bot's own lines.
    /// @p userId is the sender's platform id (QQ 号 / Discord id…) for avatar + 标识。
    void add(const std::string& key, const std::string& sender, const std::string& userId,
             const std::string& content, bool self) {
        if (content.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        auto& dq = buf_[key];
        dq.push_back(Line{++seq_, sender, userId, content, self, (int64_t)std::time(nullptr)});
        while (dq.size() > kCap) dq.pop_front();
    }

    /// Recent lines for a group (oldest→newest), at most `limit`.
    nlohmann::json recent(const std::string& key, int limit = 50) const {
        std::lock_guard<std::mutex> lk(m_);
        nlohmann::json arr = nlohmann::json::array();
        auto it = buf_.find(key);
        if (it == buf_.end()) return arr;
        const auto& dq = it->second;
        int start = (int)dq.size() - limit;
        if (start < 0) start = 0;
        for (int i = start; i < (int)dq.size(); ++i) {
            const auto& l = dq[i];
            arr.push_back({{"id", l.id}, {"sender", l.sender}, {"userId", l.userId},
                           {"content", l.content}, {"self", l.self}, {"time", l.time}});
        }
        return arr;
    }

private:
    struct Line { uint64_t id; std::string sender; std::string userId; std::string content; bool self; int64_t time; };
    static constexpr size_t kCap = 100;
    mutable std::mutex m_;
    std::map<std::string, std::deque<Line>> buf_;
    uint64_t seq_ = 0;
};

}  // namespace dice
