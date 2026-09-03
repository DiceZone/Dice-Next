#include "subprocess.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dice::proc {
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kReadChunk = 8192;

void appendLimited(std::string& output, const char* data, std::size_t count,
                   std::size_t limit, bool& truncated) {
    if (truncated) return;
    if (limit == 0) {
        output.append(data, count);
        return;
    }
    const std::size_t remaining = limit > output.size() ? limit - output.size() : 0;
    output.append(data, (std::min)(count, remaining));
    if (count > remaining) truncated = true;
}

#if defined(_WIN32)

// Native narrow encoding, matching std::filesystem::path::string(), so a path
// that used to reach the command line intact still does.
std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()),
                        wide.data(), length);
    return wide;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string narrowed(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()),
                        narrowed.data(), length, nullptr, nullptr);
    return narrowed;
}

// The quoting rules CommandLineToArgvW reverses, so the child sees exactly the
// argument passed here.
std::wstring quoteArgument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }
    std::wstring quoted;
    quoted.push_back(L'"');
    for (auto it = value.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != value.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == value.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back(L'"');
    return quoted;
}

HANDLE inheritableNullDevice(SECURITY_ATTRIBUTES& attributes, DWORD access) {
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &attributes, OPEN_EXISTING, 0, nullptr);
}

Result runWide(const std::wstring& commandLine, std::size_t outputLimit,
               bool captureStderr, const fs::path& workingDirectory,
               const CancellationCheck& cancelled) {
    Result result;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) return result;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nullInput = inheritableNullDevice(attributes, GENERIC_READ);

    // Without capture, stderr keeps going wherever the parent's goes, which is
    // what the shell used to do.
    HANDLE errorTarget = writeEnd;
    HANDLE ownedError = nullptr;
    if (!captureStderr) {
        const HANDLE parentError = GetStdHandle(STD_ERROR_HANDLE);
        if (parentError && parentError != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), parentError, GetCurrentProcess(),
                            &ownedError, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            errorTarget = ownedError;
        } else {
            ownedError = inheritableNullDevice(attributes, GENERIC_WRITE);
            errorTarget = ownedError;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = writeEnd;
    startup.hStdError = errorTarget;

    const std::wstring directory =
        workingDirectory.empty() ? std::wstring() : workingDirectory.wstring();
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL started =
        CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, directory.empty() ? nullptr : directory.c_str(), &startup,
                       &process);
    CloseHandle(writeEnd);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (ownedError && ownedError != INVALID_HANDLE_VALUE) CloseHandle(ownedError);
    if (!started) {
        CloseHandle(readEnd);
        return result;
    }

    char buffer[kReadChunk];
    while (true) {
        if (!result.cancelled && cancelled && cancelled()) {
            result.cancelled = true;
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
        }

        DWORD available = 0;
        if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD count = 0;
            const DWORD wanted = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
            if (ReadFile(readEnd, buffer, wanted, &count, nullptr) && count > 0) {
                appendLimited(result.output, buffer, count, outputLimit, result.truncated);
            }
        }

        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) &&
                   available > 0) {
                DWORD count = 0;
                const DWORD wanted = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
                if (!ReadFile(readEnd, buffer, wanted, &count, nullptr) || count == 0) break;
                appendLimited(result.output, buffer, count, outputLimit, result.truncated);
            }
            break;
        }
    }
    CloseHandle(readEnd);

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.exitCode = static_cast<int>(exitCode);
    return result;
}

#endif  // _WIN32

}  // namespace

std::string systemTool(const std::string& name) {
#if defined(_WIN32)
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return name;
    const fs::path candidate = fs::path(std::wstring(directory, length)) / widen(name);
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec)) return name;  // let PATH answer instead
    return narrow(candidate.wstring());
#else
    return name;
#endif
}

Result run(const std::string& program, const std::vector<std::string>& args,
           std::size_t outputLimit, bool captureStderr, const fs::path& workingDirectory) {
    return runCancellable(program, args, {}, outputLimit, captureStderr, workingDirectory);
}

Result runCancellable(const std::string& program, const std::vector<std::string>& args,
                      CancellationCheck cancelled, std::size_t outputLimit,
                      bool captureStderr, const fs::path& workingDirectory) {
#if defined(_WIN32)
    std::wstring commandLine = quoteArgument(widen(program));
    for (const auto& argument : args) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(widen(argument));
    }
    return runWide(commandLine, outputLimit, captureStderr, workingDirectory, cancelled);
#else
    Result result;
    int channel[2];
    if (pipe(channel) != 0) return result;

    // Everything execvp needs is built before the fork: only async-signal-safe
    // calls may run in the child.
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& argument : args) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const std::string directory =
        workingDirectory.empty() ? std::string() : workingDirectory.string();

    const pid_t child = fork();
    if (child < 0) {
        close(channel[0]);
        close(channel[1]);
        return result;
    }
    if (child == 0) {
        close(channel[0]);
        dup2(channel[1], STDOUT_FILENO);
        if (captureStderr) dup2(channel[1], STDERR_FILENO);
        close(channel[1]);
        const int nullInput = open("/dev/null", O_RDONLY);
        if (nullInput >= 0) {
            dup2(nullInput, STDIN_FILENO);
            close(nullInput);
        }
        if (!directory.empty() && chdir(directory.c_str()) != 0) _exit(127);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }

    close(channel[1]);
    const int flags = fcntl(channel[0], F_GETFL, 0);
    if (flags >= 0) fcntl(channel[0], F_SETFL, flags | O_NONBLOCK);

    char buffer[kReadChunk];
    int status = 0;
    bool finished = false;
    auto cancelledAt = std::chrono::steady_clock::time_point{};
    while (!finished) {
        ssize_t count = 0;
        while ((count = read(channel[0], buffer, sizeof(buffer))) > 0) {
            appendLimited(result.output, buffer, static_cast<std::size_t>(count), outputLimit,
                          result.truncated);
        }

        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            finished = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            finished = true;
            break;
        }

        if (!result.cancelled && cancelled && cancelled()) {
            result.cancelled = true;
            cancelledAt = std::chrono::steady_clock::now();
            kill(child, SIGTERM);
        } else if (result.cancelled &&
                   std::chrono::steady_clock::now() - cancelledAt >
                       std::chrono::milliseconds(500)) {
            kill(child, SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ssize_t count = 0;
    while ((count = read(channel[0], buffer, sizeof(buffer))) > 0) {
        appendLimited(result.output, buffer, static_cast<std::size_t>(count), outputLimit,
                      result.truncated);
    }
    close(channel[0]);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
#endif
}

Result runPaths(const std::string& program, const std::vector<fs::path>& args,
                std::size_t outputLimit, bool captureStderr, const fs::path& workingDirectory) {
    return runPathsCancellable(program, args, {}, outputLimit, captureStderr,
                               workingDirectory);
}

Result runPathsCancellable(const std::string& program, const std::vector<fs::path>& args,
                           CancellationCheck cancelled, std::size_t outputLimit,
                           bool captureStderr, const fs::path& workingDirectory) {
#if defined(_WIN32)
    std::wstring commandLine = quoteArgument(widen(program));
    for (const auto& argument : args) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(argument.wstring());
    }
    return runWide(commandLine, outputLimit, captureStderr, workingDirectory, cancelled);
#else
    std::vector<std::string> narrowed;
    narrowed.reserve(args.size());
    for (const auto& argument : args) narrowed.push_back(argument.string());
    return runCancellable(program, narrowed, std::move(cancelled), outputLimit,
                          captureStderr, workingDirectory);
#endif
}

Result curlConfig(const fs::path& configFile, std::size_t outputLimit) {
    return curlConfigCancellable(configFile, {}, outputLimit);
}

Result curlConfigCancellable(const fs::path& configFile, CancellationCheck cancelled,
                             std::size_t outputLimit) {
#if defined(_WIN32)
    // Built straight from the wide path: the config file can sit under a
    // directory the system code page cannot represent.
    const std::wstring commandLine = quoteArgument(widen(systemTool("curl.exe"))) + L" -K " +
                                     quoteArgument(fs::absolute(configFile).wstring());
    return runWide(commandLine, outputLimit, false, {}, cancelled);
#else
    return runCancellable("curl", {"-K", configFile.string()}, std::move(cancelled),
                          outputLimit);
#endif
}

Result curl(const std::vector<std::string>& args, std::size_t outputLimit) {
#if defined(_WIN32)
    return run(systemTool("curl.exe"), args, outputLimit);
#else
    return run("curl", args, outputLimit);
#endif
}

}  // namespace dice::proc
