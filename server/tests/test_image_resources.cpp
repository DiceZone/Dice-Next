#include "test_framework.h"
#include "../src/service/image_resource_service.h"
#include "../src/service/deck_package.h"
#include <miniz/miniz.h>
#include <miniz/miniz_zip.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dice::image_resources;
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
const std::string png=std::string("\x89PNG\r\n\x1a\n",8)+"fixture";
struct Images {
    fs::path folder;
    std::unique_ptr<Service> service;
    std::vector<Document> documents;
    int saves=0;
    Images() {
        static std::atomic<int> counter{0};
        folder=fs::temp_directory_path()/("dice-next-images-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(counter++));
        service=std::make_unique<Service>(folder/"data");
        service->setDocuments([this]{return documents;},[this](const Change& change) {
            for(auto& document:documents)if(document.id==change.before.id) {
                if(document.content!=change.before.content)throw std::runtime_error("concurrent edit");
                document.content=change.after;++saves;return;
            }
            throw std::runtime_error("document missing");
        });
    }
    ~Images() {service.reset();std::error_code ec;fs::remove_all(folder,ec);}
    Json upload(const std::string& path="reply/测试.png") {const auto p=fromUtf8(path);return service->upload(utf8(p.parent_path()),utf8(p.filename()),png);}
    bool fails(const std::function<void()>& fn) {try{fn();return false;}catch(const std::exception&){return true;}}
};
}

TEST(ImageResources, UploadAndListReturnPathsAndCodesWithoutPreviewPayloads) {
    Images f;const auto resource=f.upload();
    ASSERT_EQ(resource["code"],"[img,file=data/images/reply/测试.png]");
    auto page=f.service->list("reply");ASSERT_EQ(page["total"],1);ASSERT_EQ(page["items"][0]["path"],"reply/测试.png");
    ASSERT_FALSE(page["items"][0].contains("url"));ASSERT_FALSE(page["items"][0].contains("thumbnail"));ASSERT_FALSE(page["items"][0].contains("data"));
    ASSERT_TRUE(f.service->resolve("data/images/reply/测试.png").has_value());
    ASSERT_TRUE(f.service->resolve(resource["id"]).has_value());
    ASSERT_TRUE(f.fails([&]{f.upload();}));ASSERT_TRUE(f.fails([&]{f.service->upload("reply","x.png","not an image");}));
}
TEST(ImageResources, PathsCannotEscapeTheRootOrUseDeviceAndHiddenNames) {
    Images f;
    for(const std::string path:{"../x","/etc/passwd","C:/Windows/x","a/../b",".trash/x","dir/CON.png","a//b","x:stream","dir/name. ","\\\\host\\share"})
        ASSERT_TRUE(f.fails([&]{f.service->mkdir(path);}));
    f.service->mkdir("自定义/子目录");ASSERT_TRUE(fs::is_directory(f.service->checkedPath("自定义/子目录")));
    ASSERT_TRUE(f.fails([&]{f.service->operate("delete",{"reply"});}));
    ASSERT_TRUE(f.fails([&]{f.service->operate("copy",{"自定义"},"自定义/子目录");}));
}
TEST(ImageResources, RenameRewritesDocumentsAndRetainsOldCodesAndSharedLinks) {
    Images f;f.upload("reply/old.png");const auto token=f.service->share("reply/old.png");
    f.documents.push_back({"reply-1","欢迎回复","reply","[img,file=data/images/reply/old.png] {sample:好|棒}","text",true,"rule-1"});
    const auto result=f.service->operate("rename",{"reply/old.png"},"reply/new.png");
    ASSERT_EQ(result["referencesUpdated"],1);ASSERT_EQ(f.documents[0].content,std::string("[img,file=data/images/reply/new.png] {sample:好|棒}"));
    ASSERT_TRUE(f.service->resolve("data/images/reply/old.png").has_value());ASSERT_FALSE(fs::exists(f.service->checkedPath("reply/old.png")));
    ASSERT_EQ(utf8(f.service->sharedPath(token.substr(token.rfind('/')+1))->filename()),std::string("new.png"));
}
TEST(ImageResources, CopyCutPastePreserveDirectoriesAndDoNotScatterOwnerAssets) {
    Images f;f.upload("deck/塔罗.json/cards/正位.png");f.upload("deck/塔罗.json/cards/逆位.png");f.service->mkdir("自定义");
    f.service->operate("copy",{"deck/塔罗.json"},"自定义");
    ASSERT_TRUE(fs::exists(f.service->checkedPath("自定义/塔罗.json/cards/正位.png")));
    ASSERT_TRUE(fs::exists(f.service->checkedPath("deck/塔罗.json/cards/正位.png")));
    f.service->operate("move",{"自定义/塔罗.json"},"reply");
    ASSERT_TRUE(fs::exists(f.service->checkedPath("reply/塔罗.json/cards/逆位.png")));
    ASSERT_FALSE(fs::exists(f.service->checkedPath("自定义/塔罗.json")));
}
TEST(ImageResources, ReferencedImagesCannotBeDeletedAndUnreferencedOnesCanBeRestored) {
    Images f;f.upload();f.documents.push_back({"reply-1","欢迎回复","reply","[图:data/images/reply/测试.png]"});
    ASSERT_EQ(f.service->usages("reply/测试.png")["items"].size(),std::size_t(1));
    ASSERT_TRUE(f.fails([&]{f.service->operate("delete",{"reply/测试.png"});}));
    f.documents.clear();f.service->operate("delete",{"reply/测试.png"});ASSERT_FALSE(f.service->resolve("data/images/reply/测试.png").has_value());
    const auto trash=f.service->list("",1,20,"","name",false,true);ASSERT_EQ(trash["total"],1);
    f.service->operate("restore",{trash["items"][0]["id"]});ASSERT_TRUE(f.service->resolve("data/images/reply/测试.png").has_value());
}
TEST(ImageResources, FailedReferenceSaveRollsBackCopiedBytesAndDoesNotRemoveTheSource) {
    Images f;f.upload("reply/old.png");f.documents.push_back({"reply-1","回复","reply","[图:data/images/reply/old.png]"});
    f.service->setDocuments([&]{return f.documents;},[](const Change&){throw std::runtime_error("synthetic write failure");});
    ASSERT_TRUE(f.fails([&]{f.service->operate("rename",{"reply/old.png"},"reply/new.png");}));
    ASSERT_TRUE(fs::exists(f.service->checkedPath("reply/old.png")));ASSERT_FALSE(fs::exists(f.service->checkedPath("reply/new.png")));
    ASSERT_EQ(f.documents[0].content,std::string("[图:data/images/reply/old.png]"));
}
TEST(ImageResources, MetadataPaginationAndLiteralSearchAreServerSide) {
    Images f;for(int i=0;i<47;++i)f.upload("reply/picture-"+std::to_string(i)+".png");
    auto page=f.service->list("reply",2,20);ASSERT_EQ(page["total"],47);ASSERT_EQ(page["items"].size(),std::size_t(20));
    ASSERT_EQ(f.service->list("reply",999,20)["page"],3);ASSERT_EQ(f.service->list("reply",1,20,"% OR 1=1 --")["total"],0);
    ASSERT_EQ(f.service->list("reply",1,20,"picture-46")["total"],1);
}
TEST(ImageResources, ImageReferencesPreserveWeightsSamplesAndEscapes) {
    Document doc{"deck","牌堆","deck",R"deck({"x":["::2::{sample:好|棒}[CQ:image,file=data/assets/a&#44;b.png]","![图片](data/assets/c.png)"]})deck","json"};
    const auto result=rewriteDocument(doc,[](const std::string& ref)->std::optional<std::string> {
        if(ref=="data/assets/a,b.png")return "data/images/deck/test.json/a,b.png";
        if(ref=="data/assets/c.png")return "data/images/deck/test.json/c.png";return std::nullopt;
    });
    auto value=Json::parse(result);ASSERT_EQ(value["x"][0],"::2::{sample:好|棒}[CQ:image,file=data/images/deck/test.json/a&#44;b.png]");
    ASSERT_EQ(value["x"][1],"![图片](data/images/deck/test.json/c.png)");
    ASSERT_EQ(references("\\[图:data/assets/x.png]").size(),std::size_t(0));
}
TEST(ImageResources, MigrationCollectsDeckAssetsInTheFilenameDirectoryAndIsIdempotent) {
    Images f;fs::create_directories(f.folder/"data/assets");
    {std::ofstream out(f.folder/"data/assets/old.png",std::ios::binary);out<<png;}
    f.documents.push_back({"deck-1","塔罗.json","deck",R"({"x":["[图:data/assets/old.png]"]})","json",true,"塔罗.json"});
    const auto result=f.service->migrateExisting();ASSERT_EQ(result["status"],"complete");
    ASSERT_TRUE(fs::exists(f.service->checkedPath("deck/塔罗.json/old.png")));
    ASSERT_TRUE(f.documents[0].content.find("data/images/deck/塔罗.json/old.png")!=std::string::npos);
    ASSERT_TRUE(fs::exists(f.folder/"data/assets/old.png"));ASSERT_TRUE(f.service->resolve("/api/assets/old.png").has_value());
    ASSERT_EQ(f.service->migrateExisting()["copied"],0);
}
TEST(ImageResources, CacheImagesCannotBePublishedAndLinksCannotBeFollowed) {
    Images f;f.upload("cache/chat/private.png");ASSERT_TRUE(f.fails([&]{f.service->share("cache/chat/private.png");}));
    std::error_code ec;fs::create_directory_symlink(f.folder,f.service->root()/"escape",ec);
    if(!ec)ASSERT_TRUE(f.fails([&]{f.service->mkdir("escape/outside");}));
}
TEST(ImageResources, PluginAndModuleImagesRemainUnderTheirOwner) {
    Images f;const auto root=f.folder/"data/mod"/fromUtf8("自定义规则");fs::create_directories(root/"art/sub");
    {std::ofstream out(root/"art/sub"/fromUtf8("图片.png"),std::ios::binary);out<<png;}
    auto result=f.service->importOwner(f.folder/"data/mod",root,"mod","自定义规则");ASSERT_EQ(result["copied"],1);
    ASSERT_TRUE(fs::exists(f.service->checkedPath("mod/自定义规则/art/sub/图片.png")));
    ASSERT_TRUE(f.service->resolve("data/mod/自定义规则/art/sub/图片.png").has_value());
}
TEST(ImageResources, DeckPackageRoundTripUsesSealDiceLayoutAndKeepsSamplesAndWeights) {
    Images f;f.upload("deck/塔罗.json/cards/a.png");fs::create_directories(f.folder/"data/decks");
    const auto source=f.folder/"data/decks"/fromUtf8("塔罗.json");
    {std::ofstream out(source);out<<R"deck({"塔罗":["::3::{sample:好|棒}[img,file=data/images/deck/塔罗.json/cards/a.png]"],"_title":["测试"]})deck";}
    auto package=exportDeck(*f.service,source,"塔罗.json");ASSERT_EQ(package.name,std::string("塔罗.deck"));
    std::ifstream input(package.file,std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    ZipReader archive(bytes);ASSERT_EQ(archive.entries().size(),std::size_t(2));
    auto text=archive.read(archive.entries()[0]);ASSERT_TRUE(text.find("[CQ:image,file=./assets/cards/a.png]")!=std::string::npos);ASSERT_TRUE(text.find("::3::{sample:好|棒}")!=std::string::npos);
    Images other;const auto result=importDeckPackage(*other.service,bytes);ASSERT_EQ(result["images"],1);
    ASSERT_TRUE(fs::exists(other.service->checkedPath("deck/塔罗.json/assets/cards/a.png")));
    std::ifstream restored(other.folder/"data/decks"/fromUtf8("塔罗.json"));const std::string content{std::istreambuf_iterator<char>(restored),std::istreambuf_iterator<char>()};
    ASSERT_TRUE(content.find("data/images/deck/塔罗.json/assets/cards/a.png")!=std::string::npos);
}
TEST(ImageResources, DeckExportRejectsMissingImagesInsteadOfProducingAnIncompletePackage) {
    Images f;fs::create_directories(f.folder/"data/decks");const auto file=f.folder/"data/decks/missing.json";
    {std::ofstream out(file);out<<R"({"测试":["[图:data/images/deck/missing.json/no.png]"]})";}
    ASSERT_TRUE(f.fails([&]{(void)exportDeck(*f.service,file,"missing.json");}));
}
TEST(ImageResources, ZipNamesRejectTraversalDevicesAndAmbiguousPaths) {
    Images f;for(const auto& name:{"../escape.png","/root/a.png","C:/a.png","a/../../evil.png","NUL.png","a//b.png","a:stream.png"})ASSERT_TRUE(f.fails([&]{(void)safeZipName(name);}));
    ASSERT_EQ(safeZipName("./assets/图片.png"),std::string("assets/图片.png"));
}
namespace {
std::string fixtureZip(const std::vector<std::pair<std::string,std::string>>& entries) {
    mz_zip_archive zip{};if(!mz_zip_writer_init_heap(&zip,0,0))throw std::runtime_error("fixture zip");
    std::vector<std::pair<std::string,std::string>> names;unsigned i=0;
    for(const auto& [name,bytes]:entries) {
        // miniz itself refuses unsafe names when writing. Patch equally sized
        // local/central names afterward to model an independently crafted ZIP.
        const std::string safe(name.size(),static_cast<char>('a'+i++));names.emplace_back(safe,name);
        if(!mz_zip_writer_add_mem(&zip,safe.c_str(),bytes.data(),bytes.size(),0)){mz_zip_writer_end(&zip);throw std::runtime_error("fixture entry");}
    }
    void* raw=nullptr;size_t length=0;if(!mz_zip_writer_finalize_heap_archive(&zip,&raw,&length)){mz_zip_writer_end(&zip);throw std::runtime_error("fixture finish");}
    std::string bytes(static_cast<char*>(raw),length);mz_free(raw);mz_zip_writer_end(&zip);
    for(const auto& [safe,name]:names)for(std::size_t at=0;(at=bytes.find(safe,at))!=std::string::npos;at+=name.size())bytes.replace(at,safe.size(),name);
    return bytes;
}
}
TEST(ImageResources, ImageZipRoundTripPreservesFoldersAndRejectsMixedContentBeforeWriting) {
    Images f;f.upload("plugin/example/art/晴天.png");const auto archive=exportImages(*f.service,{"plugin/example"});
    std::ifstream input(archive.file,std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    Images target;ASSERT_EQ(importImages(*target.service,bytes,"shared")["completed"],1);
    ASSERT_TRUE(fs::exists(target.service->checkedPath("shared/example/art/晴天.png")));
    const auto invalid=fixtureZip({{"safe.png",png},{"script.lua","os.execute('must not run')"}});
    ASSERT_TRUE(target.fails([&]{(void)importImages(*target.service,invalid,"reply");}));
    ASSERT_EQ(target.service->list("reply")["total"],0);
}
TEST(ImageResources, ZipReaderRejectsTraversalDuplicatesCorruptionAndEncryptedEntries) {
    Images f;
    for(const auto& path:{"../outside.png","/absolute.png","C:/outside.png","nested/../../outside.png"}) {
        const auto bytes=fixtureZip({{path,png}});ASSERT_TRUE(f.fails([&]{ZipReader reader(bytes);}));
    }
    const auto duplicate=fixtureZip({{"a.png",png},{"A.png",png}});ASSERT_TRUE(f.fails([&]{ZipReader reader(duplicate);}));
    auto damaged=fixtureZip({{"a.png",png}});const auto start=damaged.find(png);ASSERT_TRUE(start!=std::string::npos);damaged[start+10]^=0x40;
    ASSERT_TRUE(f.fails([&]{(void)importImages(*f.service,damaged,"reply");}));ASSERT_EQ(f.service->list("reply")["total"],0);
    auto encrypted=fixtureZip({{"a.png",png}});const auto central=encrypted.find(std::string("PK\1\2",4));ASSERT_TRUE(central!=std::string::npos);encrypted[central+8]|=1;
    ASSERT_TRUE(f.fails([&]{ZipReader reader(encrypted);}));
}
TEST(ImageResources, PluginRelativeReferencesAreIndexedAndRenamedWithoutExecutingCode) {
    Images f;const auto privateDir=f.folder/"data/plugin/example";fs::create_directories(privateDir/"art");
    {std::ofstream out(privateDir/"art/a.png",std::ios::binary);out<<png;}
    f.service->importOwner(f.folder/"data/plugin",privateDir,"plugin","example");
    Document document{"plugin-config","插件设置","plugin",R"({"image":"art/a.png"})","json",true,"example",privateDir};f.documents.push_back(document);
    ASSERT_EQ(f.service->usages("plugin/example/art/a.png")["items"].size(),std::size_t(1));
    f.service->operate("rename",{"plugin/example/art/a.png"},"plugin/example/art/new.png");
    ASSERT_TRUE(f.documents[0].content.find("data/images/plugin/example/art/new.png")!=std::string::npos);
}
TEST(ImageResources, RestartRetainsAliasesAndCacheUrlsAfterRename) {
    Images f;f.upload("cache/chat/1700_pic.png");
    f.service->operate("rename",{"cache/chat/1700_pic.png"},"cache/chat/1700_new.png");
    f.service.reset();f.service=std::make_unique<Service>(f.folder/"data");
    ASSERT_TRUE(f.service->resolve("/api/chat/images/1700_pic.png").has_value());
    ASSERT_TRUE(f.service->resolve("data/images/cache/chat/1700_pic.png").has_value());
}
TEST(ImageResources, InterruptedMoveRecoveryRestoresKnownDocumentsAndKeepsUnknownChanges) {
    Images f;f.upload("reply/old.png");f.upload("reply/new.png");
    f.documents.push_back({"r","回复","reply","[图:data/images/reply/new.png]"});
    // A journal with a deliberately unrecognized digest cannot authorize removal
    // of either file. Recovery reports it and preserves both versions.
    const auto folder=f.folder/"data/image-operation-backups/interrupted";fs::create_directories(folder);
    Json journal{{"action","rename"},{"phase","references"},{"files",Json::array({{{"from","reply/old.png"},{"to","reply/new.png"},{"existed",false},{"hash","not-the-current-file"}}})},{"changes",Json::array({{{"id","r"},{"before","[图:data/images/reply/old.png]"},{"after",f.documents[0].content}}})}};
    {std::ofstream out(folder/"journal.json");out<<journal.dump();}
    const auto result=f.service->recoverOperations();ASSERT_EQ(result["warnings"].size(),std::size_t(1));ASSERT_TRUE(fs::exists(f.service->checkedPath("reply/old.png")));ASSERT_TRUE(fs::exists(f.service->checkedPath("reply/new.png")));
    ASSERT_TRUE(f.documents[0].content.find("reply/new.png")!=std::string::npos);
}
TEST(ImageResources, KnownCrossFileDeckDependenciesAreIncludedWithoutDrawingOrEvaluating) {
    Images f;fs::create_directories(f.folder/"data/decks");
    const auto main=f.folder/"data/decks/main.json",child=f.folder/"data/decks/child.json";
    {std::ofstream out(main);out<<R"({"main":["{%child}","{sample:{child}|{nick}}"]})";}
    {std::ofstream out(child);out<<R"({"child":["[1d100] 保留骰子表达式"]})";}
    unsigned calls=0;auto result=exportDeck(*f.service,main,"main.json",[&](const std::string& name)->std::optional<fs::path>{++calls;if(name=="child")return child;return std::nullopt;});
    std::ifstream input(result.file,std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    ZipReader zip(bytes);ASSERT_EQ(result.name,std::string("main.deck"));ASSERT_EQ(zip.entries().size(),std::size_t(2));ASSERT_TRUE(calls>0);
    ASSERT_TRUE(zip.read(zip.entries()[1]).find("[1d100]")!=std::string::npos);
}
TEST(ImageResources, PartialCatalogRestoreMergesUnselectedAliasesAndPublicTokens) {
    Images f;f.upload("reply/a.png");f.upload("plugin/example/b.png");
    f.service->addAlias("data/assets/a.png","reply/a.png");f.service->addAlias("data/plugin/example/b.png","plugin/example/b.png");
    const auto aToken=f.service->share("reply/a.png"),bToken=f.service->share("plugin/example/b.png");
    const auto staged=f.folder/"selected-images.db";f.service->backupCatalog(staged,{"reply"});
    // A newer unselected resource must survive restoring the reply-only index.
    f.upload("plugin/example/new.png");const auto newToken=f.service->share("plugin/example/new.png");f.service.reset();
    mergeCatalogForRestore(staged,f.folder/"data/image-resources.db");
    fs::copy_file(staged,f.folder/"data/image-resources.db",fs::copy_options::overwrite_existing);
    f.service=std::make_unique<Service>(f.folder/"data");
    ASSERT_TRUE(f.service->resolve("data/assets/a.png").has_value());ASSERT_TRUE(f.service->resolve("data/plugin/example/b.png").has_value());
    for(const auto& token:{aToken,bToken,newToken})ASSERT_TRUE(f.service->sharedPath(token.substr(token.rfind('/')+1)).has_value());
}
TEST(ImageResources, SameFileOrCaseAliasedOverwriteCannotRemoveTheSource) {
    Images f;f.upload("reply/original.png");
#ifdef _WIN32
    ASSERT_TRUE(f.fails([&]{f.service->operate("rename",{"reply/original.png"},"reply/ORIGINAL.png",true);}));
    ASSERT_TRUE(fs::exists(f.service->checkedPath("reply/original.png")));
    f.service->mkdir("shared/Folder");f.upload("shared/Folder/a.png");
    ASSERT_TRUE(f.fails([&]{f.service->operate("move",{"shared/Folder"},"shared/FOLDER",true);}));
#endif
}
TEST(ImageResources, LargeBracketInputsAndYamlAliasCyclesRemainBounded) {
    Images f;const auto text=std::string(100000,'[')+"[img,file=data/images/reply/a.png]";
    ASSERT_EQ(references(text).size(),std::size_t(1));
    Document document{"yaml","插件 YAML","plugin","a: &a [*a]","yaml"};
    ASSERT_TRUE(f.fails([&]{(void)documentReferences(document);}));
}
TEST(ImageResources, RecycledAliasesRemainKnownAndCannotBecomeLegacyFallbacks) {
    Images f;f.upload("reply/old.png");f.service->addAlias("data/assets/old.png","reply/old.png");
    f.service->operate("delete",{"reply/old.png"});
    ASSERT_TRUE(f.service->known("/api/assets/old.png"));ASSERT_FALSE(f.service->resolve("/api/assets/old.png").has_value());
    ASSERT_TRUE(f.service->known("data/images/reply/old.png"));
    const auto trash=f.service->list("",1,20,"","name",false,true);f.service->operate("restore",{trash["items"][0]["id"]});
    ASSERT_TRUE(f.service->resolve("data/assets/old.png").has_value());
    f.service->operate("delete",{"reply/old.png"});
    const auto deleted=f.service->list("",1,20,"","name",false,true);f.service->operate("purge",{deleted["items"][0]["id"]});
    ASSERT_TRUE(f.service->known("data/images/reply/old.png"));ASSERT_TRUE(f.service->known("/api/assets/old.png"));
    ASSERT_FALSE(f.service->resolve("data/images/reply/old.png").has_value());
}
TEST(ImageResources, UnmigratedCacheUrlsAreNotMistakenForRecycledResources) {
    Images f;
    ASSERT_FALSE(f.service->known("/api/chat/images/old.png"));
    ASSERT_FALSE(f.service->known("/api/logs/images/old.png"));
    ASSERT_FALSE(f.service->known("data/images/cache/chat/old.png"));
    ASSERT_TRUE(f.service->known("data/images/../../private.png"));
    f.upload("cache/chat/old.png");
    ASSERT_TRUE(f.service->known("/api/chat/images/old.png"));
    ASSERT_TRUE(f.service->resolve("/api/chat/images/old.png").has_value());
    f.service->operate("delete",{"cache/chat/old.png"});
    ASSERT_TRUE(f.service->known("/api/chat/images/old.png"));
    ASSERT_FALSE(f.service->resolve("/api/chat/images/old.png").has_value());
}
