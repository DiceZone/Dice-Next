// Run from an MSVC developer prompt at the repository root:
// cl /EHsc /std:c++17 /utf-8 tools/test-subprocess-paths.cpp server/src/common/subprocess.cpp /Fe:test-subprocess-paths.exe
// .\test-subprocess-paths.exe
#include "../server/src/common/subprocess.h"
#include <clocale>
#include <iostream>
#ifdef _WIN32
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--child") {
        std::cout << "child-ok";
        return 0;
    }
    std::setlocale(LC_ALL, ".utf8"); // Match the application's console setup.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
        (L"dice-path-test-" + std::to_wstring(GetCurrentProcessId())) /
        L"\u4e2d\u6587 directory";
    fs::create_directories(dir);
    const auto child = dir / L"\u6d4b\u8bd5 child.exe";
    fs::copy_file(fs::absolute(argv[0]), child, fs::copy_options::overwrite_existing);
    const auto result = dice::proc::runPathsCancellable(
        child, {"--child"}, [] { return false; }, 4096, true, dir);
    const auto missing = dice::proc::runPaths(dir / L"missing.exe", {}, 4096, true, dir);
    std::error_code ec;
    fs::remove(child, ec);
    fs::remove(dir, ec);
    fs::remove(dir.parent_path(), ec);
    if (result.exitCode != 0 || result.output != "child-ok" || missing.exitCode == 0 ||
        missing.output.find("CreateProcessW failed, Win32 error=") == std::string::npos) {
        std::cerr << "Unicode launch or creation-error regression failed\n";
        return 1;
    }
    std::cout << "Unicode executable/working-directory and creation-error checks passed\n";
    return 0;
}
#else
int main() { std::cout << "Windows-only regression check\n"; }
#endif
