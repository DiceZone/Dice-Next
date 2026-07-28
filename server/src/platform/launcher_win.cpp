// Windows distribution manager.  This executable intentionally links only to
// Win32 libraries, so it can prepare PATH before dice-next-core.exe loads DLLs.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

constexpr DWORD kManagedRestartExitCode = 42;

std::wstring executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return fs::path(std::wstring(buffer.data(), length)).parent_path().wstring();
}

std::wstring quoteArg(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashCount = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashCount; continue; }
        if (ch == L'\"') out.append(slashCount * 2 + 1, L'\\');
        else out.append(slashCount, L'\\');
        out += ch;
        slashCount = 0;
    }
    out.append(slashCount * 2, L'\\');
    return out + L"\"";
}

std::wstring pathValue(const std::wstring& name) {
    DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    GetEnvironmentVariableW(name.c_str(), value.data(), needed);
    value.resize(needed - 1);
    return value;
}

bool noRestartRequested() {
    std::wstring value = pathValue(L"DICENEXT_UPDATE_RESTART");
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    return value == L"NO";
}

std::wstring wideError(const std::error_code& error) {
    const std::string message = error.message();
    return std::wstring(message.begin(), message.end());
}

bool moveTree(const fs::path& source, const fs::path& destination, const fs::path& rollback, std::wstring& error) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return true;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) { error = wideError(ec); return false; }
    if (fs::exists(destination, ec)) {
        fs::create_directories(rollback.parent_path(), ec);
        if (ec) { error = wideError(ec); return false; }
        fs::rename(destination, rollback, ec);
        if (ec) { error = wideError(ec); return false; }
    }
    fs::rename(source, destination, ec);
    if (!ec) return true;
    ec.clear();
    if (fs::is_directory(source, ec)) {
        fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove_all(source, ec);
    } else {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(source, ec);
    }
    if (ec) { error = wideError(ec); return false; }
    return true;
}

std::wstring timestamp() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t out[32]{};
    swprintf_s(out, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return out;
}

bool applyPendingRestore(const fs::path& root) {
    const fs::path stage = root / L"restore-pending";
    std::error_code ec;
    if (!fs::is_regular_file(stage / L"manifest.json", ec)) return true;
    const fs::path rollback = root / L"restore-rollbacks" / timestamp();
    std::wstring error;
    if (!moveTree(stage / L"data", root / L"data", rollback / L"data", error)) {
        std::wcerr << L"Dice!Next restore failed: " << error << L"\n";
        return false;
    }
    const fs::path stagedConfig = stage / L"config" / L"default_config.json";
    if (fs::exists(stagedConfig, ec) && !moveTree(stagedConfig, root / L"config" / L"default_config.json", rollback / L"config" / L"default_config.json", error)) {
        std::wcerr << L"Dice!Next restore failed: " << error << L"\n";
        return false;
    }
    fs::remove_all(stage, ec);
    std::wcout << L"Dice!Next: pending restore applied. Rollback: " << rollback.wstring() << L"\n";
    return true;
}

bool applyUpdate(const fs::path& root, const fs::path& stage) {
    std::error_code ec;
    if (!fs::is_regular_file(stage / L"app" / L"dice-next-core.exe", ec)) {
        std::wcerr << L"Dice!Next update failed: staged app/dice-next-core.exe is missing.\n";
        return false;
    }
    const fs::path rollback = root / L"updates" / L"rollbacks" / timestamp();
    std::wstring error;
    for (const wchar_t* item : {L"app", L"lib", L"i18n", L"web", L"docs"}) {
        if (!moveTree(stage / item, root / item, rollback / item, error)) {
            std::wcerr << L"Dice!Next update failed while replacing " << item << L": " << error << L"\n";
            return false;
        }
    }
    for (const wchar_t* file : {L"dice-next.exe", L"使用说明.txt"}) {
        if (!moveTree(stage / file, root / file, rollback / file, error)) {
            std::wcerr << L"Dice!Next update failed while replacing " << file << L": " << error << L"\n";
            return false;
        }
    }
    fs::remove_all(stage, ec);
    std::wcout << L"Dice!Next update applied. Rollback: " << rollback.wstring() << L"\n";
    return true;
}

bool startMaintenance(const fs::path& root, const fs::path& stage) {
    std::error_code ec;
    const fs::path updates = root / L"updates";
    fs::create_directories(updates, ec);
    const fs::path helper = updates / (L"dice-next-maintenance-" + timestamp() + L".exe");
    fs::copy_file(root / L"dice-next.exe", helper, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::wcerr << L"Dice!Next could not prepare update maintenance helper: " << wideError(ec) << L"\n";
        return false;
    }
    std::wstring commandLine = quoteArg(helper.wstring()) + L" --maintenance " + quoteArg(root.wstring()) +
        L" " + quoteArg(stage.wstring()) + L" " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end()); mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, root.c_str(), &startup, &process)) {
        std::wcerr << L"Dice!Next could not start update maintenance helper (Win32 error " << GetLastError() << L").\n";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void waitForProcessExit(DWORD processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) return;
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
}

int launchUpdatedManager(const fs::path& root) {
    const fs::path manager = root / L"dice-next.exe";
    std::wstring commandLine = quoteArg(manager.wstring());
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end()); mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(manager.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, root.c_str(), &startup, &process)) {
        std::wcerr << L"Dice!Next could not start updated manager (Win32 error " << GetLastError() << L").\n";
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

int runMaintenance(const fs::path& root, const fs::path& stage, DWORD parentProcessId) {
    waitForProcessExit(parentProcessId);
    if (!applyUpdate(root, stage)) return 1;
    if (noRestartRequested()) {
        std::wcout << L"Dice!Next update finished; DICENEXT_UPDATE_RESTART=NO prevents core restart.\n";
        return 0;
    }
    return launchUpdatedManager(root);
}

DWORD launchCore(const fs::path& root, const std::vector<std::wstring>& args) {
    const fs::path core = root / L"app" / L"dice-next-core.exe";
    if (!fs::is_regular_file(core)) {
        std::wcerr << L"Dice!Next core is missing: " << core.wstring() << L"\n";
        return ERROR_FILE_NOT_FOUND;
    }
    const std::wstring originalPath = pathValue(L"PATH");
    SetEnvironmentVariableW(L"PATH", (root / L"lib").wstring().append(L";").append(originalPath).c_str());
    SetEnvironmentVariableW(L"DICENEXT_MANAGED", L"1");
    SetCurrentDirectoryW(root.c_str());

    std::wstring commandLine = quoteArg(core.wstring());
    for (const auto& arg : args) commandLine += L" " + quoteArg(arg);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end()); mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(core.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, root.c_str(), &startup, &process)) {
        const DWORD code = GetLastError();
        std::wcerr << L"Dice!Next could not start core (Win32 error " << code << L").\n";
        return code;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1; GetExitCodeProcess(process.hProcess, &result); CloseHandle(process.hProcess);
    return result;
}

int runManager(const fs::path& root, const std::vector<std::wstring>& args) {
    if (!applyPendingRestore(root)) return 1;
    for (;;) {
        const DWORD result = launchCore(root, args);
        if (result != kManagedRestartExitCode) return static_cast<int>(result);
        const fs::path pendingUpdate = root / L"updates" / L"pending";
        const bool hasPendingUpdate = fs::is_directory(pendingUpdate);
        if (hasPendingUpdate) return startMaintenance(root, pendingUpdate) ? 0 : 1;
        if (!applyPendingRestore(root)) return 1;
    }
}

} // namespace

int wmain() {
    int argc = 0;
    LPWSTR* rawArgs = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!rawArgs) return 1;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(rawArgs[i]);
    LocalFree(rawArgs);

    const fs::path root = executableDirectory();
    if (root.empty()) return 1;
    if (args.size() == 4 && args[0] == L"--maintenance") {
        return runMaintenance(fs::path(args[1]), fs::path(args[2]), static_cast<DWORD>(std::stoul(args[3])));
    }
    if (args.size() == 2 && args[0] == L"--apply-update") {
        return startMaintenance(root, fs::path(args[1])) ? 0 : 1;
    }
    return runManager(root, args);
}
