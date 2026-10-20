#include "deck_package.h"
#include "../core/deck/deck_file_utils.h"
#include "../storage/legacy_import_resources.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <set>

namespace dice::image_resources {
using Json=nlohmann::json;namespace fs=std::filesystem;
namespace {
bool remote(const std::string& value){return value.starts_with("http://") || value.starts_with("https://") || value.starts_with("base64://") || value.starts_with("data:");}
std::string readFile(const fs::path& path) {if(!fs::is_regular_file(path) || fs::file_size(path)>16ULL*1024*1024)throw std::runtime_error("牌堆文档不存在或超过 16 MiB");std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("无法读取牌堆");return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
void writeFile(const fs::path& path,const std::string& content){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);if(!out || !out.write(content.data(),static_cast<std::streamsize>(content.size())))throw std::runtime_error("牌堆暂存失败");out.close();if(!out)throw std::runtime_error("牌堆暂存保存失败");}
void validate(const Json& doc){if(!doc.is_object() || doc.empty())throw std::runtime_error("牌堆必须是非空 JSON 对象");bool group=false;for(auto it=doc.begin();it!=doc.end();++it)if(it.value().is_array()){for(const auto& item:it.value())if(!item.is_string())throw std::runtime_error("牌堆条目必须为字符串");group=true;}if(!group)throw std::runtime_error("没有找到牌堆条目");}
void sealCodes(Json& value) {
    if(value.is_string()) {auto text=value.get<std::string>();std::size_t at=0;while((at=text.find("[img,",at))!=std::string::npos){if(at && text[at-1]=='\\'){at+=5;continue;}text.replace(at,5,"[CQ:image,");at+=10;}value=std::move(text);}
    else if(value.is_array() || value.is_object())for(auto& child:value)sealCodes(child);
}
void deckTokens(const Json& value,std::set<std::string>& tokens) {
    if(value.is_string()) {
        const auto text=value.get<std::string>();std::vector<std::size_t> stack;
        for(std::size_t i=0;i<text.size();++i) {
            if(text[i]=='\\'){++i;continue;}
            if(text[i]=='{')stack.push_back(i);
            else if(text[i]=='}' && !stack.empty()) {
                const auto start=stack.back();stack.pop_back();auto name=text.substr(start+1,i-start-1);
                if(name.starts_with('%'))name.erase(0,1);if(name.starts_with("draw:"))name.erase(0,5);
                if(!name.empty() && name.find_first_of(":{}\r\n")==std::string::npos)tokens.insert(name);
            }
        }
    } else if(value.is_array() || value.is_object())for(const auto& child:value)deckTokens(child,tokens);
}
}
DeckDownload exportDeck(Service& service,const fs::path& source,const std::string& filename,DeckDependencyLookup lookup) {
    auto mutation=service.lockMutation();
    if(!deck_files::isSafeJsonFilename(filename))throw std::runtime_error("牌堆文件名无效");
    auto primary=Json::parse(readFile(source));validate(primary);
    struct Source {std::string name;fs::path path;Json document;};std::vector<Source> documents{{filename,source,std::move(primary)}};
    std::map<std::string,fs::path> visited{{filename,fs::absolute(source).lexically_normal()}};
    std::uint64_t documentBytes=0;
    for(std::size_t i=0;i<documents.size() && lookup;++i) {
        std::set<std::string> tokens;deckTokens(documents[i].document,tokens);
        for(const auto& token:tokens) {
            if(documents[i].document.contains(token))continue;
            const auto dependency=lookup(token);if(!dependency)continue;
            const auto name=utf8(dependency->filename());const auto path=fs::absolute(*dependency).lexically_normal();
            if(!deck_files::isSafeJsonFilename(name))throw std::runtime_error("依赖牌堆文件名无效");
            if(const auto found=visited.find(name);found!=visited.end()){if(found->second!=path)throw std::runtime_error("两个依赖使用相同文件名，请先重命名："+name);continue;}
            if(documents.size()>=100)throw std::runtime_error("牌堆依赖超过 100 个文件，请拆分导出");
            auto content=readFile(path);documentBytes+=content.size();if(documentBytes>64ULL*1024*1024)throw std::runtime_error("牌堆依赖文档合计超过 64 MiB");
            auto document=Json::parse(content);validate(document);visited.emplace(name,path);documents.push_back({name,path,std::move(document)});
        }
    }
    const auto work=service.dataRoot().parent_path();
    std::map<std::string,std::string> packed;std::set<std::string> archiveNames;std::vector<ZipSource> files;
    std::vector<ZipSource> jsonFiles;
    for(auto& document:documents) {
    auto transformed=rewriteDocument({"export",document.name,"deck",document.document.dump(),"json"},[&](const std::string& reference)->std::optional<std::string>{
        auto resolved=service.resolve(reference);
        if(!resolved && (remote(reference) || reference.find('{')!=std::string::npos))return std::nullopt;
        if(!resolved) {
            auto relative=fromUtf8(reference.starts_with("file://")?reference.substr(7):reference);
            for(const auto& candidate:{document.path.parent_path()/relative,work/relative}) {
                if(!imageExtension(candidate) || !fs::is_regular_file(candidate))continue;
                for(const auto& root:{document.path.parent_path(),service.dataRoot()/"assets",service.dataRoot()/"legacy_assets",work/"image"})if(migration_resources::safePath(root,candidate)){resolved=candidate;break;}
                if(resolved)break;
            }
        }
        if(!resolved) {
            if(imageExtension(fromUtf8(reference)) && reference.find_first_of("\n\r") == std::string::npos)throw std::runtime_error("牌堆引用的本地图片缺失，未导出不完整包："+reference);
            return std::nullopt;
        }
        const auto key=utf8(fs::absolute(*resolved).lexically_normal());if(auto prior=packed.find(key);prior!=packed.end())return "./"+prior->second;
        const auto owner=service.checkedPath("deck/"+document.name);
        std::string relative=migration_resources::within(owner,*resolved)?utf8(resolved->lexically_relative(owner)):utf8(resolved->filename());
        while(relative.starts_with("assets/"))relative.erase(0,7);
        auto name=safeZipName("assets/"+(document.name==filename?std::string():document.name+"/")+relative);
        if(!archiveNames.insert(name).second){name="assets/"+migration_resources::hash(key)+"/"+utf8(resolved->filename());if(!archiveNames.insert(name).second)throw std::runtime_error("牌堆资源名冲突");}
        packed.emplace(key,name);files.push_back({name,*resolved,{}});return "./"+name;
    });
    auto doc=Json::parse(transformed);sealCodes(doc);jsonFiles.push_back({document.name,{},doc.dump(2)});
    }
    auto temp=std::make_shared<TemporaryDirectory>(service.dataRoot()/"image-tmp");
    if(files.empty() && jsonFiles.size()==1){const auto path=temp->path/fromUtf8(filename);writeFile(path,jsonFiles.front().bytes);return {temp,path,filename};}
    files.insert(files.begin(),jsonFiles.begin(),jsonFiles.end());
    const auto name=filename.substr(0,filename.size()-5)+".deck";const auto path=temp->path/fromUtf8(name);writeZip(path,files);return {temp,path,name};
}
Json importDeckPackage(Service& service,std::string_view bytes,bool overwrite,bool inspect) {
    auto mutation=service.lockMutation();
    ZipReader zip(bytes);std::map<std::string,const ZipEntry*> images;std::vector<std::pair<std::string,Json>> decks;auto warnings=Json::array();
    for(const auto& entry:zip.entries()) {
        if(entry.directory)continue;
        if(imageExtension(fromUtf8(entry.name))){images.emplace(entry.name,&entry);continue;}
        if(entry.name.find('/')==std::string::npos && deck_files::isSafeJsonFilename(entry.name)){auto doc=Json::parse(zip.read(entry,16ULL*1024*1024));validate(doc);decks.emplace_back(entry.name,std::move(doc));continue;}
        // SealDice does not require info.yaml; when present it is inert metadata.
        if(entry.name=="info.yaml" || entry.name=="README.md" || entry.name=="LICENSE")continue;
        throw std::runtime_error("牌堆包含不支持的文件，未解压："+entry.name);
    }
    if(decks.empty() || decks.size()>100)throw std::runtime_error(".deck 需要在 ZIP 根目录放置 1–100 个 JSON 牌堆，不能套额外目录");
    Json names=Json::array();for(const auto& [name,doc]:decks)names.push_back(name);
    const auto directory=service.dataRoot()/"decks";
    if(!migration_resources::safePath(service.dataRoot(),directory))throw std::runtime_error("牌堆目标目录不安全");
    for(const auto& [name,doc]:decks)if(fs::exists(directory/fromUtf8(name)) && !overwrite && !inspect)throw Conflict("已有同名牌堆，请确认替换："+name);
    // Validate all references and CRCs before publishing files or changing decks.
    auto temp=std::make_shared<TemporaryDirectory>(service.dataRoot()/"image-tmp");
    std::set<std::string> used;std::map<std::string,std::string> rewritten;std::map<std::string,std::set<std::string>> ownership;
    for(const auto& [name,doc]:decks) {
        const auto text=rewriteDocument({"import",name,"deck",doc.dump(),"json"},[&](const std::string& reference)->std::optional<std::string>{
            if(remote(reference) || reference.find('{')!=std::string::npos || !imageExtension(fromUtf8(reference)))return std::nullopt;
            const auto asset=safeZipName(reference);if(!images.count(asset))throw std::runtime_error("牌堆包缺少引用的图片："+reference);
            used.insert(asset);ownership[name].insert(asset);return "data/images/deck/"+name+"/"+asset;
        });
        rewritten.emplace(name,text);
    }
    for(const auto& [name,entry]:images){auto content=zip.read(*entry);(void)mimeFor(content,name);writeFile(temp->path/"assets"/fromUtf8(name),content);}
    if(used.size()!=images.size()) {
        warnings.push_back("未被静态引用的图片归入首个牌堆的资源目录；动态拼接的图片路径请核对后使用。");
        for(const auto& [asset,entry]:images)if(!used.count(asset))ownership[decks.front().first].insert(asset);
    }
    std::uint64_t copiedBytes=0;std::size_t copiedFiles=0;
    for(const auto& [name,assets]:ownership)for(const auto& asset:assets){copiedBytes+=images.at(asset)->size;if(++copiedFiles>10000 || copiedBytes>512ULL*1024*1024)throw std::runtime_error("按牌堆归档后的图片合计超过 10000 个文件或 512 MiB，请拆分导入");}
    if(inspect)return Json{{"files",names},{"images",images.size()},{"warnings",warnings}};
    fs::create_directories(directory);
    struct Asset {fs::path target,backup;bool existed=false;};std::vector<Asset> assets;std::vector<std::pair<fs::path,fs::path>> installed;
    try {
        // Every deck owns its complete image tree, rather than a global hash pool.
        for(const auto& [name,owned]:ownership)for(const auto& asset:owned) {
            const auto target=service.checkedPath("deck/"+name+"/"+asset);
            if(fs::exists(target) && !overwrite)throw Conflict("已有同名图片资源，请确认替换："+asset);
            Asset record{target,{},fs::exists(target)};
            if(record.existed){record.backup=temp->path/"backup-images"/fromUtf8(name)/fromUtf8(asset);fs::create_directories(record.backup.parent_path());fs::copy_file(target,record.backup);}
            assets.push_back(record);fs::create_directories(target.parent_path());fs::copy_file(temp->path/"assets"/fromUtf8(asset),target,fs::copy_options::overwrite_existing);
        }
        for(const auto& [name,text]:rewritten) {
            auto target=directory/fromUtf8(name), backup=temp->path/"backup-decks"/fromUtf8(name);
            if(fs::exists(target)){fs::create_directories(backup.parent_path());fs::rename(target,backup);}else backup.clear();
            installed.emplace_back(target,backup);writeFile(temp->path/fromUtf8(name),text);fs::rename(temp->path/fromUtf8(name),target);
        }
        service.refresh();
    } catch(...) {
        for(auto it=installed.rbegin();it!=installed.rend();++it){std::error_code ec;fs::remove(it->first,ec);if(!it->second.empty())fs::rename(it->second,it->first,ec);}
        for(auto it=assets.rbegin();it!=assets.rend();++it){std::error_code ec;if(it->existed)fs::copy_file(it->backup,it->target,fs::copy_options::overwrite_existing,ec);else fs::remove(it->target,ec);}
        throw;
    }
    return Json{{"files",names},{"images",images.size()},{"warnings",warnings}};
}
}
