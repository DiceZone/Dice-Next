#include "counter_store.h"
#include "../../common/logger.h"
#include "../../common/utils.h"

#include <sqlite_orm/sqlite_orm.h>
#include <sstream>

namespace dice {

namespace orm = sqlite_orm;

CounterStore::CounterStore(Database& db) : db_(db) {}

CounterEntry CounterStore::parseKey(const std::string& key) {
    CounterEntry e;
    e.key = key;
    // Key format: "{ruleId}:{counterName}:{scope}:{scopeId}"
    // counterName itself may contain colons? No — it's a user-defined name
    // without colons (validated by the API). So we split by ':' into exactly 4 parts.
    std::stringstream ss(key);
    std::string part;
    std::vector<std::string> parts;
    while (std::getline(ss, part, ':')) parts.push_back(part);
    if (parts.size() >= 1) { try { e.ruleId = std::stoi(parts[0]); } catch (...) {} }
    if (parts.size() >= 2) e.counterName = parts[1];
    if (parts.size() >= 3) e.scope = parts[2];
    if (parts.size() >= 4) e.scopeId = parts[3];
    return e;
}

int CounterStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return 0;
    try {
        auto rows = st->get_all<RuleCounterRow>(
            orm::where(orm::c(&RuleCounterRow::key) == key), orm::limit(1));
        if (!rows.empty()) return rows.front().value;
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CounterStore: get('{}') failed: {}", key, e.what());
    }
    return 0;
}

void CounterStore::set(const std::string& key, int value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return;
    try {
        auto rows = st->get_all<RuleCounterRow>(
            orm::where(orm::c(&RuleCounterRow::key) == key), orm::limit(1));
        if (!rows.empty()) {
            auto row = rows.front();
            row.value = value;
            row.updatedAt = utils::nowIso8601();
            st->update(row);
        } else {
            RuleCounterRow row;
            row.key = key;
            row.value = value;
            row.updatedAt = utils::nowIso8601();
            st->insert(row);
        }
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CounterStore: set('{}', {}) failed: {}", key, value, e.what());
    }
}

int CounterStore::add(const std::string& key, int delta) {
    int newVal = get(key) + delta;
    set(key, newVal);
    return newVal;
}

void CounterStore::reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return;
    try {
        st->remove_all<RuleCounterRow>(orm::where(orm::c(&RuleCounterRow::key) == key));
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CounterStore: reset('{}') failed: {}", key, e.what());
    }
}

std::vector<CounterEntry> CounterStore::listAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CounterEntry> result;
    auto* st = db_.getStorage();
    if (!st) return result;
    try {
        auto rows = st->get_all<RuleCounterRow>();
        result.reserve(rows.size());
        for (auto& r : rows) {
            CounterEntry e = parseKey(r.key);
            e.value = r.value;
            e.updatedAt = r.updatedAt;
            result.push_back(std::move(e));
        }
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CounterStore: listAll() failed: {}", e.what());
    }
    return result;
}

std::vector<CounterEntry> CounterStore::listByRule(int ruleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CounterEntry> result;
    auto* st = db_.getStorage();
    if (!st) return result;
    try {
        auto rows = st->get_all<RuleCounterRow>();
        std::string prefix = std::to_string(ruleId) + ":";
        result.reserve(rows.size());
        for (auto& r : rows) {
            if (r.key.rfind(prefix, 0) != 0) continue;
            CounterEntry e = parseKey(r.key);
            e.value = r.value;
            e.updatedAt = r.updatedAt;
            result.push_back(std::move(e));
        }
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CounterStore: listByRule({}) failed: {}", ruleId, e.what());
    }
    return result;
}

void CounterStore::deleteByRule(int ruleId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return;
    try {
        auto rows = st->get_all<RuleCounterRow>();
        std::string prefix = std::to_string(ruleId) + ":";
        for (auto& r : rows) {
            if (r.key.rfind(prefix, 0) == 0) {
                st->remove<RuleCounterRow>(r.id);
            }
        }
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CounterStore: deleteByRule({}) failed: {}", ruleId, e.what());
    }
}

}  // namespace dice
