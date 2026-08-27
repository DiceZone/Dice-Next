// Windows distribution manager.  This executable intentionally links only to
// Win32 libraries, so it can prepare PATH before dice-next-core.exe loads DLLs.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

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

std::string readUpdateMetadata(const fs::path& stage) {
    std::ifstream input(stage / L"update.json", std::ios::binary);
    if (!input) return "{}";
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.empty() || text.size() > 64 * 1024 ||
        text.find('{') == std::string::npos || text.rfind('}') == std::string::npos) {
        return "{}";
    }
    return text;
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch >= 0x20) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

void writeUpdateResult(const fs::path& root, const std::string& metadata,
                       bool success, const std::string& message) {
    const fs::path updates = root / L"updates";
    const fs::path result = updates / L"last-result.json";
    const fs::path temporary = updates / L"last-result.tmp";
    std::error_code ec;
    fs::create_directories(updates, ec);
    if (ec) return;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << "{\"schema\":1,\"success\":" << (success ? "true" : "false")
               << ",\"metadata\":" << (metadata.empty() ? "{}" : metadata)
               << ",\"message\":\"" << jsonEscape(message) << "\"}\n";
        if (!output) return;
    }
    fs::remove(result, ec);
    ec.clear();
    fs::rename(temporary, result, ec);
    if (ec) fs::remove(temporary, ec);
}

bool applyPendingRestore(const fs::path& root) {
    const fs::path stage = root / L"restore-pending";
    std::error_code ec;
    if (!fs::is_regular_file(stage / L"manifest.json", ec)) return true;
    // Partial backups are applied by core's JSON-aware restore service.  The
    // manager intentionally leaves them staged so it never replaces the whole
    // data directory for a plugin/resource-only restore.
    {
        std::ifstream manifest(stage / L"manifest.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
        if (text.find("\"version\": 2") != std::string::npos && text.find("\"all\": false") != std::string::npos) return true;
    }
    const fs::path rollback = root / L"restore-rollbacks" / timestamp();
    std::wstring error;
    if (!moveTree(stage / L"data", root / L"data", rollback / L"data", error)) {
        std::wcerr << L"Dice!Next restore failed: " << error << L"\n";
        return false;
    }
    const fs::path stagedConfig = stage / L"config";
    if (fs::exists(stagedConfig, ec) && !moveTree(stagedConfig, root / L"config", rollback / L"config", error)) {
        std::wcerr << L"Dice!Next restore failed: " << error << L"\n";
        return false;
    }
    fs::remove_all(stage, ec);
    std::wcout << L"Dice!Next: pending restore applied. Rollback: " << rollback.wstring() << L"\n";
    return true;
}

struct UpdateMove {
    fs::path destination;
    fs::path backup;
    bool hadExisting = false;
};

bool replaceUpdatePath(const fs::path& source, const fs::path& destination,
                       const fs::path& backup, std::vector<UpdateMove>& moves,
                       std::wstring& error) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return true;
    UpdateMove record{destination, backup, fs::exists(destination, ec)};
    moves.push_back(record);
    return moveTree(source, destination, backup, error);
}

bool rollbackUpdateMoves(std::vector<UpdateMove>& moves) {
    bool ok = true;
    std::error_code ec;
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        fs::remove_all(it->destination, ec);
        ec.clear();
        if (!it->hadExisting || !fs::exists(it->backup, ec)) continue;
        std::wstring error;
        const fs::path conflict = fs::path(it->backup.wstring() + L".restore-conflict");
        if (!moveTree(it->backup, it->destination, conflict, error)) {
            std::wcerr << L"Dice!Next rollback failed for " << it->destination.wstring()
                       << L": " << error << L"\n";
            ok = false;
        }
    }
    return ok;
}

bool applyUpdate(const fs::path& root, const fs::path& stage) {
    std::error_code ec;
    if (!fs::is_regular_file(stage / L"app" / L"dice-next-core.exe", ec) ||
        !fs::is_regular_file(stage / L"dice-next.exe", ec) ||
        !fs::is_regular_file(stage / L"web" / L"dist" / L"index.html", ec)) {
        std::wcerr << L"Dice!Next update failed: staged package is incomplete.\n";
        return false;
    }

    const fs::path rollback = root / L"updates" / L"rollbacks" / timestamp();
    std::vector<UpdateMove> moves;
    std::wstring error;
    auto replace = [&](const fs::path& source, const fs::path& destination,
                       const fs::path& backup) {
        return replaceUpdatePath(source, destination, backup, moves, error);
    };
    auto fail = [&](const std::wstring& item) {
        std::wcerr << L"Dice!Next update failed while replacing " << item << L": "
                   << error << L"\n";
        if (rollbackUpdateMoves(moves))
            std::wcerr << L"Dice!Next restored the previous version automatically.\n";
        else
            std::wcerr << L"Dice!Next rollback was incomplete; keep " << rollback.wstring()
                       << L" for manual recovery.\n";
        return false;
    };

    for (const wchar_t* item : {
            L"app", L"lib", L"i18n", L"web", L"docs", L"decks", L"card-templates"}) {
        if (!replace(stage / item, root / item, rollback / item)) return fail(item);
    }
    for (const wchar_t* file : {L"dice-next.exe", L"update-mirrors.json",
                                L"使用说明.txt"}) {
        if (!replace(stage / file, root / file, rollback / file)) return fail(file);
    }

    // Bundled help replaces the previous bundled set. User decks and card
    // templates live elsewhere and are not touched by these paths.
    if (!replace(stage / L"data" / L"helpdoc", root / L"data" / L"helpdoc",
                 rollback / L"data" / L"helpdoc")) {
        return fail(L"data/helpdoc");
    }

    // Built-in example plugins are overlaid file-by-file so third-party
    // SealDice plugins in data/plugins remain intact.
    const fs::path pluginStage = stage / L"data" / L"plugins";
    if (fs::is_directory(pluginStage, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(pluginStage, ec)) {
            if (ec) { error = wideError(ec); return fail(L"data/plugins"); }
            if (!entry.is_regular_file(ec)) continue;
            const fs::path relative = fs::relative(entry.path(), pluginStage, ec);
            if (ec) { error = wideError(ec); return fail(L"data/plugins"); }
            if (!replace(entry.path(), root / L"data" / L"plugins" / relative,
                         rollback / L"data" / L"plugins" / relative)) {
                return fail((fs::path(L"data/plugins") / relative).wstring());
            }
        }
    }

    fs::remove_all(stage, ec);
    std::wcout << L"Dice!Next update applied. Rollback: " << rollback.wstring() << L"\n";
    return true;
}

// Waits until a process is really gone.  The port and the single-instance lock
// are released only then, so a restart has to hold off until it happens.
void waitForProcessExit(DWORD processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) return;
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
}

// Starts the core and returns immediately.  This program is a launcher, not a
// supervisor: it prepares the directory, hands off and exits, so Dice!Next
// leaves exactly one process resident.  Every dependency now sits next to the
// core inside app\, the first directory Windows searches for an executable's
// imports, so there is no DLL search path to arrange here either.
bool launchCore(const fs::path& root, const std::vector<std::wstring>& args) {
    const fs::path core = root / L"app" / L"dice-next-core.exe";
    if (!fs::is_regular_file(core)) {
        std::wcerr << L"Dice!Next core is missing: " << core.wstring() << L"\n";
        return false;
    }
    // The core reads this to decide whether a staged update can be applied.
    SetEnvironmentVariableW(L"DICENEXT_MANAGED", L"1");
    SetCurrentDirectoryW(root.c_str());

    std::wstring commandLine = quoteArg(core.wstring());
    for (const auto& arg : args) commandLine += L" " + quoteArg(arg);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(core.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        root.c_str(), &startup, &process)) {
        std::wcerr << L"Dice!Next could not start core (Win32 error " << GetLastError() << L").\n";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// Applies a staged update if one is waiting.  It runs here, in the launcher,
// with no core alive and nothing to co-ordinate with, so every file can be
// replaced — dice-next.exe included: Windows refuses to overwrite a running
// image but allows renaming it, which is what moveTree does.
// Returns whether an update was present.
bool applyPendingUpdate(const fs::path& root) {
    const fs::path stage = root / L"updates" / L"pending";
    std::error_code ec;
    if (!fs::is_directory(stage, ec)) return false;
    const std::string metadata = readUpdateMetadata(stage);
    const bool applied = applyUpdate(root, stage);
    writeUpdateResult(root, metadata, applied,
        applied ? "更新文件已成功应用" : "更新文件应用失败，已尝试回滚到原版本");
    return true;
}

int runLauncher(const fs::path& root, const std::vector<std::wstring>& args, DWORD waitFor) {
    if (waitFor != 0) waitForProcessExit(waitFor);
    if (!applyPendingRestore(root)) return 1;
    const bool updated = applyPendingUpdate(root);
    if (updated && noRestartRequested()) {
        std::wcout << L"Dice!Next update finished; DICENEXT_UPDATE_RESTART=NO prevents core start.\n";
        return 0;
    }
    return launchCore(root, args) ? 0 : 1;
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

    // "--after <pid>": the core is shutting down for a restart, or to let a
    // staged update be applied.  Anything else is forwarded to the core.
    DWORD waitFor = 0;
    if (args.size() >= 2 && args[0] == L"--after") {
        try {
            waitFor = static_cast<DWORD>(std::stoul(args[1]));
        } catch (...) {
            waitFor = 0;
        }
        args.erase(args.begin(), args.begin() + 2);
    }
    return runLauncher(root, args, waitFor);
}