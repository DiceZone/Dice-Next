#pragma once
// ─── Dice!Next v3.0.0 — Counter Store ─────────────────
// Persistent counter storage for the causal rule engine.
// Counters are identified by a composite key:
//   "{ruleId}:{counterName}:{scope}:{scopeId}"
// e.g. "5:sign_days:user:12345", "5:total:global:"
//
// Persisted to the rule_counters table (sqlite_orm). Survives restarts.

#include "../../storage/database.h"
#include <string>
#include <vector>
#include <mutex>

namespace dice {

/// A single counter entry for listing/display.
struct CounterEntry {
    std::string key;
    int value = 0;
    std::string updatedAt;
    // Parsed components (for display):
    int ruleId = 0;
    std::string counterName;
    std::string scope;
    std::string scopeId;
};

class CounterStore {
public:
    explicit CounterStore(Database& db);

    /// Get the current value of a counter. Returns 0 if not found.
    int get(const std::string& key) const;

    /// Set a counter to an explicit value.
    void set(const std::string& key, int value);

    /// Add a delta to a counter (can be negative). Returns the new value.
    int add(const std::string& key, int delta);

    /// Reset a counter to 0 (or delete it).
    void reset(const std::string& key);

    /// List all counters.
    std::vector<CounterEntry> listAll() const;

    /// List counters belonging to a specific rule ID.
    std::vector<CounterEntry> listByRule(int ruleId) const;

    /// Delete all counters for a specific rule (used when a rule is deleted).
    void deleteByRule(int ruleId);

private:
    Database& db_;
    mutable std::mutex mutex_;

    /// Parse a composite key into its components.
    static CounterEntry parseKey(const std::string& key);
};

}  // namespace dice
