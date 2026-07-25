#include "hot_reload.h"
#include "logger.h"

#include <filesystem>
#include <unordered_map>
#include <utility>

#ifdef __linux__
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#include <limits.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fileapi.h>
#endif

namespace dice {

namespace fs = std::filesystem;

// ─── Constructor / Destructor ────────────────────────────────

HotReloadMonitor::HotReloadMonitor() = default;

HotReloadMonitor::~HotReloadMonitor() {
    stop();
}

// ─── Public API ──────────────────────────────────────────────

bool HotReloadMonitor::start(const std::string& path, int debounceMs, ChangeCallback callback) {
    if (running_.load()) {
        DICE_LOG_WARN("HotReloadMonitor: already running, stopping first");
        stop();
    }

    watchPath_ = path;
    debounceMs_ = debounceMs;
    callback_ = std::move(callback);

    running_.store(true);
    watcherThread_ = std::thread(&HotReloadMonitor::watchLoop, this);

    DICE_LOG_INFO("HotReloadMonitor: started watching '{}' (debounce {}ms)", watchPath_, debounceMs_);
    return true;
}

void HotReloadMonitor::stop() {
    if (!running_.load()) return;

    running_.store(false);

#ifdef __linux__
    cleanupLinuxWatcher();
#endif
#ifdef _WIN32
    cleanupWindowsWatcher();
#endif

    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }

    DICE_LOG_INFO("HotReloadMonitor: stopped");
}

// ─── Watch Loop (platform-agnostic skeleton) ─────────────────

void HotReloadMonitor::watchLoop() {
    DICE_LOG_DEBUG("HotReloadMonitor: watch loop started for '{}'", watchPath_);

#ifdef __linux__
    if (!initLinuxWatcher()) {
        DICE_LOG_ERROR("HotReloadMonitor: failed to initialize inotify for '{}'", watchPath_);
        running_.store(false);
        return;
    }

    // Buffer for inotify events
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (running_.load()) {
        struct pollfd pfd;
        pfd.fd = inotifyFd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);  // 500ms timeout for checking running_ flag
        if (ret < 0) {
            if (running_.load()) {
                DICE_LOG_ERROR("HotReloadMonitor: poll() error: {}", strerror(errno));
            }
            break;
        }

        if (ret == 0) {
            continue;  // timeout, loop back to check running_ flag
        }

        ssize_t len = read(inotifyFd_, buf, sizeof(buf));
        if (len < 0) {
            if (running_.load()) {
                DICE_LOG_ERROR("HotReloadMonitor: read() error: {}", strerror(errno));
            }
            break;
        }

        // Process inotify events
        const struct inotify_event* event;
        for (char* ptr = buf; ptr < buf + len;
             ptr += sizeof(struct inotify_event) + event->len) {
            event = reinterpret_cast<const struct inotify_event*>(ptr);

            // Ignore events on non-regular files and temporary files
            if (event->len == 0) continue;
            if (event->name[0] == '.') continue;  // skip hidden/tmp files
            if (event->mask & IN_ISDIR) continue;  // skip directory-only events

            std::string changedFile(event->name);
            DICE_LOG_DEBUG("HotReloadMonitor: change detected in '{}'", changedFile);

            // Debounce: wait for quiet period
            std::this_thread::sleep_for(std::chrono::milliseconds(debounceMs_));

            // Drain any remaining events during debounce period
            char drainBuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            while (poll(&pfd, 1, 0) > 0) {
                read(inotifyFd_, drainBuf, sizeof(drainBuf));
            }

            if (callback_) {
                callback_(changedFile);
            }
        }
    }

    cleanupLinuxWatcher();
#endif

#ifdef _WIN32
    if (!initWindowsWatcher()) {
        DICE_LOG_ERROR("HotReloadMonitor: failed to initialize Windows watcher for '{}'", watchPath_);
        running_.store(false);
        return;
    }

    // Windows watcher loop using ReadDirectoryChangesW with overlapped I/O
    HANDLE hDir = static_cast<HANDLE>(dirHandle_);
    const DWORD bufferSize = 4096;
    BYTE buffer[bufferSize] = {0};
    DWORD bytesReturned = 0;
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (!overlapped.hEvent) {
        DICE_LOG_ERROR("HotReloadMonitor: CreateEventW failed");
        running_.store(false);
        return;
    }

    while (running_.load()) {
        // Start async read
        BOOL success = ReadDirectoryChangesW(
            hDir,
            buffer, bufferSize,
            TRUE,  // watch subtree
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE
                | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION,
            &bytesReturned,
            &overlapped,
            nullptr
        );

        if (!success) {
            DICE_LOG_ERROR("HotReloadMonitor: ReadDirectoryChangesW failed: {}", GetLastError());
            break;
        }

        // Wait with timeout for checking running_ flag
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 500);
        if (waitResult == WAIT_OBJECT_0) {
            DWORD actualBytes = 0;
            GetOverlappedResult(hDir, &overlapped, &actualBytes, FALSE);

            if (actualBytes > 0) {
                FILE_NOTIFY_INFORMATION* fni =
                    reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);

                do {
                    // Extract filename
                    int nameLen = fni->FileNameLength / sizeof(WCHAR);
                    std::wstring wFilename(fni->FileName, nameLen);
                    std::string changedFile(wFilename.begin(), wFilename.end());

                    DICE_LOG_DEBUG("HotReloadMonitor: change detected in '{}'", changedFile);

                    // Debounce
                    std::this_thread::sleep_for(std::chrono::milliseconds(debounceMs_));

                    if (callback_) {
                        callback_(changedFile);
                    }

                    fni = (fni->NextEntryOffset == 0)
                        ? nullptr
                        : reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                            reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset);
                } while (fni != nullptr);
            }

            ResetEvent(overlapped.hEvent);
        } else if (waitResult == WAIT_TIMEOUT) {
            // Normal timeout — continue loop
            CancelIo(hDir);
            ResetEvent(overlapped.hEvent);
            continue;
        } else {
            DICE_LOG_ERROR("HotReloadMonitor: WaitForSingleObject failed: {}", GetLastError());
            break;
        }
    }

    CloseHandle(overlapped.hEvent);
    cleanupWindowsWatcher();
#endif

    DICE_LOG_DEBUG("HotReloadMonitor: watch loop ended");
}

// ─── Platform: Linux inotify ─────────────────────────────────

#ifdef __linux__
bool HotReloadMonitor::initLinuxWatcher() {
    inotifyFd_ = inotify_init1(IN_NONBLOCK);
    if (inotifyFd_ < 0) {
        DICE_LOG_ERROR("HotReloadMonitor: inotify_init1 failed: {}", strerror(errno));
        return false;
    }

    // Watch for file creation, modification, deletion, and moves
    uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;

    // Add watches for all subdirectories recursively
    std::function<void(const fs::path&)> addWatchRecursive =
        [&](const fs::path& dirPath) {
            int wd = inotify_add_watch(inotifyFd_, dirPath.c_str(), mask);
            if (wd < 0) {
                DICE_LOG_WARN("HotReloadMonitor: failed to watch '{}': {}", dirPath.string(), strerror(errno));
                return;
            }

            try {
                for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
                    if (entry.is_directory()) {
                        int subWd = inotify_add_watch(inotifyFd_, entry.path().c_str(), mask);
                        if (subWd >= 0) {
                            // Store watch descriptors for cleanup
                        }
                    }
                }
            } catch (const fs::filesystem_error& e) {
                DICE_LOG_WARN("HotReloadMonitor: filesystem error: {}", e.what());
            }
        };

    addWatchRecursive(fs::path(watchPath_));

    DICE_LOG_DEBUG("HotReloadMonitor: Linux inotify initialized, fd={}", inotifyFd_);
    return true;
}

void HotReloadMonitor::cleanupLinuxWatcher() {
    if (inotifyFd_ >= 0) {
        close(inotifyFd_);
        inotifyFd_ = -1;
    }
}
#endif

// ─── Platform: Windows ReadDirectoryChangesW ─────────────────

#ifdef _WIN32
bool HotReloadMonitor::initWindowsWatcher() {
    std::wstring wPath(watchPath_.begin(), watchPath_.end());

    HANDLE hDir = CreateFileW(
        wPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        DICE_LOG_ERROR("HotReloadMonitor: CreateFileW failed for '{}': {}", watchPath_, GetLastError());
        return false;
    }

    dirHandle_ = static_cast<void*>(hDir);
    DICE_LOG_DEBUG("HotReloadMonitor: Windows watcher initialized for '{}'", watchPath_);
    return true;
}

void HotReloadMonitor::cleanupWindowsWatcher() {
    if (dirHandle_) {
        HANDLE hDir = static_cast<HANDLE>(dirHandle_);
        CancelIo(hDir);
        CloseHandle(hDir);
        dirHandle_ = nullptr;
    }
}
#endif

}  // namespace dice
