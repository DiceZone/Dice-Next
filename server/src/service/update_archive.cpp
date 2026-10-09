#include "update_archive.h"
#ifndef _WIN32
#include "../platform/posix_update.h"
#include <zlib.h>
#include <array>
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace dice::update {
namespace fs = std::filesystem;
namespace {
constexpr uint64_t kExpandedLimit = 2ULL * 1024 * 1024 * 1024;
uint64_t octal(const char* data, size_t size) {
    uint64_t number = 0;
    bool ended = false;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char ch = data[i];
        if (ch == 0 || ch == ' ') { if (number) ended = true; continue; }
        if (ended || ch < '0' || ch > '7' || number > (kExpandedLimit >> 3))
            throw std::runtime_error("invalid tar numeric field");
        number = number * 8 + ch - '0';
    }
    return number;
}
std::string field(const char* data, size_t size) {
    return std::string(data, strnlen(data, size));
}
}

bool extractPosixUpdate(const fs::path& archive, const fs::path& output,
                        std::string& error, const std::function<bool()>& cancelled) {
    gzFile input = gzopen(archive.c_str(), "rb");
    if (!input) { error = "cannot open update archive"; return false; }
    try {
        // gzopen otherwise also accepts uncompressed input.
        if (gzdirect(input)) throw std::runtime_error("POSIX update must be gzip compressed tar");
        uint64_t readBytes = 0;
        const auto read = [&](char* buffer, size_t length) {
            if (readBytes + length > kExpandedLimit) throw std::runtime_error("update archive exceeds expanded size limit");
            size_t offset = 0;
            while (offset < length) {
                if (cancelled && cancelled()) throw std::runtime_error("update operation cancelled");
                const auto count = gzread(input, buffer + offset, static_cast<unsigned>(std::min<size_t>(length - offset, 65536)));
                if (count <= 0) throw std::runtime_error("truncated or corrupt update archive");
                offset += count;
            }
            readBytes += length;
        };
        if (fs::symlink_status(output).type() != fs::file_type::directory || !fs::is_empty(output))
            throw std::runtime_error("extraction requires an empty real directory");
        std::set<std::string> names;
        std::vector<std::pair<fs::path, fs::path>> links;
        std::map<std::string, std::string> pax;
        std::string longName, longLink;
        size_t entries = 0;
        while (true) {
            std::array<char, 512> header{};
            read(header.data(), header.size());
            if (std::all_of(header.begin(), header.end(), [](char ch) { return ch == 0; })) {
                read(header.data(), header.size());
                if (!std::all_of(header.begin(), header.end(), [](char ch) { return ch == 0; }))
                    throw std::runtime_error("invalid tar end marker");
                // Read to gzip EOF to verify its trailer/CRC. Only zero padding
                // may follow the tar end; hidden additional archives are rejected.
                std::array<char, 65536> tail{};
                int count;
                while ((count = gzread(input, tail.data(), tail.size())) > 0) {
                    if (cancelled && cancelled()) throw std::runtime_error("update operation cancelled");
                    readBytes += count;
                    if (readBytes > kExpandedLimit || !std::all_of(tail.begin(), tail.begin() + count, [](char ch) { return ch == 0; }))
                        throw std::runtime_error("unexpected data after tar end");
                }
                int gzipError = Z_OK;
                gzerror(input, &gzipError);
                if (count < 0 || !gzeof(input) || (gzipError != Z_OK && gzipError != Z_STREAM_END))
                    throw std::runtime_error("corrupt gzip trailer");
                break;
            }
            if (++entries > 20000) throw std::runtime_error("too many update entries");
            uint64_t checksum = 0;
            for (size_t i = 0; i < header.size(); ++i)
                checksum += i >= 148 && i < 156 ? ' ' : static_cast<unsigned char>(header[i]);
            if (checksum != octal(header.data() + 148, 8)) throw std::runtime_error("invalid tar checksum");
            uint64_t size = octal(header.data() + 124, 12);
            const auto mode = octal(header.data() + 100, 8);
            const char type = header[156];
            const auto payload = [&](uint64_t length) {
                if (length > 65536) throw std::runtime_error("oversized tar metadata");
                std::string text(static_cast<size_t>(length), '\0');
                read(text.data(), text.size());
                return text;
            };
            const auto pad = [&](uint64_t length) {
                std::array<char, 512> padding{};
                read(padding.data(), (512 - length % 512) % 512);
            };
            if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
                const auto text = payload(size);
                pad(size);
                if (type == 'L' || type == 'K') {
                    auto name = text.substr(0, text.find('\0'));
                    if (type == 'L') longName = name; else longLink = name;
                    continue;
                }
                size_t offset = 0;
                while (offset < text.size()) {
                    const auto space = text.find(' ', offset);
                    if (space == std::string::npos) throw std::runtime_error("invalid PAX metadata");
                    const auto lengthText = text.substr(offset, space - offset);
                    if (lengthText.empty() || lengthText.find_first_not_of("0123456789") != std::string::npos)
                        throw std::runtime_error("invalid PAX length");
                    const auto length = std::stoull(lengthText);
                    if (length <= space - offset + 2 || length > text.size() - offset || text[offset + length - 1] != '\n')
                        throw std::runtime_error("invalid PAX record");
                    const auto record = text.substr(space + 1, offset + length - space - 2);
                    const auto equal = record.find('=');
                    if (equal == std::string::npos) throw std::runtime_error("invalid PAX field");
                    const auto key = record.substr(0, equal);
                    if (key == "path" || key == "linkpath" || key == "size") {
                        if (type == 'g') throw std::runtime_error("global PAX paths are unsupported");
                        pax[key] = record.substr(equal + 1);
                    }
                    if (key.rfind("GNU.sparse", 0) == 0) throw std::runtime_error("sparse update files are unsupported");
                    offset += length;
                }
                continue;
            }
            auto name = field(header.data(), 100);
            // GNU's old header reuses this region for timestamps; only POSIX
            // ustar has a pathname prefix here. GNU long paths use L records.
            const auto prefix = std::memcmp(header.data() + 257, "ustar\0", 6) == 0
                ? field(header.data() + 345, 155) : std::string();
            if (!prefix.empty()) name = prefix + '/' + name;
            if (!longName.empty()) name = longName;
            if (pax.contains("path")) name = pax.at("path");
            auto link = field(header.data() + 157, 100);
            if (!longLink.empty()) link = longLink;
            if (pax.contains("linkpath")) link = pax.at("linkpath");
            if (pax.contains("size")) {
                const auto value = pax.at("size");
                if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("invalid PAX size");
                size = std::stoull(value);
            }
            pax.clear(); longName.clear(); longLink.clear();
            while (name.rfind("./", 0) == 0) name.erase(0, 2);
            while (!name.empty() && name.back() == '/') name.pop_back();
            const fs::path relative(name);
            if (name.empty() || name.size() > 4096 || !posix::relativePathSafe(relative) ||
                name.find('\0') != std::string::npos || !names.insert(relative.generic_string()).second)
                throw std::runtime_error("unsafe or duplicate update path");
            if (size > kExpandedLimit - readBytes) throw std::runtime_error("oversized tar entry");
            posix::safeParents(output, relative, true);
            const auto destination = output / relative;
            if (type == '5') {
                if (size != 0) throw std::runtime_error("invalid tar directory");
                fs::create_directories(destination);
            } else if (type == '2') {
                if (size != 0 || link.empty() || link.find('\0') != std::string::npos)
                    throw std::runtime_error("invalid tar symlink");
                const fs::path target(link);
                const auto resolved = (relative.parent_path() / target).lexically_normal();
                if (target.is_absolute() || !posix::relativePathSafe(resolved))
                    throw std::runtime_error("update symlink escapes archive");
                links.emplace_back(relative, target); // Do not follow links while extracting.
            } else if (type == '0' || type == '\0') {
                const int fd = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
                if (fd < 0) throw std::runtime_error("cannot create extracted update file");
                try {
                    std::array<char, 65536> buffer{};
                    uint64_t remaining = size;
                    while (remaining) {
                        const auto length = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                        read(buffer.data(), length);
                        size_t offset = 0;
                        while (offset < length) {
                            const auto written = ::write(fd, buffer.data() + offset, length - offset);
                            if (written < 0 && errno == EINTR) continue;
                            if (written <= 0) throw std::runtime_error("cannot write extracted update file");
                            offset += written;
                        }
                        remaining -= length;
                    }
                    // Never preserve setuid/setgid, world-writable or owner-unreadable modes.
                    if (::fchmod(fd, (mode & 0111) ? 0755 : 0644) != 0) throw std::runtime_error("cannot set update file mode");
                    ::close(fd);
                } catch (...) { ::close(fd); throw; }
                pad(size);
            } else throw std::runtime_error("hard links and special tar files are unsupported");
        }
        if (entries == 0 || !pax.empty() || !longName.empty() || !longLink.empty())
            throw std::runtime_error("empty or incomplete update archive");
        for (const auto& [relative, target] : links) {
            posix::safeParents(output, relative, false);
            fs::create_symlink(target, output / relative);
        }
        gzclose(input);
        return true;
    } catch (const std::exception& ex) {
        gzclose(input);
        error = ex.what();
        return false;
    }
}
} // namespace dice::update
#endif
