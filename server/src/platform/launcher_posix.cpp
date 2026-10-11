// Foreground POSIX supervisor. systemd/launchd watch this process, not an
// untracked detached successor. No shell and no third-party shared libraries.
#include "posix_update.h"
#include "../common/update_schedule.h"
#include <iostream>
#include <csignal>
#include <sstream>
#include <sys/file.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
namespace update = dice::update;
namespace posix = dice::update::posix;
namespace {
volatile sig_atomic_t childPid = 0;
volatile sig_atomic_t stopping = 0;
void signalHandler(int signal) {
    stopping = signal;
    if (childPid > 0) ::kill(childPid, signal);
}
bool containerRuntime() {
    for (const auto* name : {"DICENEXT_CONTAINER", "DOTNET_RUNNING_IN_CONTAINER", "container", "KUBERNETES_SERVICE_HOST"}) {
        const char* value = std::getenv(name);
        if (value && *value && std::string(value) != "0" && std::string(value) != "false") return true;
    }
    if (fs::exists("/.dockerenv") || fs::exists("/run/.containerenv")) return true;
#ifdef __linux__
    // The core will not arm updates in containers. Keep the supervisor safe
    // too, including a staged directory copied from an earlier host install.
    const auto read = [](const char* path) {
        std::ifstream input(path);
        std::string text(1024 * 1024, '\0');
        input.read(text.data(), text.size());
        text.resize(static_cast<size_t>(input.gcount()));
        return text;
    };
    const auto marker = read("/run/systemd/container");
    if (marker.find_first_not_of(" \t\r\n") != std::string::npos) return true;
    const auto cgroup = read("/proc/self/cgroup") + read("/proc/1/cgroup");
    for (const auto* token : {"kubepods", "/docker/", "docker-", "libpod-", "containerd-", "lxc.payload", "/lxc/", "/garden/"})
        if (cgroup.find(token) != std::string::npos) return true;
    std::istringstream mounts(read("/proc/self/mountinfo"));
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string id, parent, device, root, mountPoint;
        if (!(fields >> id >> parent >> device >> root >> mountPoint) || mountPoint != "/") continue;
        for (const auto* token : {"kubepods", "/kubernetes/", "containers/storage", "libpod", "/var/lib/docker/",
                                  "/docker/containers/", "/var/lib/containerd/", "io.containerd.runtime", "lxcfs"})
            if (line.find(token) != std::string::npos) return true;
    }
#endif
    return false;
}
#ifndef __APPLE__
void setLibraryPath(const fs::path& root, bool probe) {
    const char* previous = std::getenv("DICENEXT_MANAGER_LIBRARY_PATH");
    const auto path = (root / "lib").string() +
        (!probe && previous && *previous ? ":" + std::string(previous) : "");
    ::setenv("LD_LIBRARY_PATH", path.c_str(), 1);
}
#endif

pid_t launch(const fs::path& root, const std::vector<std::string>& args, int lock, int& execError,
             const char* executable = "dice-next-server", [[maybe_unused]] bool probe = false) {
    int pipe[2];
    if (::pipe(pipe) != 0) { execError = errno; return -1; }
    ::fcntl(pipe[1], F_SETFD, FD_CLOEXEC);
    const std::string parent = std::to_string(::getpid()), descriptor = std::to_string(lock);
    ::setenv("DICENEXT_MANAGED", "1", 1);
    ::setenv("DICENEXT_MANAGER_PID", parent.c_str(), 1);
    ::setenv("DICENEXT_MANAGER_LOCK_FD", descriptor.c_str(), 1);
#ifndef __APPLE__
    // Probes use only the new lib/. Ordinary launches preserve the user's
    // original extra paths, not the previous probe or restart's generated path.
    setLibraryPath(root, probe);
#endif
    std::vector<std::string> owned{(root / executable).string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(pipe[0]);
        ::execv(argv[0], argv.data());
        const int error = errno;
        (void)::write(pipe[1], &error, sizeof(error));
        ::_exit(127);
    }
    ::close(pipe[1]);
    if (child < 0) { execError = errno; ::close(pipe[0]); return -1; }
    childPid = child;
    if (stopping) ::kill(child, stopping);
    int error = 0;
    ssize_t size;
    do { size = ::read(pipe[0], &error, sizeof(error)); } while (size < 0 && errno == EINTR);
    ::close(pipe[0]);
    execError = size == 0 ? 0 : size == sizeof(error) ? error : EIO;
    return child;
}
int waitFor(pid_t child) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return 1;
    childPid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
void probeExecutable(const fs::path& stage, int lock, const char* executable) {
    int error = 0;
    const auto child = launch(stage, {"--update-probe"}, lock, error, executable, true);
    if (error) { if (child > 0) waitFor(child); throw std::runtime_error("staged core cannot be executed"); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int status = 0;
    while (true) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) throw std::runtime_error("cannot wait for startup probe");
        if (stopping || std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL); waitFor(child);
            throw std::runtime_error("staged core startup probe timed out");
        }
        ::usleep(10000);
    }
    childPid = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("staged core startup probe failed (loader or runtime unavailable)");
}
void reexecManager(const fs::path& root, const std::vector<std::string>& args, int lock, bool resume) {
#ifndef __APPLE__
    setLibraryPath(root, false);
#endif
    ::setenv("DICENEXT_MANAGER_PID", std::to_string(::getpid()).c_str(), 1);
    ::setenv("DICENEXT_MANAGER_LOCK_FD", std::to_string(lock).c_str(), 1);
    ::setenv("DICENEXT_MANAGER_RESUME_UPDATE", resume ? "1" : "0", 1);
    std::vector<std::string> owned{(root / "dice-next").string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    throw std::runtime_error(std::string("cannot restart package manager: ") + std::strerror(errno));
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--update-probe") return 0;
        const auto root = posix::executablePath().parent_path();
        if (root.empty()) throw std::runtime_error("cannot locate Dice!Next installation");
        fs::current_path(root);
        posix::safeParents(root, "updates/manager.lock", true);
        // Inherited by the core too: even if the manager is killed, another
        // manager cannot replace files underneath an orphaned live core.
        int lock = -1;
        const char* parent = std::getenv("DICENEXT_MANAGER_PID");
        const char* inherited = std::getenv("DICENEXT_MANAGER_LOCK_FD");
        if (parent && inherited && std::string(parent) == std::to_string(::getpid())) {
            struct stat descriptor{}, file{};
            const int fd = std::stoi(inherited);
            if (fd >= 3 && ::fstat(fd, &descriptor) == 0 &&
                ::lstat((root / "updates/manager.lock").c_str(), &file) == 0 &&
                S_ISREG(file.st_mode) && descriptor.st_dev == file.st_dev && descriptor.st_ino == file.st_ino)
                lock = fd;
        }
        const bool inheritedLock = lock >= 3;
#ifndef __APPLE__
        if (!inheritedLock) {
            const char* extra = std::getenv("LD_LIBRARY_PATH");
            ::setenv("DICENEXT_MANAGER_LIBRARY_PATH", extra ? extra : "", 1);
        }
#endif
        if (lock < 0) lock = ::open((root / "updates/manager.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
            std::cerr << "Dice!Next is already running, or the installation is not writable.\n";
            return 1;
        }
        std::signal(SIGTERM, signalHandler);
        std::signal(SIGINT, signalHandler);
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        posix::Transaction transaction(root);
        const char* resumeFlag = std::getenv("DICENEXT_MANAGER_RESUME_UPDATE");
        bool resume = inheritedLock && resumeFlag && std::string(resumeFlag) == "1";
        ::unsetenv("DICENEXT_MANAGER_RESUME_UPDATE");
        if (resume) transaction.resume();
        else if (!containerRuntime()) transaction.recover();
        while (!stopping) {
            const auto stage = root / "updates/pending";
            bool installing = resume;
            resume = false;
            if (!installing && !containerRuntime() && fs::is_directory(stage) && update::pendingUpdateMayApply(stage)) {
                try {
                    posix::validatePackage(stage);
                    probeExecutable(stage, lock, "dice-next-server");
                    probeExecutable(stage, lock, "dice-next");
                    transaction.begin();
                    installing = true;
                    // Replace the manager in memory too, retaining PID, lock and
                    // arguments. The new manager finalizes this transaction.
                    reexecManager(root, args, lock, true);
                } catch (const std::exception& ex) {
                    std::cerr << "Dice!Next update failed: " << ex.what() << '\n';
                    if (transaction.active()) transaction.recover(ex.what());
                    else {
                        nlohmann::json metadata = nlohmann::json::object();
                        try { metadata = posix::readJson(stage / "update.json"); } catch (...) {}
                        posix::writeJson(root / "updates/last-result.json",
                            {{"schema", 1}, {"success", false}, {"metadata", metadata}, {"message", ex.what()}});
                        fs::rename(stage, root / "updates" / ("failed-" + std::to_string(::getpid()) + "-" +
                            std::to_string(std::chrono::system_clock::now().time_since_epoch().count())));
                    }
                    installing = false;
                }
            }
            int execError = 0;
            pid_t child = launch(root, args, lock, execError);
            if (execError != 0) {
                if (child > 0) waitFor(child);
                if (!installing) throw std::runtime_error(std::string("cannot start core: ") + std::strerror(execError));
                transaction.recover(std::string("新核心无法启动，已回滚：") + std::strerror(execError));
                installing = false;
                child = launch(root, args, lock, execError);
                if (execError != 0) {
                    if (child > 0) waitFor(child);
                    throw std::runtime_error("cannot start the restored core");
                }
            }
            if (installing) transaction.commit();
            const int status = waitFor(child);
            if (stopping || status != posix::kRestartExitCode) return status;
            // The child has fully exited: config, databases, libraries and
            // instance locks are closed before an update or ordinary restart.
            reexecManager(root, args, lock, false);
        }
        return 128 + stopping;
    } catch (const std::exception& ex) {
        std::cerr << "Dice!Next manager: " << ex.what() << '\n';
        if (childPid > 0) { ::kill(childPid, SIGTERM); waitFor(childPid); }
        return 1;
    }
}
