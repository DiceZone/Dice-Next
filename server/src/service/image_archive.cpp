#include "image_archive.h"
#include "image_resource_service.h"
#include "../storage/legacy_dice2.h"
#include "../storage/legacy_import_resources.h"
#include <miniz/miniz.h>
#include <miniz/miniz_zip.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <set>

namespace dice::image_resources {
namespace fs=std::filesystem;
namespace {
constexpr std::uint64_t totalLimit=512ULL*1024*1024, fileLimit=128ULL*1024*1024;
bool validUtf8(const std::string& text){try{(void)nlohmann::json(text).dump();return true;}catch(...){return false;}}
std::string folded(std::string name){for(auto& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return name;}
std::size_t write(void* opaque,mz_uint64 offset,const void* data,std::size_t size) {
    if(offset>totalLimit || size>totalLimit-offset)return 0;
    auto& out=*static_cast<std::ofstream*>(opaque);out.seekp(static_cast<std::streamoff>(offset));out.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));return out?size:0;
}
std::size_t read(void* opaque,mz_uint64 offset,void* data,std::size_t size) {
    auto& in=*static_cast<std::ifstream*>(opaque);in.clear();in.seekg(static_cast<std::streamoff>(offset));in.read(static_cast<char*>(data),static_cast<std::streamsize>(size));return static_cast<std::size_t>(in.gcount());
}
}
std::string safeZipName(std::string name) {
    for(auto& c:name)if(c=='\\')c='/';
    while(name.starts_with("./"))name.erase(0,2);
    if(name.ends_with('/'))name.pop_back();
    if(name.empty() || name.size()>2048 || name.front()=='/' || name.find("//")!=std::string::npos || !validUtf8(name))throw std::runtime_error("ZIP 内路径无效");
    unsigned depth=0;
    for(const auto& part:fromUtf8(name)) {
        const auto value=utf8(part);if(++depth>20 || value.empty() || value.front()=='.' || value.back()=='.' || value.back()==' ' || value.size()>240)throw std::runtime_error("ZIP 内路径过深或含不安全文件名");
        for(unsigned char c:value)if(c<32 || c==127 || std::string(":*?\"<>|").find(c)!=std::string::npos)throw std::runtime_error("ZIP 内含越界或系统路径");
        const auto stem=folded(value.substr(0,value.find('.')));
        if(stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" || (stem.size()==4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem.back()>='1' && stem.back()<='9'))throw std::runtime_error("ZIP 内含系统保留文件名");
    }
    return name;
}
struct ZipReader::Impl {
    mz_zip_archive zip{};std::vector<ZipEntry> entries;
    ~Impl(){if(zip.m_pState)mz_zip_reader_end(&zip);}
};
ZipReader::ZipReader(std::string_view bytes):impl_(std::make_unique<Impl>()) {
    if(bytes.empty() || bytes.size()>fileLimit || !mz_zip_reader_init_mem(&impl_->zip,bytes.data(),bytes.size(),0))throw std::runtime_error("无法读取 ZIP，文件无效或超过 128 MiB");
    const auto count=mz_zip_reader_get_num_files(&impl_->zip);if(count>10000)throw std::runtime_error("ZIP 最多允许 10000 个条目");
    std::set<std::string> names;std::uint64_t total=0;
    for(mz_uint i=0;i<count;++i) {
        mz_zip_archive_file_stat stat{};if(!mz_zip_reader_file_stat(&impl_->zip,i,&stat))throw std::runtime_error("ZIP 目录损坏");
        if(stat.m_is_encrypted || !stat.m_is_supported || ((stat.m_external_attr>>16)&0170000)==0120000 || (stat.m_external_attr&0x400))throw std::runtime_error("ZIP 不支持加密、链接或当前压缩方式");
        if(stat.m_uncomp_size>fileLimit || stat.m_uncomp_size>totalLimit-total)throw std::runtime_error("ZIP 解压后超过单文件 128 MiB 或总计 512 MiB 限制");
        total+=stat.m_uncomp_size;
        const auto length=mz_zip_reader_get_filename(&impl_->zip,i,nullptr,0);if(!length || length>4096)throw std::runtime_error("ZIP 文件名过长");
        std::string name(length,'\0');mz_zip_reader_get_filename(&impl_->zip,i,name.data(),length);name.resize(length-1);
        if(!validUtf8(name)) {if(stat.m_bit_flag&0x800)throw std::runtime_error("ZIP UTF-8 文件名无效");name=legacy2::gbkToUtf8(name);}
        name=safeZipName(std::move(name));
        if(!names.insert(folded(name)).second)throw std::runtime_error("ZIP 含重名或大小写冲突条目");
        impl_->entries.push_back({name,stat.m_uncomp_size,i,stat.m_is_directory!=0});
    }
    std::set<std::string> files;for(const auto& entry:impl_->entries)if(!entry.directory)files.insert(folded(entry.name));
    for(const auto& entry:impl_->entries)for(auto parent=fromUtf8(folded(entry.name)).parent_path();!parent.empty();parent=parent.parent_path())if(files.count(utf8(parent)))throw std::runtime_error("ZIP 文件与目录路径冲突");
}
ZipReader::~ZipReader()=default;
const std::vector<ZipEntry>& ZipReader::entries() const{return impl_->entries;}
std::string ZipReader::read(const ZipEntry& entry,std::uint64_t limit) {
    if(entry.directory || entry.size>limit || entry.size>fileLimit)throw std::runtime_error("ZIP 条目不是文件或超过读取限制");
    std::string bytes(static_cast<std::size_t>(entry.size),'\0');
    if(!mz_zip_reader_extract_to_mem(&impl_->zip,entry.index,bytes.data(),bytes.size(),0))throw std::runtime_error("ZIP 内容或 CRC 校验失败："+entry.name);
    return bytes;
}
void writeZip(const fs::path& output,const std::vector<ZipSource>& files) {
    if(files.empty() || files.size()>10000)throw std::runtime_error("打包文件数量无效");
    std::set<std::string> names;std::uint64_t total=0;
    for(const auto& entry:files){auto name=safeZipName(entry.name);if(!names.insert(folded(name)).second)throw std::runtime_error("打包名称冲突");const auto size=entry.file.empty()?entry.bytes.size():fs::file_size(entry.file);if(size>fileLimit || size>totalLimit-total)throw std::runtime_error("打包资源超过大小限制");total+=size;}
    std::ofstream out(output,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("无法创建压缩包");
    mz_zip_archive zip{};zip.m_pWrite=write;zip.m_pIO_opaque=&out;
    if(!mz_zip_writer_init_v2(&zip,0,MZ_ZIP_FLAG_WRITE_ZIP64))throw std::runtime_error("无法初始化 ZIP 写入");
    try {
        for(const auto& entry:files) {
            bool ok=false;
            if(entry.file.empty())ok=mz_zip_writer_add_mem(&zip,entry.name.c_str(),entry.bytes.data(),entry.bytes.size(),MZ_BEST_SPEED)!=0;
            else {std::ifstream in(entry.file,std::ios::binary);if(!in)throw std::runtime_error("图片资源读取失败");ok=mz_zip_writer_add_read_buf_callback(&zip,entry.name.c_str(),read,&in,fs::file_size(entry.file),nullptr,nullptr,0,MZ_BEST_SPEED,nullptr,0,nullptr,0)!=0;}
            if(!ok)throw std::runtime_error("压缩包写入失败："+entry.name);
        }
        if(!mz_zip_writer_finalize_archive(&zip))throw std::runtime_error("压缩包完成校验失败");
        mz_zip_writer_end(&zip);out.close();if(!out)throw std::runtime_error("压缩包保存失败");
    } catch(...){mz_zip_writer_end(&zip);out.close();std::error_code ec;fs::remove(output,ec);throw;}
}
TemporaryDirectory::TemporaryDirectory(const fs::path& parent) {
    if(migration_resources::isLink(parent))throw std::runtime_error("暂存目录不能为链接");
    fs::create_directories(parent);
    for(unsigned attempt=0;attempt<10;++attempt){unsigned char bytes[16];if(RAND_bytes(bytes,sizeof(bytes))!=1)throw std::runtime_error("无法生成暂存目录名");std::string name;const char* hex="0123456789abcdef";for(auto b:bytes){name+=hex[b>>4];name+=hex[b&15];}auto candidate=parent/fromUtf8(name);if(fs::create_directory(candidate)){path=fs::absolute(candidate);return;}}
    throw std::runtime_error("无法创建独立暂存目录");
}
TemporaryDirectory::~TemporaryDirectory(){if(!path.empty()){std::error_code ec;fs::remove_all(path,ec);}}
ImageArchiveDownload exportImages(Service& service,const std::vector<std::string>& paths) {
    auto mutation=service.lockMutation();if(paths.empty() || paths.size()>1000)throw std::runtime_error("请选择 1–1000 个资源");
    std::vector<ZipSource> files;std::set<std::string> selected(paths.begin(),paths.end());
    for(const auto& path:selected) {
        const auto source=service.checkedPath(path);bool nested=false;
        for(auto parent=fromUtf8(path).parent_path();!parent.empty();parent=parent.parent_path())if(selected.count(utf8(parent))){nested=true;break;}
        if(nested)continue;
        auto add=[&](const fs::path& file) {
            if(!migration_resources::safePath(service.root(),file) || !imageExtension(file))throw std::runtime_error("所选目录含链接或非图片文件");
            std::ifstream input(file,std::ios::binary);char prefix[1024]{};input.read(prefix,sizeof(prefix));(void)mimeFor(std::string_view(prefix,static_cast<std::size_t>(input.gcount())),utf8(file.filename()));
            files.push_back({utf8(file.lexically_relative(source.parent_path())),file,{}});if(files.size()>10000)throw std::runtime_error("一次最多打包 10000 张图片");
        };
        if(fs::is_regular_file(source))add(source);
        else if(fs::is_directory(source)){for(auto it=fs::recursive_directory_iterator(source);it!=fs::recursive_directory_iterator();++it){if(migration_resources::isLink(it->path()))throw std::runtime_error("图片目录内含链接");if(it->is_regular_file())add(it->path());}}
        else throw std::runtime_error("所选资源不存在");
    }
    if(files.empty())throw std::runtime_error("所选目录中没有可打包的图片");
    auto temporary=std::make_shared<TemporaryDirectory>(service.dataRoot()/"image-tmp");auto file=temporary->path/"images.zip";writeZip(file,files);return {temporary,file};
}
nlohmann::json importImages(Service& service,std::string_view bytes,const std::string& directory,bool overwrite) {
    auto mutation=service.lockMutation();ZipReader zip(bytes);const auto destination=service.checkedPath(directory,true);
    TemporaryDirectory temporary(service.dataRoot()/"image-tmp");
    struct Pending {std::string path;fs::path target,backup;bool existed;};std::vector<Pending> files;
    for(const auto& entry:zip.entries()) {
        if(entry.directory)continue;if(!imageExtension(fromUtf8(entry.name)))throw std::runtime_error("图片 ZIP 内含非图片文件："+entry.name);
        const auto path=(directory.empty()?"":directory+"/")+entry.name;const auto target=service.checkedPath(path);
        const auto content=zip.read(entry);(void)mimeFor(content,entry.name);
        const auto stage=temporary.path/"stage"/fromUtf8(entry.name);fs::create_directories(stage.parent_path());std::ofstream out(stage,std::ios::binary);if(!out.write(content.data(),static_cast<std::streamsize>(content.size())))throw std::runtime_error("ZIP 图片暂存失败");out.close();if(!out)throw std::runtime_error("ZIP 图片保存失败");
        if(fs::exists(target) && (!overwrite || !fs::is_regular_file(target)))throw Conflict("ZIP 解压位置已有同名文件："+path);
        files.push_back({path,target,{},fs::exists(target)});
    }
    if(files.empty())throw std::runtime_error("ZIP 中没有图片");
    std::size_t done=0;
    try {
        for(auto& file:files) {
            if(file.existed){file.backup=temporary.path/"backup"/fromUtf8(file.path);fs::create_directories(file.backup.parent_path());fs::copy_file(file.target,file.backup);}
            ++done;fs::create_directories(file.target.parent_path());fs::copy_file(temporary.path/"stage"/file.target.lexically_relative(destination),file.target,fs::copy_options::overwrite_existing);
        }
        service.refresh();
    } catch(...) {
        while(done){const auto& file=files[--done];std::error_code ec;if(file.existed && !file.backup.empty())fs::copy_file(file.backup,file.target,fs::copy_options::overwrite_existing,ec);else if(!file.existed)fs::remove(file.target,ec);}throw;
    }
    return nlohmann::json{{"completed",files.size()}};
}
}
