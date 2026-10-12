#include "hot_reload.h"
#include "logger.h"

#include <filesystem>
#include <unordered_map>
#include <utility>
#include <algorithm>

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

bool HotReloadMonitor::start(const std::string& path, int debounceMs, ChangeCallback callback,
                             ChangeFilter filter) {
    if (running_.load() || watcherThread_.joinable()) {
        DICE_LOG_WARN("HotReloadMonitor: joining previous watcher before restart");
        stop();
    }

    watchPath_ = path;
    debounceMs_ = (std::max)(0, debounceMs);
    callback_ = std::move(callback);
    filter_ = std::move(filter);

    running_.store(true);
    watcherThread_ = std::thread([this] {
        try { watchLoop(); }
        catch (...) { DICE_LOG_EXCEPTION("configuration watcher stopped unexpectedly"); }
#ifdef __linux__
        cleanupLinuxWatcher();
#endif
#ifdef _WIN32
        cleanupWindowsWatcher();
#endif
        running_.store(false);
    });

    DICE_LOG_INFO("HotReloadMonitor: started watching '{}' (debounce {}ms)", watchPath_, debounceMs_);
    return true;
}

void HotReloadMonitor::stop() {
    if (!running_.load() && !watcherThread_.joinable()) return;

    running_.store(false);

    // The watcher owns its OS handle and any outstanding I/O. Closing it from
    // this thread races GetOverlappedResult / poll and can leave a signalled
    // completion event for a cancelled read.
    if (watcherThread_.joinable()) {
        watcherThread_.join();
    }

    DICE_LOG_INFO("HotReloadMonitor: stopped");
}

// ─── Watch Loop (platform-agnostic skeleton) ─────────────────

void HotReloadMonitor::notifyChange(const std::string& filename) noexcept {
    try {
        LogOperation operation(DICE_LOG_SOURCE, "config", "file_change", "file=" + filename);
        if (callback_) callback_(filename);
    } catch (...) { DICE_LOG_EXCEPTION("configuration reload callback file=" + filename); }
}

void HotReloadMonitor::watchLoop() {
    DICE_LOG_DEBUG("HotReloadMonitor: watch loop started for '{}'", watchPath_);
    std::string pendingFile;
    auto quietUntil = (std::chrono::steady_clock::time_point::max)();
    const auto changed = [&](const std::string& relativePath) {
        if (filter_ && !filter_(relativePath)) return;
        pendingFile = relativePath;
        quietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(debounceMs_);
        DICE_LOG_DEBUG("检测到配置文件变更：{}；等待写入稳定。", relativePath);
    };
    const auto flushPending = [&] {
        if (!pendingFile.empty() && std::chrono::steady_clock::now() >= quietUntil) {
            auto filename = std::exchange(pendingFile, std::string{});
            notifyChange(filename);
        }
    };

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

        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (running_.load()) {
                DICE_LOG_ERROR("HotReloadMonitor: poll() error: {}", strerror(errno));
            }
            break;
        }

        if (ret == 0) {
            flushPending();
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

            const auto dir = linuxPaths_.find(event->wd);
            changed((dir != linuxPaths_.end() ? dir->second : std::string()) + event->name);
        }
        flushPending();
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
    alignas(DWORD) BYTE buffer[bufferSize] = {0};
    DWORD bytesReturned = 0;
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (!overlapped.hEvent) {
        DICE_LOG_ERROR("HotReloadMonitor: CreateEventW failed");
        running_.store(false);
        cleanupWindowsWatcher();
        return;
    }

    bool readPending = false;
    struct PendingReadScope {
        HANDLE directory;
        OVERLAPPED& operation;
        bool& pending;
        void close() noexcept {
            if (pending) {
                CancelIoEx(directory, &operation);
                DWORD ignored = 0;
                GetOverlappedResult(directory, &operation, &ignored, TRUE);
                pending = false;
            }
            if (operation.hEvent) { CloseHandle(operation.hEvent); operation.hEvent = nullptr; }
        }
        ~PendingReadScope() { close(); }
    } readScope{hDir, overlapped, readPending};
    while (running_.load()) {
        // Start async read
        if (!readPending) {
            ResetEvent(overlapped.hEvent);
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
            readPending = true;
        }

        // Wait with timeout for checking running_ flag
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            DWORD actualBytes = 0;
            if (!GetOverlappedResult(hDir, &overlapped, &actualBytes, FALSE)) {
                const auto error = GetLastError();
                if (running_.load()) DICE_LOG_ERROR("配置目录监视失败：Windows 错误 {}。", error);
                break;
            }
            readPending = false;

            if (actualBytes > 0) {
                FILE_NOTIFY_INFORMATION* fni =
                    reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                std::string changedFile;

                do {
                    // Extract filename
                    int nameLen = fni->FileNameLength / sizeof(WCHAR);
                    std::wstring wFilename(fni->FileName, nameLen);
#if defined(_WIN32)
                    int wlen = WideCharToMultiByte(CP_UTF8, 0, wFilename.c_str(), (int)wFilename.size(), nullptr, 0, nullptr, nullptr);
                    if (wlen > 0) { std::string tmp(wlen, '\0'); WideCharToMultiByte(CP_UTF8, 0, wFilename.c_str(), (int)wFilename.size(), tmp.data(), wlen, nullptr, nullptr); changedFile = tmp; }
#else
                    changedFile.assign(wFilename.begin(), wFilename.end());
#endif

                    changed(changedFile);

                    fni = (fni->NextEntryOffset == 0)
                        ? nullptr
                        : reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                            reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset);
                } while (fni != nullptr);

            } else {
                // Windows discarded an overflowing batch. Read the full
                // configuration once after a quiet period rather than losing
                // an external edit. Semantic comparison rejects false alarms.
                changed("server.json");
            }
        } else if (waitResult == WAIT_TIMEOUT) {
            flushPending();
            // Keep this read pending. A timeout is not an I/O completion;
            // reusing OVERLAPPED before cancellation completes is unsafe.
            continue;
        } else {
            DICE_LOG_ERROR("HotReloadMonitor: WaitForSingleObject failed: {}", GetLastError());
            break;
        }
        flushPending();
    }

    readScope.close();
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

    linuxPaths_.clear();
    // Keep relative directory names so a nested JSON file cannot impersonate
    // a top-level configuration section.
    std::function<void(const fs::path&)> addWatchRecursive =
        [&](const fs::path& dirPath) {
            int wd = inotify_add_watch(inotifyFd_, dirPath.c_str(), mask);
            if (wd < 0) {
                DICE_LOG_WARN("HotReloadMonitor: failed to watch '{}': {}", dirPath.string(), strerror(errno));
                return;
            }
            linuxPaths_[wd] = "";

            try {
                for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
                    if (entry.is_directory()) {
                        int subWd = inotify_add_watch(inotifyFd_, entry.path().c_str(), mask);
                        if (subWd >= 0) {
                            linuxPaths_[subWd] = fs::relative(entry.path(), dirPath).generic_string() + "/";
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
    linuxPaths_.clear();
}
#endif

// ─── Platform: Windows ReadDirectoryChangesW ─────────────────

#ifdef _WIN32
bool HotReloadMonitor::initWindowsWatcher() {
    const std::wstring wPath = fs::u8path(watchPath_).wstring();

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
