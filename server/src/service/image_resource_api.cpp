#include "image_resource_api.h"
#include "image_resource_service.h"
#include "deck_package.h"
#include "../core/deck/deck_file_utils.h"
#include "../storage/legacy_import_resources.h"
#include "game_session_list.h"
#include "../common/logger.h"
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <fstream>
#include <nlohmann/json.hpp>

namespace dice::image_resources {
using Json=nlohmann::json;
namespace {
using Req=const drogon::HttpRequestPtr&;
using Callback=std::function<void(const drogon::HttpResponsePtr&)>;
void reply(Callback&& callback,const Json& data) {
    auto response=drogon::HttpResponse::newHttpResponse();response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(Json{{"code",0},{"message","ok"},{"data",data}}.dump());callback(response);
}
void error(Callback&& callback,const std::exception& error) {
    auto response=drogon::HttpResponse::newHttpResponse();response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    const bool conflict=dynamic_cast<const Conflict*>(&error)!=nullptr;
    if(conflict)response->setStatusCode(drogon::k409Conflict);
    response->setBody(Json{{"code",conflict?409:1},{"message",error.what()}}.dump());callback(response);
}
void sendImage(const std::filesystem::path& path,bool download,Callback&& callback,bool publicAccess=false) {
    auto input=std::make_shared<std::ifstream>(path,std::ios::binary);
    if(!*input || !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path)>128ULL*1024*1024) throw std::runtime_error("图片不存在或超过大小限制");
    char prefix[1024]{};input->read(prefix,sizeof(prefix));const auto mime=mimeFor(std::string_view(prefix,static_cast<std::size_t>(input->gcount())),utf8(path.filename()));
    input->clear();input->seekg(0);
    auto response=drogon::HttpResponse::newStreamResponse([input](char* buffer,std::size_t length)->std::size_t {
        if(!buffer) {input->close();return 0;}input->read(buffer,static_cast<std::streamsize>(length));return static_cast<std::size_t>(input->gcount());
    });
    response->setContentTypeString(mime);response->addHeader("X-Content-Type-Options","nosniff");
    response->addHeader("Content-Security-Policy","sandbox; default-src 'none'; style-src 'unsafe-inline'");
    response->addHeader("Cache-Control",publicAccess?"public, max-age=60":"private, no-store");
    response->addHeader("Content-Disposition",std::string(download?"attachment":"inline")+"; filename*=UTF-8''"+drogon::utils::urlEncode(utf8(path.filename())));
    callback(response);
}
}
void registerRoutes(const std::shared_ptr<Service>& service) {
    auto& app=drogon::app();
    app.registerHandler("/api/resources/images",[service](Req req,Callback&& cb) {
        try {reply(std::move(cb),service->list(req->getParameter("directory"),
            game_sessions::integerParameter(req->getParameter("page"),1),game_sessions::integerParameter(req->getParameter("size"),20),
            req->getParameter("q"),req->getParameter("sort"),req->getParameter("dir")=="desc",req->getParameter("trash")=="1"));}
        catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Get});
    app.registerHandler("/api/resources/images/upload",[service](Req req,Callback&& cb) {
        try {
            const auto directory=req->getParameter("directory").empty()?"shared":req->getParameter("directory");
            auto result=service->upload(directory,req->getParameter("filename"),req->body(),req->getParameter("overwrite")=="1");
            DICE_LOG_INFO_M("文件管理","图片已上传：{}（{} 字节）。",result["path"].get<std::string>(),req->body().size());
            reply(std::move(cb),result);
        }catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    app.registerHandler("/api/resources/images/content",[service](Req req,Callback&& cb) {
        try {const auto path=service->resolve(req->getParameter("id"));if(!path)throw std::runtime_error("图片资源不存在");sendImage(*path,req->getParameter("download")=="1",std::move(cb));}
        catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Get});
    app.registerHandler("/api/resources/images/folder",[service](Req req,Callback&& cb) {
        try {const auto body=Json::parse(req->body());service->mkdir(body.at("path").get<std::string>());DICE_LOG_INFO_M("文件管理","已新建图片文件夹：{}。",body.at("path").get<std::string>());reply(std::move(cb),Json::object());}
        catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    app.registerHandler("/api/resources/images/operation",[service](Req req,Callback&& cb) {
        try {
            const auto body=Json::parse(req->body());const auto action=body.at("action").get<std::string>();
            const auto paths=body.at("paths").get<std::vector<std::string>>();
            auto result=service->operate(action,paths,body.value("target",std::string()),body.value("overwrite",false));
            DICE_LOG_INFO_M("文件管理","资源操作 {} 已完成，共 {} 个所选项。",action,paths.size());reply(std::move(cb),result);
        }catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    app.registerHandler("/api/resources/images/usages",[service](Req req,Callback&& cb) {
        try{reply(std::move(cb),service->usages(req->getParameter("path")));}catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Get});
    app.registerHandler("/api/resources/images/scan",[service](Req,Callback&& cb) {
        try{reply(std::move(cb),service->scanReferences());}catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    app.registerHandler("/api/resources/images/migration",[service](Req req,Callback&& cb) {
        try{if(req->method()==drogon::Post)service->startMigration(true);reply(std::move(cb),service->migrationStatus());}catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Get,drogon::Post});
    app.registerHandler("/api/resources/images/share",[service](Req req,Callback&& cb) {
        try {const auto body=Json::parse(req->body());reply(std::move(cb),Json{{"url",service->share(body.at("path").get<std::string>())}});}
        catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    app.registerHandler("/api/resources/images/archive",[service](Req req,Callback&& cb) {
        try {
            auto archive=exportImages(*service,Json::parse(req->body()).at("paths").get<std::vector<std::string>>());
            auto input=std::make_shared<std::ifstream>(archive.file,std::ios::binary);if(!*input)throw std::runtime_error("压缩包读取失败");
            auto response=drogon::HttpResponse::newStreamResponse([input,temporary=archive.temporary](char* buffer,std::size_t size)->std::size_t{if(!buffer){input->close();return 0;}input->read(buffer,static_cast<std::streamsize>(size));return static_cast<std::size_t>(input->gcount());});
            response->setContentTypeString("application/zip");response->addHeader("Content-Disposition","attachment; filename=images.zip");response->addHeader("Cache-Control","private, no-store");cb(response);
        }catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    app.registerHandler("/api/resources/images/extract",[service](Req req,Callback&& cb) {
        try {reply(std::move(cb),importImages(*service,req->body(),req->getParameter("directory"),req->getParameter("overwrite")=="1"));}
        catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
    // Only an explicitly published, 256-bit opaque token is publicly readable.
    // Neither directory listing nor chat/log caches are exposed by this route.
    app.registerHandler("/media/images/{1}",[service](Req,Callback&& cb,const std::string& token) {
        try {
            const auto path=service->sharedPath(token);
            if(!path){auto response=drogon::HttpResponse::newHttpResponse();response->setStatusCode(drogon::k404NotFound);cb(response);return;}
            sendImage(*path,false,std::move(cb),true);
        }catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Get});
}
void registerDeckPackageRoutes(const std::shared_ptr<Service>& service,std::function<void()> reload,DeckDependencyLookup lookup) {
    drogon::app().registerHandler("/api/decks/export",[service,lookup=std::move(lookup)](Req req,Callback&& cb) {
        try {
            const auto name=req->getParameter("name");if(!deck_files::isSafeJsonFilename(name))throw std::runtime_error("牌堆文件名无效");
            const auto work=service->dataRoot().parent_path();
            const bool builtin=req->getParameter("source")=="builtin";
            auto source=(builtin?work/"decks":service->dataRoot()/"decks")/fromUtf8(name);
            if(!std::filesystem::exists(source) && !builtin)source=work/"decks"/fromUtf8(name);
            if(!std::filesystem::exists(source) && std::filesystem::exists(work.parent_path()/"decks"/fromUtf8(name)))source=work.parent_path()/"decks"/fromUtf8(name);
            if(!migration_resources::safePath(work,source) && !migration_resources::safePath(work.parent_path()/"decks",source))throw std::runtime_error("牌堆路径不安全");
            auto package=exportDeck(*service,source,name,lookup);
            auto input=std::make_shared<std::ifstream>(package.file,std::ios::binary);if(!*input)throw std::runtime_error("打包文件读取失败");
            auto response=drogon::HttpResponse::newStreamResponse([input,temporary=package.temporary](char* buffer,std::size_t size)->std::size_t{if(!buffer){input->close();return 0;}input->read(buffer,static_cast<std::streamsize>(size));return static_cast<std::size_t>(input->gcount());});
            response->setContentTypeString(package.name.ends_with(".deck")?"application/zip":"application/json");
            response->addHeader("Content-Disposition","attachment; filename*=UTF-8''"+drogon::utils::urlEncode(package.name));
            response->addHeader("X-Content-Type-Options","nosniff");response->addHeader("Cache-Control","private, no-store");cb(response);
        }catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Get});
    drogon::app().registerHandler("/api/decks/package",[service,reload=std::move(reload)](Req req,Callback&& cb) {
        try {
            const bool inspect=req->getParameter("inspect")=="1";
            auto result=importDeckPackage(*service,req->body(),req->getParameter("overwrite")=="1",inspect);
            if(!inspect){reload();DICE_LOG_INFO_M("牌堆","牌堆包导入完成：{} 个牌堆文件、{} 张图片。",result["files"].size(),result["images"].get<std::size_t>());}
            reply(std::move(cb),result);
        }catch(const std::exception& e){error(std::move(cb),e);}
    },{drogon::Post});
}
} // namespace dice::image_resources
