#pragma once
// ─── Dice!Next v3.0.0 — Cooldown Manager (C#29) ──────────────
// In-memory cooldown tracking for the causal rule engine.
// Each cooldown is identified by a composite key:
//   "{ruleId}:{cooldownKey}:{scopeId}"
// e.g. "5:per-user:12345", "5:per-group:67890", "5:global:"
//
// P0 in-memory approach: cooldowns reset on restart (acceptable per PRD Q4).

#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <chrono>

namespace dice {

class CooldownManager {
public:
    /// Check if a cooldown is currently active for @p key.
    /// @param key        Composite key: "{ruleId}:{cooldownKey}:{scopeId}"
    /// @param cooldownMs Cooldown duration in milliseconds
    /// @return true if still cooling (should not trigger), false if ready.
    bool isCooling(const std::string& key, int cooldownMs) const;

    /// Record a trigger — sets the timestamp to "now".
    void trigger(const std::string& key);

    /// Clear the cooldown for a specific key (e.g. on rule edit).
    void clear(const std::string& key);

    /// Clear all cooldowns (e.g. on full reload).
    void clearAll();

    /// Remaining milliseconds until the cooldown expires.
    /// Returns 0 if not cooling or cooldownMs <= 0.
    int64_t remainingMs(const std::string& key, int cooldownMs) const;

private:
    /// Map: composite key → last-trigger timestamp (ms since epoch).
    mutable std::unordered_map<std::string, int64_t> timestamps_;
    mutable std::mutex mutex_;

    /// Get current time in milliseconds since epoch.
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
};

}  // namespace dice
