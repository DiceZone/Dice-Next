#pragma once

#include <string>

namespace dice::heart {

// Return true only after an adapter that was previously online has remained
// offline for the full debounce window.  Seeing it online clears a pending
// offline observation immediately.
inline bool offlineTransitionReady(
    const std::string& lastReportedStatus,
    bool connected,
    long long now,
    long long& firstOfflineObservedAt,
    long long debounceSeconds = 60
) {
    if (connected || lastReportedStatus != "online") {
        firstOfflineObservedAt = 0;
        return false;
    }
    if (firstOfflineObservedAt == 0) {
        firstOfflineObservedAt = now;
        return false;
    }
    return now - firstOfflineObservedAt >= debounceSeconds;
}

}  // namespace dice::heart
