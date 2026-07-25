#pragma once
// ─── Dice!Next — triggered announcement broadcast ────────────
// A single global pending broadcast. It is NOT pushed to every group at once
// (that would look like spam and risk the account being banned). Instead, when
// a group actively triggers the bot (any command produces a reply), the
// broadcast is piggy-backed onto that reply ONCE per group. The buffer holds at
// most one pending broadcast; setting a new one replaces it and resets delivery.

#include <string>
#include <set>
#include <mutex>
#include <ctime>

namespace dice {

class BroadcastManager {
public:
    static BroadcastManager& instance() { static BroadcastManager b; return b; }

    void set(const std::string& content) {
        std::lock_guard<std::mutex> lk(m_);
        content_ = content; active_ = !content.empty();
        pushed_.clear(); createdAt_ = (int64_t)std::time(nullptr);
    }
    void cancel() {
        std::lock_guard<std::mutex> lk(m_);
        content_.clear(); active_ = false; pushed_.clear();
    }

    /// Broadcast text for this group if it should be pushed now (once), else "".
    /// key = "<platform>:<groupId>".
    std::string takeFor(const std::string& key) {
        std::lock_guard<std::mutex> lk(m_);
        if (!active_ || content_.empty() || pushed_.count(key)) return "";
        pushed_.insert(key);
        return content_;
    }

    bool active() const { std::lock_guard<std::mutex> lk(m_); return active_; }
    std::string content() const { std::lock_guard<std::mutex> lk(m_); return content_; }
    int pushedCount() const { std::lock_guard<std::mutex> lk(m_); return (int)pushed_.size(); }
    int64_t createdAt() const { std::lock_guard<std::mutex> lk(m_); return createdAt_; }

private:
    mutable std::mutex m_;
    std::string content_;
    bool active_ = false;
    std::set<std::string> pushed_;
    int64_t createdAt_ = 0;
};

}  // namespace dice
