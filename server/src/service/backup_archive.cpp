#include "backup_archive.h"
#include "../storage/legacy_import_resources.h"
#include <miniz/miniz.h>
#include <miniz/miniz_zip.h>
#include <fstream>
#include <system_error>

namespace dice::backup {
namespace fs = std::filesystem;

namespace {

std::string utf8Path(const fs::path& path) { return migration_resources::utf8(path); }
fs::path fromUtf8Path(const std::string& text) { return migration_resources::fromUtf8(text); }

std::size_t writeAt(void* opaque, mz_uint64 offset, const void* data, std::size_t size) {
    auto& out = *static_cast<std::ofstream*>(opaque);
    out.seekp(static_cast<std::streamoff>(offset));
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out ? size : 0;
}

std::size_t readAt(void* opaque, mz_uint64 offset, void* data, std::size_t size) {
    auto& in = *static_cast<std::ifstream*>(opaque);
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount());
}

dice::proc::Result fail(const std::string& message) {
    dice::proc::Result result;
    result.exitCode = 1;
    result.output = message;
    return result;
}

dice::proc::Result ok(std::string output = {}) {
    dice::proc::Result result;
    result.exitCode = 0;
    result.output = std::move(output);
    return result;
}

void appendLimited(std::string& output, const std::string& line, std::size_t limit, bool& truncated) {
    if (truncated) return;
    const std::string chunk = line + "\n";
    if (limit == 0) {
        output += chunk;
        return;
    }
    const std::size_t remaining = limit > output.size() ? limit - output.size() : 0;
    output.append(chunk, 0, remaining);
    if (chunk.size() > remaining) truncated = true;
}

std::string normalizeName(std::string name) {
    for (char& c : name) if (c == '\\') c = '/';
    while (name.size() >= 2 && name[0] == '.' && name[1] == '/') name.erase(0, 2);
    return name;
}

bool escapedDestination(const fs::path& destination, const fs::path& target) {
    const auto relative = target.lexically_normal().lexically_relative(destination.lexically_normal());
    if (relative.empty() || relative.is_absolute()) return true;
    for (const auto& part : relative) {
        if (part == "..") return true;
    }
    return false;
}

struct ZipReader {
    std::ifstream input;
    mz_zip_archive zip{};
    bool open = false;

    explicit ZipReader(const fs::path& archive) : input(archive, std::ios::binary) {}
    ~ZipReader() { if (open) mz_zip_reader_end(&zip); }

    dice::proc::Result init() {
        if (!input) return fail("无法打开压缩包");
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size < 0) return fail("无法读取压缩包大小");
        input.seekg(0);
        zip.m_pRead = readAt;
        zip.m_pIO_opaque = &input;
        if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(size), 0)) {
            return fail(mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        }
        open = true;
        return ok();
    }

    std::string filename(mz_uint index) {
        const mz_uint length = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
        if (length == 0) return {};
        std::string name(length, '\0');
        mz_zip_reader_get_filename(&zip, index, name.data(), length);
        if (!name.empty() && name.back() == '\0') name.pop_back();
        return normalizeName(std::move(name));
    }
};

bool addFile(mz_zip_archive& zip, const std::string& name, const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    return mz_zip_writer_add_read_buf_callback(&zip, name.c_str(), readAt, &in, size,
                                               nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION,
                                               nullptr, 0, nullptr, 0) != 0;
}

bool addDirectory(mz_zip_archive& zip, std::string name) {
    if (name.empty()) return true;
    if (name.back() != '/') name += '/';
    return mz_zip_writer_add_mem(&zip, name.c_str(), "", 0, MZ_DEFAULT_COMPRESSION) != 0;
}

bool addTree(mz_zip_archive& zip, const fs::path& stage, const fs::path& member, std::string& error) {
    std::error_code ec;
    const fs::path root = stage / member;
    if (!fs::exists(root, ec)) return true;
    if (migration_resources::isLink(root)) {
        error = "备份不包含符号链接";
        return false;
    }
    const std::string name = utf8Path(member);
    if (fs::is_regular_file(root, ec)) {
        if (!addFile(zip, name, root)) {
            error = "无法压缩 " + utf8Path(root);
            return false;
        }
        return true;
    }
    if (!fs::is_directory(root, ec)) {
        error = "无法读取 " + utf8Path(root);
        return false;
    }
    if (!addDirectory(zip, name)) {
        error = "无法写入目录 " + name;
        return false;
    }
    fs::recursive_directory_iterator it(root, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { error = "遍历 " + utf8Path(root) + " 失败: " + ec.message(); return false; }
        if (migration_resources::isLink(it->path())) {
            error = "备份不包含符号链接";
            return false;
        }
        const auto rel = fs::relative(it->path(), stage, ec);
        if (ec) { error = "解析压缩路径失败: " + ec.message(); return false; }
        const std::string name = utf8Path(rel);
        if (it->is_directory(ec)) {
            if (!addDirectory(zip, name)) { error = "无法写入目录 " + name; return false; }
        } else if (it->is_regular_file(ec)) {
            if (!addFile(zip, name, it->path())) { error = "无法压缩 " + name; return false; }
        } else if (ec) {
            error = "读取 " + utf8Path(it->path()) + " 失败: " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace

dice::proc::Result zipList(const fs::path& archive, std::size_t outputLimit) {
    ZipReader reader(archive);
    const auto opened = reader.init();
    if (opened.exitCode != 0) return opened;
    std::string output;
    bool truncated = false;
    const mz_uint count = mz_zip_reader_get_num_files(&reader.zip);
    for (mz_uint i = 0; i < count; ++i) {
        appendLimited(output, reader.filename(i), outputLimit, truncated);
    }
    auto result = ok(std::move(output));
    result.truncated = truncated;
    return result;
}

dice::proc::Result zipCreate(const fs::path& stage, const fs::path& archive,
                             const std::vector<fs::path>& members) {
    std::error_code ec;
    const fs::path target = fs::absolute(archive, ec);
    if (ec || target.empty()) return fail("无法解析备份压缩包路径");
    fs::create_directories(target.parent_path(), ec);
    if (ec) return fail("无法创建备份目录: " + ec.message());
    const fs::path temporary = fs::path(target.native() + fs::path(".tmp").native());
    fs::remove(temporary, ec);
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return fail("无法创建备份压缩包");
    mz_zip_archive zip{};
    zip.m_pWrite = writeAt;
    zip.m_pIO_opaque = &out;
    if (!mz_zip_writer_init_v2(&zip, 0, 0)) {
        out.close();
        fs::remove(temporary, ec);
        return fail("无法初始化压缩包写入");
    }
    std::string error;
    bool okWrite = true;
    for (const auto& member : members) {
        if (!addTree(zip, stage, member, error)) { okWrite = false; break; }
    }
    if (okWrite && !mz_zip_writer_finalize_archive(&zip)) {
        error = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
        okWrite = false;
    }
    mz_zip_writer_end(&zip);
    if (okWrite) out.flush();
    const bool saved = okWrite && static_cast<bool>(out);
    out.close();
    if (!saved) {
        fs::remove(temporary, ec);
        return fail(error.empty() ? "无法创建备份压缩包" : error);
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return fail("无法保存备份压缩包: " + ec.message());
    }
    return ok();
}

dice::proc::Result zipExtract(const fs::path& archive, const fs::path& destination) {
    ZipReader reader(archive);
    const auto opened = reader.init();
    if (opened.exitCode != 0) return opened;
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) return fail("无法创建解压目录: " + ec.message());
    const fs::path dest = fs::absolute(destination, ec);
    if (ec) return fail("无法解析解压目录");
    const mz_uint count = mz_zip_reader_get_num_files(&reader.zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&reader.zip, i, &stat)) {
            return fail("压缩包目录损坏");
        }
        const std::string name = reader.filename(i);
        if (name.empty() || name.front() == '/') return fail("压缩包含越界路径");
        const fs::path target = dest / fromUtf8Path(name);
        if (escapedDestination(dest, target)) return fail("压缩包含越界路径");
        if (stat.m_is_directory || (!name.empty() && name.back() == '/')) {
            fs::create_directories(target, ec);
            if (ec) return fail("无法创建解压目录: " + ec.message());
            continue;
        }
        if (stat.m_is_encrypted || !stat.m_is_supported) {
            return fail("压缩包含加密或不支持的条目");
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) return fail("无法创建解压目录: " + ec.message());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return fail("无法写入 " + name);
        auto writeFile = [](void* opaque, mz_uint64 offset, const void* data, std::size_t size) {
            auto& file = *static_cast<std::ofstream*>(opaque);
            file.seekp(static_cast<std::streamoff>(offset));
            file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            return file ? size : 0;
        };
        if (!mz_zip_reader_extract_to_callback(&reader.zip, i, writeFile, &out, 0) || !out) {
            out.close();
            fs::remove(target, ec);
            return fail("解压失败: " + name);
        }
    }
    return ok();
}

dice::proc::Result zipRead(const fs::path& archive, const std::string& innerName,
                           std::size_t outputLimit) {
    ZipReader reader(archive);
    const auto opened = reader.init();
    if (opened.exitCode != 0) return opened;
    const int index = mz_zip_reader_locate_file(&reader.zip, innerName.c_str(), nullptr, 0);
    if (index < 0) return fail("压缩包缺少 " + innerName);
    size_t size = 0;
    void* bytes = mz_zip_reader_extract_to_heap(&reader.zip, static_cast<mz_uint>(index), &size, 0);
    if (!bytes) return fail("无法读取 " + innerName);
    std::string output(static_cast<const char*>(bytes), size);
    mz_free(bytes);
    dice::proc::Result result = ok();
    if (outputLimit != 0 && output.size() > outputLimit) {
        output.resize(outputLimit);
        result.truncated = true;
    }
    result.output = std::move(output);
    return result;
}

} // namespace dice::backup
