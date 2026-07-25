#include "cooldown_manager.h"

namespace dice {

bool CooldownManager::isCooling(const std::string& key, int cooldownMs) const {
    if (cooldownMs <= 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = timestamps_.find(key);
    if (it == timestamps_.end()) return false;
    int64_t elapsed = nowMs() - it->second;
    return elapsed < cooldownMs;
}

void CooldownManager::trigger(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    timestamps_[key] = nowMs();
}

void CooldownManager::clear(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    timestamps_.erase(key);
}

void CooldownManager::clearAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    timestamps_.clear();
}

int64_t CooldownManager::remainingMs(const std::string& key, int cooldownMs) const {
    if (cooldownMs <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = timestamps_.find(key);
    if (it == timestamps_.end()) return 0;
    int64_t elapsed = nowMs() - it->second;
    int64_t remaining = cooldownMs - elapsed;
    return remaining > 0 ? remaining : 0;
}

}  // namespace dice
