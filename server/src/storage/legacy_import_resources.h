#pragma once

// Filesystem helpers shared by the legacy resource/session/log importer. They
// never follow a symlink/reparse point or accept a path outside the selected
// legacy root. A migration is not a reason to read arbitrary machine files.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dice::migration_resources {
namespace fs = std::filesystem;
inline std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}
inline fs::path fromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}
inline std::string hash(const std::string& text) {
    // A reproducible source identity, not a password or cryptographic token.
    std::uint64_t value = 14695981039346656037ULL;
    for (unsigned char c : text) { value ^= c; value *= 1099511628211ULL; }
    std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}
inline bool isLink(const fs::path& path) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(path, ec))) return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
inline bool within(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const auto absoluteRoot = fs::absolute(root, ec).lexically_normal();
    if (ec) return false;
    const auto base = absoluteRoot;
    const auto absolutePath = fs::absolute(path, ec).lexically_normal();
    if (ec) return false;
    // No filesystem resolution is needed for a not-yet-created destination.
    // safePath below rejects links in every ORIGINAL ancestor before use.
    const auto absolute = absolutePath;
    auto left = base.begin(), right = absolute.begin();
    for (; left != base.end(); ++left, ++right) {
        if (left->empty()) break;
        if (right == absolute.end()) return false;
#ifdef _WIN32
        std::wstring a = left->native(), b = right->native();
        std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return std::towlower(c); });
        std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return std::towlower(c); });
        if (a != b) return false;
#else
        if (*left != *right) return false;
#endif
    }
    return true;
}
inline bool safePath(const fs::path& root, const fs::path& path) {
    if (!within(root, path)) return false;
    std::error_code ec;
    const auto base = fs::absolute(root, ec).lexically_normal();
    if (ec) return false;
    const auto target = fs::absolute(path, ec);
    if (ec) return false;
    // "C:" is drive-relative and may resolve to an unrelated per-drive
    // working directory. Start at the absolute root before checking ancestors.
    fs::path cursor = target.root_path();
    if (isLink(cursor)) return false;
    for (const auto& part : target.relative_path()) {
        cursor /= part;
        if (isLink(cursor)) return false;
    }
    return true;
}
inline std::string sourceIdentity(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    auto absolute = fs::weakly_canonical(root, ec);
    if (ec) throw std::runtime_error("Cannot resolve the legacy source directory");
    std::string base = utf8(absolute);
#ifdef _WIN32
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return base + "\n" + utf8(path.lexically_relative(root));
}
using FileVisitor = std::function<void(const fs::path&)>;
using ProblemVisitor = std::function<void(const fs::path&, const std::string&)>;
inline void walk(const fs::path& selectedRoot, const fs::path& directory,
                 const FileVisitor& visit, const ProblemVisitor& problem) {
    std::error_code ec;
    if (!fs::exists(directory, ec)) { if (ec) problem(directory, ec.message()); return; }
    if (!safePath(selectedRoot, directory) || !fs::is_directory(directory, ec)) {
        problem(directory, "unsafe source directory (link/reparse point or outside the selected root)"); return;
    }
    fs::recursive_directory_iterator it(directory, fs::directory_options::none, ec), end;
    if (ec) { problem(directory, ec.message()); return; }
    std::size_t count = 0;
    while (it != end) {
        const fs::path path = it->path();
        if (++count > 20000) { problem(directory, "resource scan exceeds 20000 entries; split the import"); break; }
        if (!safePath(selectedRoot, path)) {
            it.disable_recursion_pending(); problem(path, "symlink/reparse point or outside source root; not followed");
        } else if (it->is_regular_file(ec)) {
            const auto size = fs::file_size(path, ec);
            if (ec) problem(path, ec.message());
            else if (size > 128ULL * 1024 * 1024) problem(path, "file exceeds the 128 MiB migration limit");
            else visit(path);
        } else if (ec) { problem(path, ec.message()); ec.clear(); }
        it.increment(ec);
        if (ec) { problem(path, ec.message()); break; }
    }
}
inline std::string copyFile(const fs::path& source, const fs::path& target, bool overwrite) {
    std::error_code ec;
    const auto destinationRoot = fs::current_path(ec);
    if (ec || !safePath(destinationRoot, target)) return "unsafe destination path (link/reparse point or outside runtime directory)";
    fs::create_directories(target.parent_path(), ec);
    if (ec) return ec.message();
    const bool copied = fs::copy_file(source, target, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    return copied && !ec ? std::string() : ec ? ec.message() : "file was not copied";
}
inline std::string writeFile(const fs::path& target, const std::string& bytes) {
    std::error_code ec;
    if (!safePath(fs::current_path(), target)) return "unsafe destination path";
    fs::create_directories(target.parent_path(), ec);
    if (ec) return ec.message();
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot create destination file";
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); out.close();
    return out ? std::string() : "cannot write destination file";
}
inline std::string lowerExtension(const fs::path& path) {
    std::string extension = utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}
inline bool mediaFile(const fs::path& path) {
    const auto ext = lowerExtension(path);
    for (const char* allowed : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".avif", ".ico", ".mp3", ".wav", ".ogg", ".flac", ".m4a", ".mp4", ".webm"})
        if (ext == allowed) return true;
    return false;
}
} // namespace dice::migration_resources
