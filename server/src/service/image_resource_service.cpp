#include "image_resource_service.h"
#include "../storage/legacy_import_resources.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>
#include <stdexcept>

namespace dice::image_resources {
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
constexpr std::uintmax_t maxImageBytes = 128ULL * 1024 * 1024;
std::mutex defaultMutex;
std::shared_ptr<Service> defaultService;
std::int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string lower(std::string text) { for (auto& c:text) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return text; }
std::string randomId(std::size_t bytes=16) {
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(),static_cast<int>(bytes))!=1) throw std::runtime_error("无法生成资源标识");
    constexpr char hex[]="0123456789abcdef"; std::string out;
    for (auto b:random) { out+=hex[b>>4]; out+=hex[b&15]; } return out;
}
struct Statement {
    sqlite3_stmt* value=nullptr;
    Statement(sqlite3* db,const std::string& sql) {
        if (sqlite3_prepare_v2(db,sql.c_str(),-1,&value,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    void bind(int i,const std::string& value) { if(sqlite3_bind_text(this->value,i,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK) throw std::runtime_error("资源参数过长"); }
    void bind(int i,std::int64_t value) { sqlite3_bind_int64(this->value,i,value); }
    bool step() { int rc=sqlite3_step(value); if(rc!=SQLITE_ROW && rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(value))); return rc==SQLITE_ROW; }
    std::string text(int i) const { const auto* p=sqlite3_column_text(value,i); return p ? std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(value,i)) : std::string(); }
    std::int64_t integer(int i) const { return sqlite3_column_int64(value,i); }
};
void exec(sqlite3* db,const char* sql) { if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db)); }
std::string clean(std::string value,bool empty=false) {
    try{(void)Json(value).dump();}catch(...){throw std::runtime_error("文件名必须使用有效的 UTF-8 编码");}
    for(auto& c:value) if(c=='\\') c='/';
    if(value.empty()) { if(empty) return {}; throw std::runtime_error("文件路径不能为空"); }
    if(value.size()>2048 || value.front()=='/' || value.back()=='/' || value.find("//")!=std::string::npos)
        throw std::runtime_error("图片路径无效");
    for(const auto& part:fromUtf8(value)) {
        const auto name=utf8(part), stem=lower(name.substr(0,name.find('.')));
        if(name.empty() || name=="." || name==".." || name.front()=='.' || name.back()=='.' || name.back()==' ' || name.size()>240)
            throw std::runtime_error("目录或文件名无效");
        for(unsigned char c:name) if(c<32 || c==127 || std::string(":*?\"<>|").find(c)!=std::string::npos) throw std::runtime_error("文件名含不允许的字符");
        if(stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" || (stem.size()==4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3]>='1' && stem[3]<='9'))
            throw std::runtime_error("不能使用系统保留文件名");
    }
    return value;
}
bool childOf(const std::string& path,const std::string& directory) {
    return path==directory || (path.size()>directory.size() && path.compare(0,directory.size(),directory)==0 && path[directory.size()]=='/');
}
std::string parentOf(const std::string& path) { const auto at=path.rfind('/'); return at==std::string::npos ? "" : path.substr(0,at); }
std::string basename(const std::string& path) { return path.substr(path.rfind('/')==std::string::npos ? 0 : path.rfind('/')+1); }
std::string referenceKey(std::string value) {
    for(auto& c:value) if(c=='\\') c='/';
    if(value.starts_with("file://")) value.erase(0,7);
    while(value.starts_with("./")) value.erase(0,2);
    // Only known old asset endpoints are aliases; this never fetches an URL.
    auto at=value.find("/api/assets/");
    if(at!=std::string::npos) {
        auto tail=value.substr(at+12);
        if(!tail.empty() && tail.find_first_of("/?#") == std::string::npos) return "data/assets/"+tail;
    }
    for(const auto& entry:std::vector<std::pair<std::string,std::string>>{{"/api/chat/images/","data/images/cache/chat/"},{"/api/logs/images/","data/images/cache/log/"}}) {
        at=value.find(entry.first);
        if(at!=std::string::npos) {auto tail=value.substr(at+entry.first.size());if(!tail.empty() && tail.find_first_of("/?#\\") == std::string::npos)return entry.second+tail;}
    }
    at=value.find("/api/resources/images/content?id=");
    if(at!=std::string::npos) {auto id=value.substr(at+33);id=id.substr(0,id.find('&'));if(id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos)return id;}
    return value;
}
std::string digest(const fs::path& path) {
    std::ifstream input(path,std::ios::binary); if(!input) throw std::runtime_error("无法读取图片文件");
    auto context=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!context || EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1) throw std::runtime_error("无法校验图片");
    char buffer[64*1024]; while(input) { input.read(buffer,sizeof(buffer)); if(input.gcount()>0) EVP_DigestUpdate(context.get(),buffer,static_cast<std::size_t>(input.gcount())); }
    if(!input.eof()) throw std::runtime_error("图片读取中断");
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned int length=0; EVP_DigestFinal_ex(context.get(),bytes,&length);
    constexpr char hex[]="0123456789abcdef"; std::string out; for(unsigned i=0;i<length;++i) { out+=hex[bytes[i]>>4]; out+=hex[bytes[i]&15]; } return out;
}
std::string readFile(const fs::path& path,std::uintmax_t limit=maxImageBytes) {
    if(!fs::is_regular_file(path) || fs::file_size(path)>limit) throw std::runtime_error("文件不存在或超过大小限制");
    std::ifstream input(path,std::ios::binary); if(!input) throw std::runtime_error("无法读取文件");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void writeFile(const fs::path& path,std::string_view bytes) {
    fs::create_directories(path.parent_path()); std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output || !output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("无法写入图片资源");
    output.close(); if(!output) throw std::runtime_error("图片保存失败");
}
std::string mimeOfFile(const fs::path& path) {
    std::ifstream input(path,std::ios::binary); char prefix[1024]{}; input.read(prefix,sizeof(prefix));
    return mimeFor(std::string_view(prefix,static_cast<std::size_t>(input.gcount())),utf8(path.filename()));
}
const std::set<std::string> systemFolders{"reply","deck","text","chat","shared","plugin","mod","cache","cache/chat","cache/log"};
}

bool imageExtension(const fs::path& path) {
    static const std::set<std::string> extensions{".png",".jpg",".jpeg",".gif",".webp",".bmp",".ico",".avif",".svg"};
    return extensions.count(lower(utf8(path.extension())))!=0;
}
std::string mimeFor(std::string_view b,const std::string& filename) {
    auto starts=[&](std::string_view signature){ return b.size()>=signature.size() && b.substr(0,signature.size())==signature; };
    if(starts(std::string_view("\x89PNG\r\n\x1a\n",8))) return "image/png";
    if(starts(std::string_view("\xff\xd8\xff",3))) return "image/jpeg";
    if(starts("GIF87a") || starts("GIF89a")) return "image/gif";
    if(b.size()>=12 && b.substr(0,4)=="RIFF" && b.substr(8,4)=="WEBP") return "image/webp";
    if(starts("BM")) return "image/bmp";
    if(starts(std::string_view("\0\0\1\0",4))) return "image/x-icon";
    if(b.size()>=16 && b.substr(4,4)=="ftyp" && (b.substr(8,4)=="avif" || b.substr(8,4)=="avis")) return "image/avif";
    // SVG is served with sandbox/nosniff and never inserted as inline markup.
    if(lower(utf8(fromUtf8(filename).extension()))==".svg" && b.find("<svg")!=std::string_view::npos) return "image/svg+xml";
    throw std::runtime_error("不是受支持的图片文件；仅允许 PNG、JPEG、GIF、WebP、BMP、ICO、AVIF、SVG");
}

struct Service::Impl {
    fs::path data, images;
    sqlite3* db=nullptr;
    mutable std::recursive_mutex mutex;
    std::recursive_mutex mutation;
    Scan scan; Save save;std::function<void()> documentsChanged;
    Json migration=Json{{"status","pending"},{"copied",0},{"warnings",Json::array()}};
    Json recoveryWarnings=Json::array();
    std::future<void> worker;
    explicit Impl(fs::path path) {
        fs::create_directories(path); data=fs::canonical(path); images=data/"images";
        if(migration_resources::isLink(images)) throw std::runtime_error("图片根目录不能是符号链接或重解析点");
        fs::create_directories(images); images=fs::canonical(images);
        if(sqlite3_open_v2(utf8(data/"image-resources.db").c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK)
            throw std::runtime_error("无法打开图片资源索引");
        sqlite3_busy_timeout(db,3000);
        exec(db,"PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"
            "CREATE TABLE IF NOT EXISTS resources(id TEXT PRIMARY KEY,path TEXT NOT NULL UNIQUE,parent TEXT NOT NULL,name TEXT NOT NULL,"
            "is_dir INTEGER NOT NULL,bytes INTEGER NOT NULL,modified INTEGER NOT NULL,hash TEXT NOT NULL DEFAULT '',mime TEXT NOT NULL DEFAULT '',"
            "token TEXT UNIQUE,trashed INTEGER NOT NULL DEFAULT 0,origin TEXT NOT NULL DEFAULT '');"
            "CREATE INDEX IF NOT EXISTS idx_images_parent ON resources(trashed,parent,name);"
            "CREATE TABLE IF NOT EXISTS aliases(alias TEXT PRIMARY KEY,resource_id TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS refs(resource_id TEXT NOT NULL,document_id TEXT NOT NULL,label TEXT NOT NULL,type TEXT NOT NULL,PRIMARY KEY(resource_id,document_id));"
            "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY,v TEXT NOT NULL);");
        for(const auto& folder:systemFolders) fs::create_directories(images/fromUtf8(folder));
    }
    ~Impl() { if(worker.valid())worker.wait(); if(db) sqlite3_close(db); }
    fs::path path(const std::string& relative,bool allowRoot=false,bool internal=false) const {
        const auto safe=internal ? relative : clean(relative,allowRoot);
        const auto result=(images/fromUtf8(safe)).lexically_normal();
        if(!migration_resources::within(images,result)) throw std::runtime_error("路径超出图片资源目录");
        fs::path cursor=images;
        for(const auto& part:result.lexically_relative(images)) {
            cursor/=part;
            if(migration_resources::isLink(cursor)) throw std::runtime_error("不允许操作符号链接或重解析点");
        }
        return result;
    }
    std::string registerPath(const std::string& relative) {
        const auto file=path(relative); const auto status=fs::status(file); const bool directory=fs::is_directory(status);
        if(!directory && (!fs::is_regular_file(status) || !imageExtension(file))) throw std::runtime_error("不是受支持的图片或目录");
        const auto size=directory ? 0 : fs::file_size(file);
        const auto modified=fs::last_write_time(file)-fs::file_time_type::clock::now()+std::chrono::system_clock::now();
        const auto epoch=std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch()).count();
        Statement previous(db,"SELECT id FROM resources WHERE path=?1 AND trashed=0"); previous.bind(1,relative);
        const auto id=previous.step() ? previous.text(0) : randomId();
        Statement put(db,"INSERT INTO resources(id,path,parent,name,is_dir,bytes,modified) VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(id) DO UPDATE SET parent=excluded.parent,name=excluded.name,is_dir=excluded.is_dir,bytes=excluded.bytes,modified=excluded.modified");
        put.bind(1,id);put.bind(2,relative);put.bind(3,parentOf(relative));put.bind(4,basename(relative));put.bind(5,directory?1:0);put.bind(6,static_cast<std::int64_t>(size));put.bind(7,epoch);put.step();
        return id;
    }
    std::optional<std::pair<std::string,std::string>> find(const std::string& reference) const {
        auto key=referenceKey(reference);
        // An API cannot use an arbitrary absolute path. Only aliases explicitly
        // registered during an approved migration can map an old location.
        Statement query(db,"SELECT r.id,r.path FROM resources r WHERE r.trashed=0 AND (r.id=?1 OR r.path=?2 OR r.id IN(SELECT resource_id FROM aliases WHERE alias=?1)) LIMIT 1");
        query.bind(1,key);query.bind(2,key.starts_with("data/images/") ? key.substr(12) : std::string());
        if(query.step()) return std::pair{query.text(0),query.text(1)};
        return std::nullopt;
    }
    void alias(const std::string& old,const std::string& id) {
        Statement put(db,"INSERT INTO aliases(alias,resource_id) VALUES(?1,?2) ON CONFLICT(alias) DO UPDATE SET resource_id=excluded.resource_id");
        put.bind(1,referenceKey(old));put.bind(2,id);put.step();
    }
    void scanDirectory(const std::string& directory,bool recursive=false) {
        const auto base=path(directory,true); if(!fs::is_directory(base)) throw std::runtime_error("文件夹不存在");
        std::set<std::string> found;
        auto visit=[&](const fs::directory_entry& entry) {
            const auto relative=utf8(entry.path().lexically_relative(images));
            if(utf8(entry.path().filename()).starts_with(".")) return;
            if(migration_resources::isLink(entry.path())) return;
            if(entry.is_directory() || (entry.is_regular_file() && imageExtension(entry.path()))) { registerPath(relative); found.insert(relative); }
        };
        if(recursive) {
            for(fs::recursive_directory_iterator it(base),end;it!=end;++it) {
                if(utf8(it->path().filename()).starts_with(".") || migration_resources::isLink(it->path())) { if(it->is_directory()) it.disable_recursion_pending(); continue; }
                visit(*it);
            }
        } else for(const auto& entry:fs::directory_iterator(base)) visit(entry);
        // A file removed outside the WebUI must not remain a downloadable row.
        Statement all(db,recursive ? "SELECT id,path FROM resources WHERE trashed=0" : "SELECT id,path FROM resources WHERE trashed=0 AND parent=?1");
        if(!recursive) all.bind(1,directory);
        std::vector<std::string> missing;
        while(all.step()) { const auto p=all.text(1); if(!found.count(p) && !fs::exists(path(p))) missing.push_back(all.text(0)); }
        for(const auto& id:missing) { Statement remove(db,"DELETE FROM resources WHERE id=?1");remove.bind(1,id);remove.step(); }
    }
    std::vector<Document> documents() const { return scan ? scan() : std::vector<Document>(); }
};

Service::Service(fs::path dataRoot):impl_(std::make_unique<Impl>(std::move(dataRoot))) {}
Service::~Service()=default;
std::unique_lock<std::recursive_mutex> Service::lockMutation() {return std::unique_lock(impl_->mutation);}
fs::path Service::root() const { return impl_->images; }
fs::path Service::dataRoot() const { return impl_->data; }
fs::path Service::checkedPath(const std::string& relative,bool allowRoot) const { std::lock_guard lock(impl_->mutex);return impl_->path(relative,allowRoot); }
void Service::setDocuments(Scan scan,Save save,std::function<void()> changed) { std::lock_guard lock(impl_->mutex);impl_->scan=std::move(scan);impl_->save=std::move(save);impl_->documentsChanged=std::move(changed); }
void Service::mkdir(const std::string& directory) {auto mutation=lockMutation(); std::lock_guard lock(impl_->mutex); fs::create_directories(impl_->path(directory));impl_->registerPath(directory); }
void Service::refresh() { std::lock_guard lock(impl_->mutex);impl_->scanDirectory("",true); }

Json Service::describe(const std::string& path) {
    std::lock_guard lock(impl_->mutex);const auto relative=clean(path);const auto id=impl_->registerPath(relative);
    Statement row(impl_->db,"SELECT id,path,name,is_dir,bytes,modified,(SELECT COUNT(*) FROM refs WHERE resource_id=resources.id) FROM resources WHERE id=?1");row.bind(1,id);row.step();
    return Json{{"id",row.text(0)},{"path",row.text(1)},{"name",row.text(2)},{"isDirectory",row.integer(3)!=0},{"size",row.integer(4)},{"modified",row.integer(5)},
        {"references",row.integer(6)},{"code",row.integer(3) ? "" : imageCode("data/images/"+relative)}};
}
Json Service::list(std::string directory,int page,int size,std::string query,std::string sort,bool descending,bool trash) {
    std::lock_guard lock(impl_->mutex);directory=clean(directory,true);page=(std::max)(1,page);size=std::clamp(size,1,100);
    if(!trash && !fs::exists(impl_->path(directory,true))) return Json{{"items",Json::array()},{"total",0},{"page",1},{"size",size},{"directory",directory},{"root","data/images"},{"exists",false},{"migration",impl_->migration}};
    if(!trash) impl_->scanDirectory(directory);
    const std::map<std::string,std::string> columns{{"name","name COLLATE NOCASE"},{"path","path COLLATE NOCASE"},{"size","bytes"},{"modified","modified"}};
    const auto column=columns.find(sort);
    const std::string where=trash ? " WHERE trashed=1 AND (?2='' OR instr(lower(origin),lower(?2))>0)" :
        " WHERE trashed=0 AND parent=?1 AND (?2='' OR instr(lower(name),lower(?2))>0 OR instr(lower(path),lower(?2))>0)";
    Statement count(impl_->db,"SELECT COUNT(*) FROM resources"+where);count.bind(1,directory);count.bind(2,query);count.step();const auto total=count.integer(0);
    page=(std::min)(page,static_cast<int>((std::max)(std::int64_t(1),(total+size-1)/size)));
    Statement rows(impl_->db,"SELECT id,path,name,is_dir,bytes,modified,origin,(SELECT COUNT(*) FROM refs WHERE resource_id=resources.id) FROM resources"+where+
        " ORDER BY is_dir DESC,"+(column==columns.end()?std::string("name COLLATE NOCASE"):column->second)+(descending?" DESC":" ASC")+",id ASC LIMIT ?3 OFFSET ?4");
    rows.bind(1,directory);rows.bind(2,query);rows.bind(3,size);rows.bind(4,static_cast<std::int64_t>(page-1)*size);
    auto items=Json::array();while(rows.step()) { const auto path=trash?rows.text(6):rows.text(1);items.push_back(Json{{"id",rows.text(0)},{"path",path},{"name",rows.text(2)},
        {"isDirectory",rows.integer(3)!=0},{"size",rows.integer(4)},{"modified",rows.integer(5)},{"references",rows.integer(7)},
        {"code",rows.integer(3)?"":imageCode("data/images/"+path)},{"protected",systemFolders.count(path)!=0}}); }
    Statement scanned(impl_->db,"SELECT v FROM meta WHERE k='refs_checked_at'");const auto checked=scanned.step()?scanned.text(0):"";
    return Json{{"items",items},{"total",total},{"page",page},{"size",size},{"directory",directory},{"root","data/images"},{"referencesCheckedAt",checked},{"migration",impl_->migration},{"recoveryWarnings",impl_->recoveryWarnings}};
}

Json Service::upload(const std::string& directory,const std::string& filename,std::string_view bytes,bool overwrite) {
    auto mutation=lockMutation();
    std::lock_guard lock(impl_->mutex);
    if(bytes.empty() || bytes.size()>maxImageBytes) throw std::runtime_error("图片为空或超过 128 MiB");
    if(filename.find_first_of("/\\")!=std::string::npos || !imageExtension(fromUtf8(filename))) throw std::runtime_error("图片文件名无效");
    const auto mime=mimeFor(bytes,filename), dir=clean(directory,true), relative=clean(dir.empty()?filename:dir+"/"+filename);
    const auto target=impl_->path(relative); if(fs::exists(target) && !overwrite) throw Conflict("目标已存在，请改名或确认替换");
    fs::create_directories(target.parent_path()); const auto temp=target.parent_path()/fromUtf8(".upload-"+randomId());
    writeFile(temp,bytes);
    fs::path backup;
    try {
        if(fs::exists(target)) { backup=impl_->data/"image-operation-backups"/randomId()/fromUtf8(relative);fs::create_directories(backup.parent_path());fs::rename(target,backup); }
        fs::rename(temp,target); const auto id=impl_->registerPath(relative);
        Statement metadata(impl_->db,"UPDATE resources SET mime=?2,hash=?3 WHERE id=?1");metadata.bind(1,id);metadata.bind(2,mime);metadata.bind(3,digest(target));metadata.step();
    } catch(...) {
        std::error_code ignored;fs::remove(temp,ignored);if(!backup.empty()) {fs::remove(target,ignored);fs::rename(backup,target,ignored);}throw;
    }
    auto result=describe(relative);result["url"]="/api/resources/images/content?id="+result["id"].get<std::string>();return result;
}
Json Service::importFile(const fs::path& sourceRoot,const fs::path& source,const std::string& destination,const std::string& oldReference) {
    auto mutation=lockMutation();
    std::lock_guard lock(impl_->mutex);
    if(!migration_resources::safePath(sourceRoot,source) || !fs::is_regular_file(source) || fs::file_size(source)>maxImageBytes || !imageExtension(source))
        throw std::runtime_error("迁移图片不存在、过大或超出选定目录");
    const auto mime=mimeOfFile(source);
    std::string relative=clean(destination);const auto hash=digest(source);auto target=impl_->path(relative);
    if(fs::exists(target) && digest(target)!=hash) {
        const auto p=fromUtf8(relative);relative=utf8(p.parent_path()/(p.stem().native()+fromUtf8("-"+hash.substr(0,10)).native()+p.extension().native()));target=impl_->path(relative);
        if(fs::exists(target) && digest(target)!=hash) throw std::runtime_error("迁移图片名称冲突，未覆盖已有文件");
    }
    if(!fs::exists(target)) { fs::create_directories(target.parent_path());fs::copy_file(source,target); }
    const auto id=impl_->registerPath(relative);
    Statement metadata(impl_->db,"UPDATE resources SET hash=?2,mime=?3 WHERE id=?1");metadata.bind(1,id);metadata.bind(2,hash);metadata.bind(3,mime);metadata.step();
    if(!oldReference.empty()) impl_->alias(oldReference,id);
    return describe(relative);
}
void Service::addAlias(const std::string& oldReference,const std::string& path) { std::lock_guard lock(impl_->mutex);impl_->alias(oldReference,impl_->registerPath(clean(path))); }
Json Service::importOwner(const fs::path& sourceRoot,const fs::path& directory,const std::string& category,const std::string& owner) {
    const auto prefix=clean(category+"/"+owner);int copied=0;auto warnings=Json::array();
    if(!fs::is_directory(directory))return Json{{"copied",0},{"warnings",warnings}};
    migration_resources::walk(sourceRoot,directory,[&](const fs::path& source) {
        if(!imageExtension(source))return;
        try {
            const auto relative=utf8(source.lexically_relative(directory));
            const auto imported=importFile(sourceRoot,source,prefix+"/"+relative,utf8(source));
            addAlias(utf8(fs::absolute(source).lexically_normal()),imported["path"].get<std::string>());
            // The instance root is canonical. Resolve the already safety-checked
            // source to the same spelling before deriving a work-relative alias:
            // Windows TEMP may contain an 8.3 name such as RUNNER~1. Keep both
            // absolute spellings for callers that still use the original path.
            const auto canonicalSource=fs::canonical(source);
            addAlias(utf8(canonicalSource),imported["path"].get<std::string>());
            const auto fromWork=canonicalSource.lexically_relative(impl_->data.parent_path());
            if(!fromWork.empty() && !utf8(fromWork).starts_with("../"))addAlias(utf8(fromWork),imported["path"].get<std::string>());
            ++copied;
        } catch(const std::exception& error){warnings.push_back(utf8(source)+"："+error.what());}
    },[&](const auto& file,const auto& error){warnings.push_back(utf8(file)+"："+error);});
    return Json{{"copied",copied},{"warnings",warnings}};
}
std::optional<fs::path> Service::resolve(const std::string& reference) const {
    std::lock_guard lock(impl_->mutex);auto found=impl_->find(reference);
    if(found) { const auto path=impl_->path(found->second);if(fs::is_regular_file(path)) return path;return std::nullopt; }
    const auto key=referenceKey(reference);
    if(key.starts_with("data/images/")) {
        try { auto path=impl_->path(key.substr(12));if(fs::is_regular_file(path) && imageExtension(path)) return path; } catch(...) {}
    }
    return std::nullopt;
}
bool Service::known(const std::string& reference) const {
    std::lock_guard lock(impl_->mutex);const auto key=referenceKey(reference);
    std::string relative;
    if(key.starts_with("data/images/")) {
        // Reject invalid managed paths, but do not treat an as-yet unmigrated
        // cache URL as a deleted resource. Its legacy fallback is still needed.
        try {relative=clean(key.substr(12));} catch(...) {return true;}
    }
    Statement query(impl_->db,"SELECT 1 FROM resources WHERE id=?1 OR (?2<>'' AND (path=?2 OR (trashed<>0 AND origin=?2))) "
        "UNION ALL SELECT 1 FROM aliases WHERE alias=?1 LIMIT 1");
    query.bind(1,key);query.bind(2,relative);return query.step();
}
std::string Service::canonicalReference(const std::string& reference) const {
    if(const auto resolved=resolve(reference)) return "data/images/"+utf8(resolved->lexically_relative(impl_->images));
    return reference;
}
std::optional<fs::path> Service::resolveInDocument(const Document& document,const std::string& reference) const {
    if(auto path=resolve(reference))return path;
    if(!document.baseDirectory.empty() && reference.size()<2048 && reference.find_first_of("\r\n{}[]") == std::string::npos && imageExtension(fromUtf8(reference))) {
        const auto path=(document.baseDirectory/fromUtf8(reference)).lexically_normal();return resolve(utf8(path));
    }
    return std::nullopt;
}
std::string Service::share(const std::string& path) {
    std::lock_guard lock(impl_->mutex);const auto relative=clean(path);
    if(relative.starts_with("cache/")) throw std::runtime_error("聊天及日志缓存不能通过图片资源公共链接公开");
    const auto id=impl_->registerPath(relative);if(fs::is_directory(impl_->path(relative))) throw std::runtime_error("不能公开目录");
    Statement previous(impl_->db,"SELECT token FROM resources WHERE id=?1");previous.bind(1,id);previous.step();auto token=previous.text(0);
    if(token.empty()) {token=randomId(32);Statement put(impl_->db,"UPDATE resources SET token=?2 WHERE id=?1");put.bind(1,id);put.bind(2,token);put.step();}
    return "/media/images/"+token;
}
std::optional<fs::path> Service::sharedPath(const std::string& token) const {
    std::lock_guard lock(impl_->mutex);if(token.size()!=64) return std::nullopt;
    Statement row(impl_->db,"SELECT path FROM resources WHERE trashed=0 AND is_dir=0 AND (token=?1 OR id IN(SELECT resource_id FROM aliases WHERE alias=?2)) LIMIT 1");row.bind(1,token);row.bind(2,"share:"+token);
    if(!row.step() || row.text(0).starts_with("cache/")) return std::nullopt;
    const auto path=impl_->path(row.text(0));return fs::is_regular_file(path)?std::optional(path):std::nullopt;
}
Json Service::scanReferences() {
    std::lock_guard lock(impl_->mutex);const auto documents=impl_->documents();int count=0;auto warnings=Json::array();
    exec(impl_->db,"BEGIN IMMEDIATE");
    try {
        exec(impl_->db,"DELETE FROM refs");
        for(const auto& document:documents) try {
            std::set<std::string> ids;
            for(const auto& reference:documentReferences(document)) if(auto path=resolveInDocument(document,reference)) ids.insert(impl_->registerPath(utf8(path->lexically_relative(impl_->images))));
            for(const auto& id:ids) {Statement put(impl_->db,"INSERT OR IGNORE INTO refs(resource_id,document_id,label,type) VALUES(?1,?2,?3,?4)");put.bind(1,id);put.bind(2,document.id);put.bind(3,document.label);put.bind(4,document.type);put.step();++count;}
        } catch(const std::exception& e) { warnings.push_back(Json{{"document",document.label},{"error",e.what()}}); }
        Statement mark(impl_->db,"INSERT INTO meta(k,v) VALUES('refs_checked_at',?1) ON CONFLICT(k) DO UPDATE SET v=excluded.v");mark.bind(1,std::to_string(now()));mark.step();
        exec(impl_->db,"COMMIT");
    } catch(...) {exec(impl_->db,"ROLLBACK");throw;}
    return Json{{"documents",documents.size()},{"references",count},{"warnings",warnings},{"dynamicReferencesUncertain",true}};
}
Json Service::usages(const std::string& path) {
    std::lock_guard lock(impl_->mutex);const auto scan=scanReferences();auto items=Json::array();
    Statement rows(impl_->db,"SELECT r.path,f.document_id,f.label,f.type FROM refs f JOIN resources r ON r.id=f.resource_id WHERE r.trashed=0");
    while(rows.step()) if(childOf(rows.text(0),clean(path))) items.push_back(Json{{"path",rows.text(0)},{"id",rows.text(1)},{"label",rows.text(2)},{"type",rows.text(3)}});
    return Json{{"items",items},{"warnings",scan["warnings"]},{"dynamicReferencesUncertain",true}};
}
Json Service::migrationStatus() const { std::lock_guard lock(impl_->mutex);return impl_->migration; }
void Service::waitForBackground() { if(impl_->worker.valid())impl_->worker.wait(); }
void Service::startMigration(bool force) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->worker.valid() && impl_->worker.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
    impl_->migration["status"]="running";
    impl_->worker=std::async(std::launch::async,[this,force] {
        try { (void)migrateExisting(force); }
        catch(const std::exception& e) {std::lock_guard lock(impl_->mutex);impl_->migration["status"]="failed";impl_->migration["error"]=e.what();}
    });
}

Json Service::operate(const std::string& action,const std::vector<std::string>& paths,const std::string& target,bool overwrite) {
    auto mutation=lockMutation();
    std::unique_lock lock(impl_->mutex);
    if(paths.empty() || paths.size()>1000) throw std::runtime_error("请选择 1–1000 个文件或目录");
    if(action=="restore" || action=="purge") {
        // Validate the complete selection before touching the first entry.
        std::set<std::string> destinations;
        for(const auto& id:paths) {
            Statement row(impl_->db,"SELECT path,origin FROM resources WHERE id=?1 AND trashed=1");row.bind(1,id);
            if(!row.step() || !row.text(0).starts_with(".trash/"))throw std::runtime_error("回收站条目不存在或无效");
            (void)impl_->path(row.text(0),false,true);
            if(action=="restore") {const auto original=clean(row.text(1));if(fs::exists(impl_->path(original)) || !destinations.insert(original).second)throw Conflict("恢复位置有同名文件，请分别处理，未恢复任何条目");}
        }
        auto restored=Json::array();
        for(const auto& id:paths) {
            Statement row(impl_->db,"SELECT path,origin FROM resources WHERE id=?1 AND trashed=1");row.bind(1,id);
            if(!row.step()) throw std::runtime_error("回收站条目不存在");
            const auto old=row.text(0), original=clean(row.text(1));const auto source=impl_->path(old,false,true);
            if(!old.starts_with(".trash/")) throw std::runtime_error("回收站索引无效");
            std::vector<std::pair<std::string,std::string>> descendants;
            Statement all(impl_->db,"SELECT id,path FROM resources WHERE trashed<>0");while(all.step()) if(childOf(all.text(1),old)) descendants.emplace_back(all.text(0),all.text(1));
            if(action=="purge") {
                fs::remove_all(source);
                for(const auto& [entryId,path]:descendants) {Statement remove(impl_->db,"DELETE FROM resources WHERE id=?1");remove.bind(1,entryId);remove.step();}
            } else {
                const auto destination=impl_->path(original);if(fs::exists(destination)) throw std::runtime_error("原位置已有同名文件，未覆盖；请先改名或移动已有文件");
                fs::create_directories(destination.parent_path());fs::rename(source,destination);
                exec(impl_->db,"BEGIN IMMEDIATE");
                try {
                    for(const auto& [entryId,path]:descendants) {
                        const auto next=original+path.substr(old.size());
                        Statement put(impl_->db,"UPDATE resources SET path=?2,parent=?3,name=?4,trashed=0,origin='' WHERE id=?1");
                        put.bind(1,entryId);put.bind(2,next);put.bind(3,parentOf(next));put.bind(4,basename(next));put.step();
                    }
                    exec(impl_->db,"COMMIT");
                } catch(...) {exec(impl_->db,"ROLLBACK");fs::rename(destination,source);throw;}
                restored.push_back(original);
            }
        }
        return Json{{"completed",paths.size()},{"restored",restored}};
    }
    if(action!="copy" && action!="move" && action!="rename" && action!="delete") throw std::runtime_error("不支持的文件操作");
    if(action=="rename" && paths.size()!=1) throw std::runtime_error("重命名只能选择一项");
    std::set<std::string> selected;
    for(const auto& value:paths) {
        const auto path=clean(value);
        if(systemFolders.count(path) && action!="copy") throw std::runtime_error("内置资源分类目录不能删除或改名，可管理其内部文件");
        if(!fs::exists(impl_->path(path))) throw std::runtime_error("所选文件已不存在，请刷新列表");
        selected.insert(path);
    }
    std::vector<std::string> roots;
    for(const auto& path:selected) {
        bool nested=false;for(const auto& other:selected) if(other!=path && childOf(path,other)) {nested=true;break;}
        if(!nested)roots.push_back(path);
    }
    refresh();
    if(action=="delete") {
        const auto scan=scanReferences();
        if(!scan["warnings"].empty()) throw std::runtime_error("部分文档无法检查引用，暂不能安全删除；请先查看引用扫描报告");
        Statement refs(impl_->db,"SELECT r.path,f.label FROM refs f JOIN resources r ON r.id=f.resource_id WHERE r.trashed=0");
        while(refs.step()) for(const auto& path:roots) if(childOf(refs.text(0),path)) throw std::runtime_error("资源仍被「"+refs.text(1)+"」引用，请先替换或移除引用");
        const auto batch=randomId();auto completed=Json::array();std::vector<std::pair<std::string,std::string>> moved;
        exec(impl_->db,"BEGIN IMMEDIATE");
        try {
            for(const auto& path:roots) {
                const auto next=".trash/"+batch+"/"+path;const auto destination=impl_->path(next,false,true);fs::create_directories(destination.parent_path());
                fs::rename(impl_->path(path),destination);moved.emplace_back(path,next);
                std::vector<std::pair<std::string,std::string>> rows;
                Statement all(impl_->db,"SELECT id,path FROM resources WHERE trashed=0");while(all.step())if(childOf(all.text(1),path))rows.emplace_back(all.text(0),all.text(1));
                for(const auto& [id,old]:rows) {
                    impl_->alias("data/images/"+old,id);
                    Statement put(impl_->db,"UPDATE resources SET path=?2,parent=?3,trashed=?4,origin=?5 WHERE id=?1");put.bind(1,id);put.bind(2,next+old.substr(path.size()));
                    put.bind(3,parentOf(next+old.substr(path.size())));put.bind(4,old==path?1:2);put.bind(5,old);put.step();
                }
                completed.push_back(path);
            }
            exec(impl_->db,"COMMIT");
        } catch(...) {
            exec(impl_->db,"ROLLBACK");bool failed=false;
            for(auto it=moved.rbegin();it!=moved.rend();++it){std::error_code ec;fs::rename(impl_->path(it->second,false,true),impl_->path(it->first),ec);failed=failed||bool(ec);}
            if(failed)throw std::runtime_error("部分回收操作无法回滚；文件未永久删除，仍保留于 data/images/.trash/"+batch);
            throw;
        }
        return Json{{"completed",completed},{"recoverable",true}};
    }
    const auto destinationRoot=clean(target,action!="rename");
    if(action!="rename" && !fs::is_directory(impl_->path(destinationRoot,true))) throw std::runtime_error("粘贴目标文件夹不存在");
    struct Entry { std::string from,to,id,oldHash; fs::path backup; bool existed=false; };
    std::vector<Entry> files;
    std::uintmax_t totalBytes=0;
    std::vector<std::pair<std::string,std::string>> directories;
    std::map<std::string,std::string> mapping;
    auto add=[&](const fs::path& source,const std::string& from,const std::string& to) {
        const auto destination=impl_->path(to);
        if(fs::is_directory(source)) { if(fs::exists(destination) && !fs::is_directory(destination)) throw std::runtime_error("目录与同名文件冲突");directories.emplace_back(from,to);return; }
        if(!fs::is_regular_file(source) || !imageExtension(source) || !imageExtension(destination)) throw std::runtime_error("只能复制或移动图片资源");
        const auto bytes=fs::file_size(source);
        if(bytes>maxImageBytes || bytes>512ULL*1024*1024-totalBytes || files.size()>=10000)throw std::runtime_error("一次最多操作 10000 张图片，总量不超过 512 MiB，单文件不超过 128 MiB");
        totalBytes+=bytes;
        (void)mimeOfFile(source);
        const bool existed=fs::exists(destination);
        if(existed && fs::equivalent(source,destination))throw Conflict("原文件与目标是同一文件；仅大小写更名请先使用一个临时名称");
        if(existed && (!overwrite || !fs::is_regular_file(destination))) throw Conflict("目标已存在，请确认替换或改名后粘贴");
        files.push_back({from,to,impl_->registerPath(from),digest(source),{},existed});
        mapping.emplace(from,to);
    };
    for(const auto& from:roots) {
        std::string to=action=="rename" ? destinationRoot : (destinationRoot.empty()?"":destinationRoot+"/")+basename(from);
        if(from==to) {
            if(action!="copy")continue;
            const auto path=fromUtf8(to);int suffix=1;
            do {to=utf8(path.parent_path()/(path.stem().native()+fromUtf8("-copy-"+std::to_string(suffix++)).native()+path.extension().native()));} while(fs::exists(impl_->path(to)));
        }
        if(childOf(to,from)) throw std::runtime_error("不能把文件夹复制或移动到自身内部");
        const auto source=impl_->path(from);
        if(fs::is_directory(source) && migration_resources::within(source,impl_->path(to)))throw std::runtime_error("不能把文件夹复制或移动到自身内部（包括仅大小写不同的路径）");
        add(source,from,to);
        if(fs::is_directory(source)) for(const auto& entry:fs::recursive_directory_iterator(source)) {
            if(migration_resources::isLink(entry.path())) throw std::runtime_error("目录含符号链接，未执行操作");
            const auto relative=utf8(entry.path().lexically_relative(source));add(entry.path(),from+"/"+relative,to+"/"+relative);
        }
    }
    if(files.size()>10000) throw std::runtime_error("一次最多操作 10000 个图片文件");
    std::vector<Change> changes;
    if(action!="copy") for(const auto& document:impl_->documents()) {
        const auto rewritten=rewriteDocument(document,[&](const std::string& ref)->std::optional<std::string> {
            const auto resolved=resolveInDocument(document,ref);if(!resolved)return std::nullopt;
            const auto found=mapping.find(utf8(resolved->lexically_relative(impl_->images)));
            if(found==mapping.end())return std::nullopt;return "data/images/"+found->second;
        });
        if(rewritten!=document.content && document.writable) changes.push_back({document,rewritten});
    }
    if(!changes.empty() && !impl_->save) throw std::runtime_error("引用更新服务未就绪");
    const auto batch=randomId();
    const auto backupRoot=impl_->data/"image-operation-backups"/batch;
    fs::create_directories(backupRoot);
    auto journal=Json{{"action",action},{"phase","preparing"},{"files",Json::array()},{"changes",Json::array()}};
    for(const auto& change:changes) journal["changes"].push_back(Json{{"id",change.before.id},{"before",change.before.content},{"after",change.after}});
    for(const auto& file:files)journal["files"].push_back(Json{{"from",file.from},{"to",file.to},{"existed",file.existed},{"hash",file.oldHash}});
    writeFile(backupRoot/"journal.json",journal.dump(2));
    auto warnings=Json::array();
    std::size_t saved=0;
    try {
        // The old sources remain readable while large files are copied. Keep
        // mutation serialization, but do not hold up message sends on metadata.
        lock.unlock();
        try {
        for(const auto& [from,to]:directories) fs::create_directories(impl_->path(to));
        for(auto& file:files) {
            const auto destination=impl_->path(file.to);fs::create_directories(destination.parent_path());
            if(file.existed) {file.backup=backupRoot/fromUtf8(file.to);fs::create_directories(file.backup.parent_path());fs::copy_file(destination,file.backup);}
            const auto temporary=destination.parent_path()/fromUtf8(".copy-"+batch);
            try {
                fs::copy_file(impl_->path(file.from),temporary,fs::copy_options::overwrite_existing);
                if(digest(temporary)!=file.oldHash)throw std::runtime_error("复制图片校验失败");
#ifdef _WIN32
                if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("图片副本发布失败");
#else
                fs::rename(temporary,destination);
#endif
            }catch(...){std::error_code ec;fs::remove(temporary,ec);throw;}
        }
        }catch(...){lock.lock();throw;}
        lock.lock();
        journal["phase"]="references";writeFile(backupRoot/"journal.json",journal.dump(2));
        for(const auto& change:changes) {impl_->save(change);++saved;}
        if(!changes.empty() && impl_->documentsChanged)impl_->documentsChanged();
        exec(impl_->db,"BEGIN IMMEDIATE");
        try {
            for(const auto& file:files) {
                const auto nextId=impl_->registerPath(file.to);
                if(action!="copy") {
                    impl_->alias("data/images/"+file.from,nextId);impl_->alias(file.id,nextId);
                    Statement previous(impl_->db,"SELECT token FROM resources WHERE id=?1");previous.bind(1,file.id);previous.step();const auto token=previous.text(0);
                    if(!token.empty())impl_->alias("share:"+token,nextId);
                    Statement redirects(impl_->db,"UPDATE aliases SET resource_id=?2 WHERE resource_id=?1");redirects.bind(1,file.id);redirects.bind(2,nextId);redirects.step();
                    Statement retire(impl_->db,"UPDATE resources SET path=?2,trashed=2 WHERE id=?1 AND id<>?3");retire.bind(1,file.id);retire.bind(2,".retired/"+file.id);retire.bind(3,nextId);retire.step();
                }
            }
            exec(impl_->db,"COMMIT");
        } catch(...) {exec(impl_->db,"ROLLBACK");throw;}
    } catch(...) {
        bool reversalFailed=false;
        while(saved) {const auto& change=changes[--saved];auto reverse=change;reverse.before.content=change.after;reverse.after=change.before.content;try{impl_->save(reverse);}catch(...){reversalFailed=true;}}
        if(impl_->documentsChanged)try{impl_->documentsChanged();}catch(...){reversalFailed=true;}
        if(reversalFailed) {
            journal["phase"]="recovery_required";try{writeFile(backupRoot/"journal.json",journal.dump(2));}catch(...){}
            throw std::runtime_error("引用回滚未全部完成，已保留原文件和新副本避免失效；恢复记录："+batch);
        }
        for(auto it=files.rbegin();it!=files.rend();++it) {
            std::error_code ec;const auto destination=impl_->path(it->to);
            if(!it->backup.empty() && fs::exists(it->backup))fs::copy_file(it->backup,destination,fs::copy_options::overwrite_existing,ec);
            else if(!it->existed)fs::remove(destination,ec);
        }
        journal["phase"]="rolled_back";try{writeFile(backupRoot/"journal.json",journal.dump(2));}catch(...){}throw;
    }
    bool journalSaved=true;
    journal["phase"]="committed";try{writeFile(backupRoot/"journal.json",journal.dump(2));}catch(...){journalSaved=false;warnings.push_back("操作已完成，但恢复日志写入失败；原文件暂时保留。 "+batch);}
    if(action!="copy" && journalSaved) {
        for(const auto& file:files) {std::error_code ec;fs::remove(impl_->path(file.from),ec);if(ec)warnings.push_back("原文件清理失败，已保留兼容副本："+file.from);}
        for(auto it=directories.rbegin();it!=directories.rend();++it) {std::error_code ec;fs::remove(impl_->path(it->first),ec);}
    }
    refresh();
    return Json{{"completed",files.size()},{"referencesUpdated",changes.size()},{"warnings",warnings},{"backup",batch}};
}

Json Service::recoverOperations() {
    auto mutation=lockMutation();std::lock_guard lock(impl_->mutex);
    const auto root=impl_->data/"image-operation-backups";auto warnings=Json::array();int recovered=0;
    if(!fs::is_directory(root))return Json{{"recovered",0},{"warnings",warnings}};
    if(!migration_resources::safePath(impl_->data,root))throw std::runtime_error("图片恢复目录不安全");
    std::map<std::string,Document> documents;bool loaded=false;
    for(const auto& entry:fs::directory_iterator(root)) {
        const auto journalPath=entry.path()/"journal.json";
        if(!entry.is_directory() || !fs::is_regular_file(journalPath))continue;
        const auto batch=utf8(entry.path().filename());
        try {
            if(!migration_resources::safePath(root,journalPath))throw std::runtime_error("恢复日志不能是链接");
            auto journal=Json::parse(readFile(journalPath,16ULL*1024*1024));const auto phase=journal.value("phase",std::string());
            if(phase=="committed" || phase=="rolled_back")continue;
            if(phase!="preparing" && phase!="references" && phase!="recovery_required")throw std::runtime_error("未知恢复阶段");
            if(!journal["files"].is_array() || journal["files"].size()>10000 || !journal["changes"].is_array() || journal["changes"].size()>20000)throw std::runtime_error("恢复日志内容无效或过大");
            bool committed=false;
            for(const auto& file:journal["files"]) {
                const auto from=clean(file.at("from").get<std::string>()),to=clean(file.at("to").get<std::string>());
                Statement alias(impl_->db,"SELECT r.path FROM aliases a JOIN resources r ON a.resource_id=r.id WHERE a.alias=?1 AND r.trashed=0");alias.bind(1,"data/images/"+from);
                if(alias.step() && alias.text(0)==to)committed=true;
            }
            if(committed){journal["phase"]="committed";writeFile(journalPath,journal.dump(2));++recovered;continue;}
            // Before rollback, prove that every destination is still the copy we
            // made. A later user edit must never be overwritten by recovery.
            for(const auto& file:journal["files"]) {
                const auto source=impl_->path(file.at("from").get<std::string>()),target=impl_->path(file.at("to").get<std::string>());
                if(!fs::is_regular_file(source) || digest(source)!=file.at("hash").get<std::string>())throw std::runtime_error("原文件已改变，已保留两个位置供确认");
                if(fs::exists(target) && digest(target)!=file.at("hash").get<std::string>())throw std::runtime_error("目标有后续修改，未覆盖");
                if(file.value("existed",false) && fs::exists(target) && !fs::is_regular_file(entry.path()/fromUtf8(file.at("to").get<std::string>())))throw std::runtime_error("缺少覆盖前的备份，未自动恢复");
            }
            if(!loaded){for(auto& doc:impl_->documents())documents.emplace(doc.id,std::move(doc));loaded=true;}
            for(auto it=journal["changes"].rbegin();it!=journal["changes"].rend();++it) {
                auto found=documents.find(it->at("id").get<std::string>());if(found==documents.end())throw std::runtime_error("引用文档已经移除，保留资源等待确认");
                if(found->second.content==it->at("before").get<std::string>())continue;
                if(found->second.content!=it->at("after").get<std::string>() || !impl_->save)throw std::runtime_error("引用文档有后续修改，未覆盖");
                const auto before=it->at("before").get<std::string>();impl_->save({found->second,before});found->second.content=before;
            }
            if(impl_->documentsChanged)impl_->documentsChanged();
            for(const auto& file:journal["files"]) {
                const auto target=impl_->path(file.at("to").get<std::string>());
                if(file.value("existed",false)) {
                    const auto backup=entry.path()/fromUtf8(file.at("to").get<std::string>());
                    if(fs::is_regular_file(backup)){if(!migration_resources::safePath(entry.path(),backup))throw std::runtime_error("恢复备份路径不安全");fs::copy_file(backup,target,fs::copy_options::overwrite_existing);}
                } else if(fs::exists(target))fs::remove(target);
            }
            journal["phase"]="rolled_back";writeFile(journalPath,journal.dump(2));++recovered;
        } catch(const std::exception& error){warnings.push_back("未完成操作 "+batch+"："+error.what());}
    }
    impl_->recoveryWarnings=warnings;return Json{{"recovered",recovered},{"warnings",warnings}};
}

Json Service::migrateExisting(bool force) {
    auto mutation=lockMutation();
    // Copy-first migration: old files remain readable until all known documents
    // have been saved. Unknown plugin IO is never rewritten or executed.
    std::vector<Document> documents;
    {
        std::lock_guard lock(impl_->mutex);
        Statement prior(impl_->db,"SELECT v FROM meta WHERE k='layout_version'");
        if(prior.step() && prior.text(0)=="1" && !force) {impl_->migration=Json{{"status","complete"},{"copied",0},{"warnings",Json::array()}};return impl_->migration;}
        impl_->migration=Json{{"status","running"},{"copied",0},{"warnings",Json::array()}};
        documents=impl_->documents();
    }
    auto warning=[&](const std::string& value) {std::lock_guard lock(impl_->mutex);impl_->migration["warnings"].push_back(value);};
    auto increment=[&] {std::lock_guard lock(impl_->mutex);impl_->migration["copied"]=impl_->migration["copied"].get<int>()+1;};
    const auto work=impl_->data.parent_path();
    const std::vector<std::pair<fs::path,std::string>> oldRoots{
        {impl_->data/"assets","shared"},{impl_->data/"legacy_assets","shared"},{work/"image","shared"},
        {impl_->data/"chat/images","cache/chat"},{impl_->data/"logs/images","cache/log"}
    };
    std::map<std::string,std::string> copied;
    // Preserve complete private image trees, even when paths are generated at run time.
    for(const auto& folder:{"mod","plugin","plugins/js","plugins"}) {
        const auto root=impl_->data/fromUtf8(folder);if(!fs::is_directory(root) || migration_resources::isLink(root))continue;
        for(const auto& entry:fs::directory_iterator(root)) {
            if(!entry.is_directory() || migration_resources::isLink(entry.path()) || (std::string(folder)=="plugins" && entry.path().filename()=="js"))continue;
            const auto result=importOwner(root,entry.path(),std::string(folder)=="mod"?"mod":"plugin",utf8(entry.path().filename()));
            {std::lock_guard lock(impl_->mutex);impl_->migration["copied"]=impl_->migration["copied"].get<int>()+result["copied"].get<int>();}
            for(const auto& item:result["warnings"])warning(item.get<std::string>());
        }
    }
    for(const auto& document:documents) {
        try {
            auto rewritten=rewriteDocument(document,[&](const std::string& ref)->std::optional<std::string> {
                const auto key=referenceKey(ref);if(key.starts_with("data/images/") || key.size()>2048 || key.find_first_of("\r\n{}[]")!=std::string::npos || !imageExtension(fromUtf8(key)))return std::nullopt;
                fs::path source=fromUtf8(key);
                if(source.is_relative()) {
                    const auto adjacent=document.baseDirectory/source;
                    source=!document.baseDirectory.empty() && fs::is_regular_file(adjacent)?adjacent:work/source;
                }
                for(const auto& [root,category]:oldRoots) {
                    if(!fs::exists(root) || !migration_resources::safePath(root,source) || !fs::is_regular_file(source) || !imageExtension(source))continue;
                    auto owned=(document.type=="deck"?"deck":document.type=="reply"?"reply":document.type=="plugin"?"plugin":document.type=="mod"?"mod":"text");
                    const auto owner=document.owner.empty()?document.id:document.owner;
                    const auto destination=std::string(owned)+"/"+clean(owner)+"/"+utf8(source.lexically_relative(root));
                    auto image=importFile(root,source,destination,key);copied[utf8(source)]=image["path"].get<std::string>();
                    increment();
                    return "data/images/"+image["path"].get<std::string>();
                }
                if(auto managed=resolveInDocument(document,ref))return "data/images/"+utf8(managed->lexically_relative(impl_->images));
                return std::nullopt;
            });
            if(rewritten!=document.content && document.writable) {if(!impl_->save)throw std::runtime_error("引用写入服务未就绪");impl_->save({document,rewritten});}
        } catch(const std::exception& e) {warning(document.label+"："+e.what());}
    }
    for(const auto& [root,category]:oldRoots) if(fs::is_directory(root)) {
        migration_resources::walk(root,root,[&](const fs::path& source) {
            if(!imageExtension(source) || copied.count(utf8(source)))return;
            try {
                const auto old="data/"+utf8(source.lexically_relative(impl_->data));
                const auto destination=category+"/"+utf8(source.lexically_relative(root));
                auto image=importFile(root,source,destination,root==work/"image"?"image/"+utf8(source.lexically_relative(root)):old);
                copied[utf8(source)]=image["path"].get<std::string>();increment();
            } catch(const std::exception& e) {warning(utf8(source)+"："+e.what());}
        },[&](const auto& path,const auto& error){warning(utf8(path)+"："+error);});
    }
    // Keep old bytes as a recovery backup. The alias index is authoritative for
    // new sends; no old source deck/plugin is edited or removed here.
    std::lock_guard lock(impl_->mutex);
    if(impl_->documentsChanged)try{impl_->documentsChanged();}catch(const std::exception& error){impl_->migration["warnings"].push_back(std::string("引用缓存刷新失败：")+error.what());}
    if(impl_->migration["warnings"].empty()) {
        Statement mark(impl_->db,"INSERT INTO meta(k,v) VALUES('layout_version','1') ON CONFLICT(k) DO UPDATE SET v=excluded.v");mark.step();
        impl_->migration["status"]="complete";
    } else impl_->migration["status"]="partial";
    refresh();return impl_->migration;
}
void Service::backupCatalog(const fs::path& target,const std::vector<std::string>& included) const {
    std::lock_guard lock(impl_->mutex);fs::create_directories(target.parent_path());sqlite3* output=nullptr;
    if(sqlite3_open(utf8(target).c_str(),&output)!=SQLITE_OK) {if(output)sqlite3_close(output);throw std::runtime_error("无法备份图片资源索引");}
    auto backup=sqlite3_backup_init(output,"main",impl_->db,"main");const int result=backup?sqlite3_backup_step(backup,-1):SQLITE_ERROR;
    if(backup)sqlite3_backup_finish(backup);
    if(result!=SQLITE_DONE){sqlite3_close(output);throw std::runtime_error("图片资源索引备份失败");}
    try {
        if(!included.empty()) {
            std::vector<std::string> remove;Statement rows(output,"SELECT id,path FROM resources");
            while(rows.step()){bool keep=false;for(const auto& prefix:included)if(childOf(rows.text(1),prefix)){keep=true;break;}if(!keep)remove.push_back(rows.text(0));}
            for(const auto& id:remove){Statement erase(output,"DELETE FROM resources WHERE id=?1");erase.bind(1,id);erase.step();}
            exec(output,"DELETE FROM aliases WHERE resource_id NOT IN (SELECT id FROM resources); DELETE FROM refs WHERE resource_id NOT IN (SELECT id FROM resources);");
        }
        sqlite3_close(output);
    }catch(...){sqlite3_close(output);throw;}
}
void mergeCatalogForRestore(const fs::path& staged,const fs::path& existing) {
    if(!fs::exists(staged) || !fs::exists(existing))return;
    if(migration_resources::isLink(staged) || migration_resources::isLink(existing))throw std::runtime_error("资源索引恢复不能跟随链接");
    const auto temporary=staged.parent_path()/fromUtf8(".images-merge-"+randomId()+".db");
    sqlite3 *source=nullptr,*old=nullptr,*merged=nullptr;
    auto open=[](const fs::path& path,sqlite3** db,int flags){if(sqlite3_open_v2(utf8(path).c_str(),db,flags,nullptr)!=SQLITE_OK)throw std::runtime_error("无法打开待合并的资源索引");sqlite3_busy_timeout(*db,3000);exec(*db,"PRAGMA trusted_schema=OFF;");};
    auto close=[&]{if(source){sqlite3_close(source);source=nullptr;}if(old){sqlite3_close(old);old=nullptr;}if(merged){sqlite3_close(merged);merged=nullptr;}};
    try {
        open(staged,&source,SQLITE_OPEN_READONLY);open(existing,&old,SQLITE_OPEN_READONLY);open(temporary,&merged,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
        auto backup=sqlite3_backup_init(merged,"main",old,"main");const auto rc=backup?sqlite3_backup_step(backup,-1):SQLITE_ERROR;if(backup)sqlite3_backup_finish(backup);if(rc!=SQLITE_DONE)throw std::runtime_error("现有资源索引快照失败");
        exec(merged,"PRAGMA journal_mode=DELETE; BEGIN IMMEDIATE;");
        std::map<std::string,std::string> ids;
        {
            Statement rows(source,"SELECT id,path,parent,name,is_dir,bytes,modified,hash,mime,token,trashed,origin FROM resources");unsigned count=0;
            while(rows.step()) {
                if(++count>100000)throw std::runtime_error("资源索引超过局部恢复的 100000 条目限制");
                const auto path=rows.text(1);if(path.starts_with(".retired/"))continue;
                Statement found(merged,"SELECT id FROM resources WHERE path=?1");found.bind(1,path);
                const auto id=found.step()?found.text(0):randomId();ids.emplace(rows.text(0),id);
                Statement put(merged,"INSERT INTO resources(id,path,parent,name,is_dir,bytes,modified,hash,mime,trashed,origin) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT(path) DO UPDATE SET bytes=excluded.bytes,modified=excluded.modified,hash=excluded.hash,mime=excluded.mime,trashed=excluded.trashed,origin=excluded.origin");
                put.bind(1,id);put.bind(2,path);put.bind(3,rows.text(2));put.bind(4,rows.text(3));put.bind(5,rows.integer(4));put.bind(6,rows.integer(5));put.bind(7,rows.integer(6));put.bind(8,rows.text(7));put.bind(9,rows.text(8));put.bind(10,rows.integer(10));put.bind(11,rows.text(11));put.step();
                for(const auto& key:{rows.text(0),"data/images/"+path,rows.text(9).empty()?std::string():"share:"+rows.text(9)})if(!key.empty()) {
                    Statement alias(merged,"INSERT INTO aliases(alias,resource_id) VALUES(?1,?2) ON CONFLICT(alias) DO UPDATE SET resource_id=excluded.resource_id");alias.bind(1,key);alias.bind(2,id);alias.step();
                }
            }
        }
        {
            Statement aliases(source,"SELECT alias,resource_id FROM aliases");unsigned count=0;
            while(aliases.step()) {
                if(++count>1000000)throw std::runtime_error("资源路径别名超过局部恢复限制");
                const auto found=ids.find(aliases.text(1));if(found==ids.end())continue;
                Statement put(merged,"INSERT INTO aliases(alias,resource_id) VALUES(?1,?2) ON CONFLICT(alias) DO UPDATE SET resource_id=excluded.resource_id");put.bind(1,aliases.text(0));put.bind(2,found->second);put.step();
            }
        }
        exec(merged,"COMMIT");close();
#ifdef _WIN32
        if(!MoveFileExW(temporary.c_str(),staged.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("资源索引合并保存失败");
#else
        fs::rename(temporary,staged);
#endif
    } catch(...){close();std::error_code ec;fs::remove(temporary,ec);throw;}
}
void setDefault(std::shared_ptr<Service> service) {std::shared_ptr<Service> old;{std::lock_guard lock(defaultMutex);old=std::exchange(defaultService,std::move(service));}}
std::shared_ptr<Service> current() { std::lock_guard lock(defaultMutex);return defaultService; }
std::string resolvePath(const std::string& reference) { if(auto service=current()) if(auto path=service->resolve(reference)) return utf8(*path);return reference; }
fs::path cacheDirectory(const std::string& kind) {
    if(auto service=current()) {auto path=service->checkedPath("cache/"+kind);fs::create_directories(path);return path;}
    return kind=="log"?fs::path("data/logs/images"):fs::path("data/chat/images");
}
} // namespace dice::image_resources
