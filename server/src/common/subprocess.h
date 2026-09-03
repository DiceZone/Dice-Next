#pragma once

// Child-process helpers that never go through a shell.
//
// The project shells out to curl, tar and esbuild in a dozen places.  On
// Windows every one of those went through _popen or std::system, which start
// cmd.exe; an unsigned application whose process tree is
// app -> cmd.exe -> curl.exe -> downloaded file is the living-off-the-land
// pattern behaviour-based scanners alert on.  Passing an explicit argument
// vector also removes the shell-quoting hazards that came with pasting URLs and
// paths into a command string.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dice::proc {

struct Result {
    int exitCode = -1;
    std::string output;
    bool truncated = false;
    bool cancelled = false;

    bool ok() const { return exitCode == 0; }
};

using CancellationCheck = std::function<bool()>;

/// Runs @p program with @p args directly.  stdout is captured, and stderr too
/// when @p captureStderr.  @p outputLimit of 0 means unlimited; output past the
/// limit is dropped and @p truncated is set -- what was read before the cap is
/// kept, so a caller that must not act on a partial result checks the flag.  An
/// empty @p workingDirectory inherits the caller's.
///
/// Narrow strings are interpreted in the platform's native encoding, the same
/// one std::filesystem::path::string() produces, so moving a call site off the
/// shell cannot change how an existing path is read.
Result run(const std::string& program,
           const std::vector<std::string>& args,
           std::size_t outputLimit = 0,
           bool captureStderr = false,
           const std::filesystem::path& workingDirectory = {});

/// Cancellable form of run(). When @p cancelled returns true, the child is
/// terminated and reaped before this function returns.
Result runCancellable(const std::string& program,
                      const std::vector<std::string>& args,
                      CancellationCheck cancelled,
                      std::size_t outputLimit = 0,
                      bool captureStderr = false,
                      const std::filesystem::path& workingDirectory = {});

/// Same as run(), with every argument given as a path.  On Windows they reach
/// the child as wide strings, so an install directory the system code page
/// cannot represent still works.  Plain flags may be passed too.
Result runPaths(const std::string& program,
                const std::vector<std::filesystem::path>& args,
                std::size_t outputLimit = 0,
                bool captureStderr = false,
                const std::filesystem::path& workingDirectory = {});

Result runPathsCancellable(const std::string& program,
                           const std::vector<std::filesystem::path>& args,
                           CancellationCheck cancelled,
                           std::size_t outputLimit = 0,
                           bool captureStderr = false,
                           const std::filesystem::path& workingDirectory = {});

/// Absolute path of a tool that ships with the operating system.  On Windows it
/// resolves inside the system directory, so a file dropped next to the
/// application cannot answer for curl.exe or tar.exe -- the manager puts the
/// package lib directory on PATH before starting the core.  Elsewhere the plain
/// name is returned and the normal PATH lookup applies.
std::string systemTool(const std::string& name);

/// curl reading its options from a config file: the dominant pattern here,
/// because it keeps tokens and URLs out of the process list.  The path is
/// passed to the child as a wide string, so non-ASCII install directories work
/// regardless of the system code page.
Result curlConfig(const std::filesystem::path& configFile, std::size_t outputLimit = 0);

Result curlConfigCancellable(const std::filesystem::path& configFile,
                             CancellationCheck cancelled,
                             std::size_t outputLimit = 0);

/// curl with an explicit argument vector, for the calls that do not use a
/// config file.  Arguments are passed verbatim -- no quoting required.
Result curl(const std::vector<std::string>& args, std::size_t outputLimit = 0);

} // namespace dice::proc
