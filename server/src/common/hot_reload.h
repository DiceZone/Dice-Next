#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>

namespace dice {

// ─── Change Callback Type ────────────────────────────────────

using ChangeCallback = std::function<void(const std::string& path)>;

// ─── HotReloadMonitor ────────────────────────────────────────
// Cross-platform directory watcher.
//   Linux:   inotify
//   Windows: ReadDirectoryChangesW
// Watches config/ and data/ directories, invokes the callback
// on any file change after a configurable debounce period.

class HotReloadMonitor {
public:
    HotReloadMonitor();
    ~HotReloadMonitor();

    // ─── Lifecycle ───────────────────────────────────────────

    /// Start watching the given directory path. A change detected
    /// in any file within this path (or its subdirectories) will
    /// trigger `callback` after `debounceMs` milliseconds of
    /// inactivity.
    /// Returns true on success.
    bool start(const std::string& path, int debounceMs, ChangeCallback callback);

    /// Stop the monitor and join the watcher thread.
    void stop();

    /// Whether the monitor is currently running.
    bool isRunning() const noexcept { return running_.load(); }

private:
    std::atomic<bool> running_{false};
    std::thread watcherThread_;
    std::string watchPath_;
    int debounceMs_{500};
    ChangeCallback callback_;

    // ─── Platform-specific implementations ───────────────────

    void watchLoop();

#ifdef __linux__
    bool initLinuxWatcher();
    void cleanupLinuxWatcher();
    int inotifyFd_{-1};
    int inotifyWd_{-1};
#endif

#ifdef _WIN32
    bool initWindowsWatcher();
    void cleanupWindowsWatcher();
    void* dirHandle_{nullptr};  // HANDLE
    void* completionPort_{nullptr};  // HANDLE for IOCP
#endif
};

}  // namespace dice
