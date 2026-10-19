#pragma once
// ─── Dice!Next — in-memory group chat buffer ─────────────────
// A small per-group ring buffer of recent messages, used by the web admin's
// "模拟聊天" (simulated chat) view. Fed from the main message loop (incoming
// messages + bot replies) and read/written by the REST API. Not persisted —
// it's a live window, capped per group.  When chat history is not saved to
// chat.db this buffer is the window's only source, so it also keeps message
// ids (quotes, de-duplication) and recall marks.

#include <algorithm>
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
             const std::string& content, bool self, const std::string& msgId = {}) {
        insert(key, sender, userId, content, self, msgId, (int64_t)std::time(nullptr));
    }

    /// Platform history: placed by its own timestamp; a message already present
    /// (same platform message id) is skipped.
    void addHistory(const std::string& key, const std::string& sender, const std::string& userId,
                    const std::string& content, bool self, const std::string& msgId, int64_t time) {
        insert(key, sender, userId, content, self, msgId, time > 0 ? time : (int64_t)std::time(nullptr));
    }

    void markRecalled(const std::string& key, const std::string& msgId) {
        if (msgId.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        auto it = buf_.find(key);
        if (it == buf_.end()) return;
        for (auto& line : it->second) if (line.msgId == msgId) line.recalled = true;
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
                           {"content", l.content}, {"self", l.self}, {"time", l.time},
                           {"recalled", l.recalled}, {"msgId", l.msgId}});
        }
        return arr;
    }

private:
    struct Line {
        uint64_t id; std::string sender; std::string userId; std::string content; bool self; int64_t time;
        std::string msgId; bool recalled = false;
    };

    void insert(const std::string& key, const std::string& sender, const std::string& userId,
                const std::string& content, bool self, const std::string& msgId, int64_t time) {
        if (content.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        auto& dq = buf_[key];
        if (!msgId.empty() && std::any_of(dq.begin(), dq.end(), [&](const Line& l) { return l.msgId == msgId; }))
            return;
        // Keep time order: live lines append; older history slots in before newer lines.
        auto pos = std::upper_bound(dq.begin(), dq.end(), time,
                                    [](int64_t t, const Line& l) { return t < l.time; });
        dq.insert(pos, Line{++seq_, sender, userId, content, self, time, msgId});
        while (dq.size() > kCap) dq.pop_front();
    }

    static constexpr size_t kCap = 100;
    mutable std::mutex m_;
    std::map<std::string, std::deque<Line>> buf_;
    uint64_t seq_ = 0;
};

}  // namespace dice
