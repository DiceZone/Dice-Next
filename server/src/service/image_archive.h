#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace dice::image_resources {
struct ZipEntry { std::string name; std::uint64_t size=0; unsigned index=0; bool directory=false; };
// A bounded ZIP reader. Names, links, sizes, encryption and duplicate paths are
// checked before any extraction. Archives never execute embedded files.
class ZipReader {
public:
    explicit ZipReader(std::string_view bytes);
    ~ZipReader();
    const std::vector<ZipEntry>& entries() const;
    std::string read(const ZipEntry&, std::uint64_t limit=128ULL*1024*1024);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
struct ZipSource { std::string name; std::filesystem::path file; std::string bytes; };
void writeZip(const std::filesystem::path& output, const std::vector<ZipSource>& files);
std::string safeZipName(std::string name);
struct TemporaryDirectory {
    std::filesystem::path path;
    explicit TemporaryDirectory(const std::filesystem::path& parent);
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&)=delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&)=delete;
};
class Service;
struct ImageArchiveDownload { std::shared_ptr<TemporaryDirectory> temporary; std::filesystem::path file; };
ImageArchiveDownload exportImages(Service&,const std::vector<std::string>& paths);
nlohmann::json importImages(Service&,std::string_view bytes,const std::string& directory,bool overwrite=false);
}
