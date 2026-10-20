#pragma once
#include "image_resource_service.h"
#include "../storage/database.h"
#include "../storage/legacy_import_resources.h"
#include "../config/config_manager.h"
#include "../core/reply/reply_manager.h"
#include "../core/persona/persona_manager.h"
#include "../core/deck/card_deck.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>

namespace dice::image_resources {
namespace document_source_detail {
using Json=nlohmann::json;
namespace fs=std::filesystem;
struct Db {
    sqlite3* raw=nullptr;
    explicit Db(const fs::path& path) {
        if(sqlite3_open_v2(utf8(path).c_str(),&raw,SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
            if(raw)sqlite3_close(raw);raw=nullptr;throw std::runtime_error("无法读取资源引用数据库");
        }
        sqlite3_busy_timeout(raw,3000);
    }
    ~Db(){if(raw)sqlite3_close(raw);}
};
struct Stmt {
    sqlite3_stmt* raw=nullptr;
    Stmt(sqlite3* db,const std::string& sql){if(sqlite3_prepare_v2(db,sql.c_str(),-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
    ~Stmt(){sqlite3_finalize(raw);}
    void bind(int n,const std::string& text){sqlite3_bind_text(raw,n,text.data(),static_cast<int>(text.size()),SQLITE_TRANSIENT);}
    bool step(){const auto rc=sqlite3_step(raw);if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(raw)));return rc==SQLITE_ROW;}
    std::string text(int n)const{const auto* p=sqlite3_column_text(raw,n);return p?std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(raw,n)):"";}
};
struct Table {std::string name,type;std::vector<std::string> keys,columns;};
inline const std::vector<Table>& mainTables(){static const std::vector<Table> list{
    {"reply_rules","reply",{"id"},{"reply_content","results","cooldown_notice","day_limit_notice"}},
    {"i18n_overrides","text",{"id"},{"value"}}, {"persona_entries","text",{"id"},{"value"}},
    {"group_settings","text",{"id"},{"value"}}, {"group_account_settings","text",{"id"},{"value"}},
    {"scheduled_tasks","text",{"id"},{"content"}}, {"decks","deck",{"id"},{"cards"}},
    {"causal_rules","reply",{"id"},{"actions"}}
};return list;}
inline std::vector<Table> sourceTables(const std::string& source) {
    if(source=="main")return mainTables();
    if(source=="lua")return {{"lua_conf","plugin",{"scope","k"},{"v"}}};
    if(source=="js")return {{"js_kv","plugin",{"k"},{"v"}}};
    if(source=="log")return {{"game_log_messages","log",{"id"},{"content","images"}}};
    if(source=="chat")return {{"chat_messages","chat",{"id"},{"content"}}};
    return {};
}
inline std::string read(const fs::path& path) {
    if(!fs::is_regular_file(path) || fs::file_size(path)>16ULL*1024*1024)throw std::runtime_error("引用文档不存在或超过 16 MiB");
    std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("无法读取引用文档");return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
inline std::string format(const std::string& text) {return Json::parse(text,nullptr,false).is_discarded()?"text":"json";}
inline std::string safeOwner(std::string text) {
    for(auto& c:text)if(static_cast<unsigned char>(c)<32 || std::string("/\\:*?\"<>|").find(c)!=std::string::npos)c='_';
    if(text.empty() || text.front()=='.')text="resource-"+migration_resources::hash(text);return text;
}
inline void scanDatabase(std::vector<Document>& out,const fs::path& path,const std::string& source,const std::vector<Table>& tables) {
    if(!fs::exists(path))return;
    Db db(path);
    for(const auto& table:tables) {
        std::set<std::string> available;Stmt schema(db.raw,"PRAGMA table_info("+table.name+")");while(schema.step())available.insert(schema.text(1));
        for(const auto& column:table.columns) {
            if(!available.count(column))continue;
            std::string fields;for(const auto& key:table.keys)fields+=key+",";
            std::string sql="SELECT "+fields+column+" FROM "+table.name+" WHERE "+column+" IS NOT NULL AND "+column+"<>''";
            // Historical cache filenames keep their own retention lifecycle. Only
            // permanent managed-resource references need to be rewritten here.
            if(source=="log" || source=="chat")sql+=" AND (instr("+column+",'data/images/')>0 OR instr("+column+",'data/assets/')>0 OR instr("+column+",'/api/assets/')>0)";
            Stmt rows(db.raw,sql);
            while(rows.step()) {
                auto keys=Json::array();std::string label=table.name;
                for(std::size_t i=0;i<table.keys.size();++i){keys.push_back(rows.text(static_cast<int>(i)));label+="/"+rows.text(static_cast<int>(i));}
                const auto content=rows.text(static_cast<int>(table.keys.size()));
                const auto id=Json{{"source",source},{"table",table.name},{"column",column},{"keys",keys}}.dump();
                out.push_back({id,label+"/"+column,table.type,content,format(content),true,safeOwner(label)});
            }
        }
    }
}
inline void writeAtomic(const fs::path& path,const std::string& bytes) {
    const auto temporary=path.parent_path()/(path.filename().native()+fromUtf8(".image-update-"+migration_resources::hash(bytes)+".tmp").native());
    {std::ofstream output(temporary,std::ios::binary);if(!output || !output.write(bytes.data(),static_cast<std::streamsize>(bytes.size())))throw std::runtime_error("引用文档写入失败");output.close();if(!output)throw std::runtime_error("引用文档保存失败");}
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {std::error_code ec;fs::remove(temporary,ec);throw std::runtime_error("引用文档原子替换失败");}
#else
    fs::rename(temporary,path);
#endif
}
}

// This adapter only reads/writes declared data fields. It never executes plugins,
// templates or expressions while discovering references.
inline void configureDocuments(const std::shared_ptr<Service>& service,Database& db,ConfigManager& cfg,
                               I18n& i18n,PersonaManager& personas,ReplyManager& replies,CardDeck& decks) {
    using namespace document_source_detail;
    const auto work=service->dataRoot().parent_path(), data=service->dataRoot();
    struct PendingRefresh {bool replies=false,decks=false;std::set<int> personas;};
    auto pending=std::make_shared<PendingRefresh>();
    auto scan=[&db,&cfg,work,data] {
        std::vector<Document> out;
        out.push_back({"config","系统设置","text",cfg.getAll().dump(),"json",true,"settings"});
        scanDatabase(out,fromUtf8(db.path()),"main",mainTables());
        scanDatabase(out,data/"lua_mod.db","lua",{{"lua_conf","plugin",{"scope","k"},{"v"}}});
        scanDatabase(out,data/"plugins.db","js",{{"js_kv","plugin",{"k"},{"v"}}});
        scanDatabase(out,fromUtf8(db.logPath()),"log",sourceTables("log"));
        scanDatabase(out,fromUtf8(db.chatPath()),"chat",sourceTables("chat"));
        for(const auto& folder:{"decks","help","helpdoc","mod","plugin","plugins","rules","rulepacks","card-templates","self_data"}) {
            const auto root=data/folder;if(!fs::is_directory(root) || migration_resources::isLink(root))continue;
            migration_resources::walk(root,root,[&](const fs::path& file) {
                const auto ext=migration_resources::lowerExtension(file);
                if(!std::set<std::string>{".json",".yaml",".yml",".toml",".md",".txt",".xml",".lua",".js",".mjs"}.count(ext))return;
                const auto relative=utf8(file.lexically_relative(work));
                const bool code=ext==".lua" || ext==".js" || ext==".mjs";
                const std::string type=std::string(folder)=="decks"?"deck":std::string(folder)=="mod"?"mod":std::string(folder)=="plugin" || std::string(folder)=="plugins"?"plugin":"text";
                const auto owned=file.lexically_relative(root);auto first=owned.begin();if(std::string(folder)=="plugins" && *first=="js")++first;
                const auto part=first==owned.end()?file.filename():*first;
                const auto owner=type=="deck"?utf8(file.filename()):type=="mod" || type=="plugin"?utf8(part==file.filename()?part.stem():part):std::string(folder);
                auto document=Document{"file:"+relative,relative,type,read(file),ext==".json"?"json":ext==".yaml" || ext==".yml"?"yaml":"text",!code,safeOwner(owner)};
                document.baseDirectory=file.parent_path();out.push_back(std::move(document));
            },[&](const auto& file,const auto& reason){throw std::runtime_error("引用文档扫描失败："+utf8(file)+"："+reason);});
        }
        return out;
    };
    service->setDocuments(std::move(scan),[&db,&cfg,&i18n,pending,work,data](const Change& change) {
        if(!change.before.writable)throw std::runtime_error("只读插件脚本不允许自动改写");
        if(change.before.id=="config") {
            const auto before=Json::parse(change.before.content), after=Json::parse(change.after);
            if(!cfg.restoreSnapshot(after,&before))throw std::runtime_error("设置已被修改，未覆盖新设置");
            if(!cfg.save()){(void)cfg.restoreSnapshot(before,&after);throw std::runtime_error("设置保存失败");}
            return;
        }
        if(change.before.id.starts_with("file:")) {
            const auto relative=change.before.id.substr(5);const auto path=work/fromUtf8(relative);
            if(!migration_resources::safePath(data,path) || read(path)!=change.before.content)throw std::runtime_error("资源文档已变更或路径不安全，未覆盖");
            writeAtomic(path,change.after);
            if(change.before.type=="deck")pending->decks=true;
            return;
        }
        const auto identity=Json::parse(change.before.id);const auto source=identity.at("source").get<std::string>();
        const auto table=identity.at("table").get<std::string>(), column=identity.at("column").get<std::string>();
        const auto list=sourceTables(source);
        const auto entry=std::find_if(list.begin(),list.end(),[&](const Table& item){return item.name==table && std::find(item.columns.begin(),item.columns.end(),column)!=item.columns.end();});
        if(entry==list.end() || identity["keys"].size()!=entry->keys.size())throw std::runtime_error("引用文档标识不合法");
        const auto database=source=="main"?fromUtf8(db.path()):source=="log"?fromUtf8(db.logPath()):source=="chat"?fromUtf8(db.chatPath()):data/(source=="lua"?"lua_mod.db":"plugins.db");
        Db connection(database);
        std::string sql="UPDATE "+table+" SET "+column+"=?1 WHERE "+column+"=?2";
        for(std::size_t i=0;i<entry->keys.size();++i)sql+=" AND "+entry->keys[i]+"=?"+std::to_string(i+3);
        Stmt update(connection.raw,sql);update.bind(1,change.after);update.bind(2,change.before.content);
        for(std::size_t i=0;i<entry->keys.size();++i)update.bind(static_cast<int>(i+3),identity["keys"][i].get<std::string>());
        update.step();if(sqlite3_changes(connection.raw)!=1)throw std::runtime_error("引用文档已被修改，未覆盖新内容");
        if(source=="main") {
            if(table=="reply_rules")pending->replies=true;
            else if(table=="i18n_overrides") {
                const auto row=db.getStorage()->get<I18nOverrideRow>(std::stoi(identity["keys"][0].get<std::string>()));
                i18n.setOverride(localeFromString(row.locale),row.key,row.value,row.format=="markdown"?ContentFormat::kMarkdown:ContentFormat::kPlainText);
            } else if(table=="persona_entries") {
                const auto row=db.getStorage()->get<PersonaEntryRow>(std::stoi(identity["keys"][0].get<std::string>()));pending->personas.insert(row.personaId);
            }
        }
    },[pending,&personas,&replies,&decks] {
        if(pending->replies)replies.loadRules();
        if(pending->decks)decks.reload({"decks","data/decks"});
        for(const auto id:pending->personas)personas.loadIntoI18n(id);
        pending->replies=pending->decks=false;pending->personas.clear();
    });
}
} // namespace dice::image_resources
