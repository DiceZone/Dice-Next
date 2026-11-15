#include "live_log_service.h"
#include "../adapter/adapter_interface.h"
#include "../config/config_manager.h"
#include "../common/logger.h"
#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace dice {
namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
class Connection {
public:
    sqlite3* db{};
    explicit Connection(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            throw std::runtime_error("无法打开跑团日志库");
        }
        sqlite3_busy_timeout(db, 5000);
    }
    ~Connection() { sqlite3_close(db); }
    void exec(const char* sql) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
};
class Statement {
    sqlite3_stmt* stmt{};
public:
    Statement(Connection& c, const char* sql) {
        if (sqlite3_prepare_v2(c.db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(c.db));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    void bind(int i, const std::string& s) { sqlite3_bind_text(stmt,i,s.data(),static_cast<int>(s.size()),SQLITE_TRANSIENT); }
    void bind(int i, int64_t n) { sqlite3_bind_int64(stmt,i,n); }
    bool step() {
        int rc=sqlite3_step(stmt);
        if (rc==SQLITE_ROW) return true;
        if (rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    std::string text(int i) { const auto* p=sqlite3_column_text(stmt,i); return p ? reinterpret_cast<const char*>(p) : ""; }
    int64_t number(int i) { return sqlite3_column_int64(stmt,i); }
};

// Tokens are only sent to the official BDC log service, never to a custom
// legacy upload URL. The client validates TLS and does not follow redirects.
std::pair<int,Json> post(const std::string& origin,const std::string& path,const std::string& secret,const Json& body) {
    trantor::EventLoopThread loop("live-log-http"); loop.run();
    auto client=drogon::HttpClient::newHttpClient(origin,loop.getLoop(),false,true);
    auto req=drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post); req->setPath("/api/v2/live/"+path);
    req->addHeader("Authorization","Bearer "+secret);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON); req->setBody(body.dump());
    auto [result,response]=client->sendRequest(req,8.0);
    if (result!=drogon::ReqResult::Ok || !response) return {0,Json()};
    return {static_cast<int>(response->statusCode()),Json::parse(response->body(),nullptr,false)};
}
std::string botKey(ConfigManager& cfg,const std::string& adapter) {
    const auto adapters=cfg.get<Json>("adapters",Json::array());
    for(const auto& a:adapters) {
        if (!a.is_object() || !a.contains("id")) continue;
        const auto& id=a.at("id");
        if ((id.is_string()?id.get<std::string>():id.dump())==adapter)
            return a.value("heart_api_key",a.value("heartApiKey",std::string()));
    }
    return {};
}

constexpr auto schema=R"SQL(
CREATE TABLE IF NOT EXISTS live_sessions(
 log_id INTEGER PRIMARY KEY,adapter TEXT,platform TEXT,bot TEXT,key TEXT,url TEXT,token TEXT,
 seq INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL DEFAULT 'live',retry INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS live_outbox(log_id INTEGER,seq INTEGER,event TEXT,PRIMARY KEY(log_id,seq));
)SQL";
constexpr auto triggers=R"SQL(
CREATE TRIGGER IF NOT EXISTS live_message_add AFTER INSERT ON game_log_messages
WHEN EXISTS(SELECT 1 FROM live_sessions WHERE log_id=NEW.log_id AND state='live') BEGIN
 UPDATE live_sessions SET seq=seq+1 WHERE log_id=NEW.log_id;
 INSERT INTO live_outbox SELECT log_id,seq,json_object('seq',seq,'op','add','item',
 json_object('rawMsgId',msg_prefix||NEW.id,'message',NEW.content,'nickname',NEW.sender,
 'IMUserId',(CASE WHEN platform IN ('onebot_v11','milky','openopq','qq_official') THEN 'QQ' ELSE platform END)||':'||NEW.user_id,'time',coalesce(unixepoch(NEW.created_at),0),
 'isDice',json(CASE WHEN bot<>'' AND bot=NEW.user_id THEN 'true' ELSE 'false' END)))
 FROM live_sessions WHERE log_id=NEW.log_id;
END;
CREATE TRIGGER IF NOT EXISTS live_message_edit AFTER UPDATE OF content ON game_log_messages
WHEN NEW.content<>OLD.content AND EXISTS(SELECT 1 FROM live_sessions WHERE log_id=NEW.log_id AND state='live') BEGIN
 UPDATE live_sessions SET seq=seq+1 WHERE log_id=NEW.log_id;
 INSERT INTO live_outbox SELECT log_id,seq,json_object('seq',seq,'op','edit','rawMsgId',msg_prefix||NEW.id,'message',NEW.content)
 FROM live_sessions WHERE log_id=NEW.log_id;
END;
CREATE TRIGGER IF NOT EXISTS live_message_delete AFTER DELETE ON game_log_messages
WHEN EXISTS(SELECT 1 FROM live_sessions WHERE log_id=OLD.log_id AND state='live') BEGIN
 UPDATE live_sessions SET seq=seq+1 WHERE log_id=OLD.log_id;
 INSERT INTO live_outbox SELECT log_id,seq,json_object('seq',seq,'op','delete','rawMsgId',msg_prefix||OLD.id)
 FROM live_sessions WHERE log_id=OLD.log_id;
END;
)SQL";

bool validKey(const std::string& key) {
    return key.size() >= 4 && key.size() <= 12 &&
        key.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == std::string::npos;
}

void prepareSchema(Connection& db) {
    db.exec("BEGIN IMMEDIATE");
    db.exec(schema);
    bool migrated;
    {
        Statement column(db, "SELECT 1 FROM pragma_table_info('live_sessions') WHERE name='msg_prefix'");
        migrated = column.step();
    }
    if (!migrated) {
        // Pre-upgrade events used the bare local row ID. Their edits must keep that ID.
        db.exec("ALTER TABLE live_sessions ADD COLUMN msg_prefix TEXT NOT NULL DEFAULT '';"
                "DROP TRIGGER IF EXISTS live_message_add; DROP TRIGGER IF EXISTS live_message_edit;"
                "DROP TRIGGER IF EXISTS live_message_delete;");
    }
    db.exec(triggers);
    db.exec("COMMIT");
}
}

struct LiveLogService::Impl {
    std::string path,origin;
    ConfigManager& cfg;
    std::mutex opening;
    std::jthread worker;
    Impl(std::string file,ConfigManager& c,std::string host):path(std::move(file)),origin(std::move(host)),cfg(c) {
        Connection db(path); prepareSchema(db);
        worker=std::jthread([this](std::stop_token stop){
            while (!stop.stop_requested()) {
                try { tick(stop); } catch(const std::exception& e) { DICE_LOG_WARN("云日志同步失败：{}",e.what()); }
                for(int i=0;i<15 && !stop.stop_requested();++i) std::this_thread::sleep_for(100ms);
            }
        });
    }
    void tick(std::stop_token stop) {
        Connection db(path);
        std::vector<int64_t> ids;
        {
            Statement ready(db,"SELECT log_id FROM live_sessions WHERE state='live' AND retry<=unixepoch() AND (EXISTS(SELECT 1 FROM live_outbox o WHERE o.log_id=live_sessions.log_id) OR coalesce((SELECT status FROM game_logs WHERE id=log_id),2)=2)");
            while(ready.step()) ids.push_back(ready.number(0));
        }
        for(auto id:ids) {
            if(stop.stop_requested()) break;
            std::string key,token,adapter,url;
            bool closed=false;
            {
                Statement row(db,"SELECT key,token,adapter,coalesce(g.status,2),url FROM live_sessions s LEFT JOIN game_logs g ON g.id=s.log_id WHERE s.log_id=?");
                row.bind(1,id); if(!row.step()) continue;
                key=row.text(0);token=row.text(1);adapter=row.text(2);closed=row.number(3)==2;url=row.text(4);
            }
            if(botKey(cfg,adapter).empty()) {
                Statement pause(db,"UPDATE live_sessions SET retry=unixepoch()+60 WHERE log_id=?");
                pause.bind(1,id);pause.step();continue;
            }
            Json batch=Json::array(); size_t bytes=0;
            {
                Statement events(db,"SELECT event FROM live_outbox WHERE log_id=? ORDER BY seq LIMIT 500");
                events.bind(1,id);
                while(events.step()) {
                    auto item=events.text(0);
                    if(!batch.empty() && bytes+item.size()>1024*1024) break;
                    bytes+=item.size(); batch.push_back(Json::parse(item));
                }
            }
            bool ending=batch.empty();
            if(ending && !closed) continue;
            auto [status,body]=post(origin,key+(ending?"/end":"/append"),token,ending?Json::object():Json{{"events",batch}});
            // The site may have committed /end before its acknowledgement was lost.
            if (ending && status == 409 && body.is_object() && body.value("url",std::string()) == url) {
                Statement done(db,"UPDATE live_sessions SET state='done',token='' WHERE log_id=?");
                done.bind(1,id);done.step();continue;
            }
            if (!ending && status==409 && body.is_object() && body.contains("last_seq")) {
                // A retained suffix can be replayed if it begins at the expected sequence.
                // A missing prefix cannot be invented: retain the local transcript and stop.
                auto acknowledged=body.value("last_seq",int64_t{-1});
                auto first=batch.front().at("seq").get<int64_t>(), last=batch.back().at("seq").get<int64_t>();
                if(acknowledged>=first && acknowledged<=last) {
                    Statement ack(db,"DELETE FROM live_outbox WHERE log_id=? AND seq<=?");
                    ack.bind(1,id);ack.bind(2,acknowledged);ack.step();continue;
                }
            }
            if(status==200 && body.is_object() && body.value("success",false)) {
                if(ending) {
                    Statement done(db,"UPDATE live_sessions SET state=?,token='' WHERE log_id=?");
                    done.bind(1,body.value("deleted",false)?"deleted":"done");done.bind(2,id);done.step();continue;
                }
                auto seq=batch.back().at("seq").get<int64_t>();
                if(body.value("last_seq",int64_t{-1})==seq) {
                    Statement ack(db,"DELETE FROM live_outbox WHERE log_id=? AND seq<=?");
                    ack.bind(1,id);ack.bind(2,seq);ack.step();continue;
                }
            }
            const bool permanent=status==400||status==401||status==403||status==404||status==409||status==413;
            Statement later(db,"UPDATE live_sessions SET state=?,retry=unixepoch()+60 WHERE log_id=?");
            later.bind(1,permanent?"failed":"live");later.bind(2,id);later.step();
            DICE_LOG_WARN("云日志「{}」同步失败（HTTP {}），{}",key,status,permanent?"请检查日志站状态，本地记录仍保留":"一分钟后重试");
        }
    }
};

LiveLogService::LiveLogService(const std::string& path,ConfigManager& cfg,std::string origin):impl_(std::make_unique<Impl>(path,cfg,std::move(origin))) {}
LiveLogService::~LiveLogService()=default;
void LiveLogService::stop(){impl_->worker.request_stop();if(impl_->worker.joinable())impl_->worker.join();}
bool LiveLogService::belongsTo(int id,const std::string& adapter){
    Connection db(impl_->path); Statement s(db,"SELECT adapter FROM live_sessions WHERE log_id=?");
    s.bind(1,id);return !s.step() || s.text(0)==adapter;
}
std::string LiveLogService::link(int id){
    Connection db(impl_->path); Statement s(db,"SELECT key,url,state FROM live_sessions WHERE log_id=?");
    s.bind(1,id);if(!s.step())return {};
    if (s.text(2)=="deleted") return "空日志已由日志站删除。";
    return "云日志 "+s.text(0)+"\n"+s.text(1)+(s.text(2)=="failed"?"\n云端同步已停止，本地仍保留记录。":"");
}
std::string LiveLogService::start(const Message& msg,const std::string& name,int& id){
    return open(msg, name, {}, {}, id);
}

std::string LiveLogService::fork(const Message& msg, const std::string& arguments, int& id) {
    id = 0;
    std::istringstream input(arguments);
    std::string key, password, name;
    input >> key;
    if (key.starts_with("https://") || key.starts_with("http://")) {
        // Read credentials from the viewer link. All requests still go to our configured origin.
        const auto query = key.find("?key=");
        const auto fragment = key.find('#');
        if (query != std::string::npos && fragment != std::string::npos && fragment > query + 5) {
            password = key.substr(fragment + 1);
            key = key.substr(query + 5, fragment - query - 5);
        }
    } else if (const auto separator = key.find('#'); separator != std::string::npos) {
        password = key.substr(separator + 1);
        key.resize(separator);
    } else {
        input >> password;
    }
    if (!validKey(key) || password.empty() || password.size() > 128)
        return "用法：.log fork 编码#查看密码 [新名称]。也可使用完整观看链接，或用空格分隔编码与密码。";
    std::getline(input >> std::ws, name);
    return open(msg, std::move(name), key, password, id);
}

std::string LiveLogService::open(const Message& msg, std::string name, const std::string& sourceKey,
                                 const std::string& password, int& id) {
    id=0;
    const auto secret=botKey(impl_->cfg,msg.adapterId);
    if(secret.empty())return "请先为当前适配器配置 BDC 骰娘密钥。";
    for (unsigned char c : secret)
        if (c <= 0x20 || c >= 0x7f) return "当前适配器的 BDC 骰娘密钥格式无效。";
    std::lock_guard lock(impl_->opening);
    Connection db(impl_->path);
    {
        Statement existing(db,"SELECT g.id FROM game_logs g JOIN live_sessions s ON s.log_id=g.id WHERE g.group_id=? AND s.adapter=? AND g.status=0 AND s.state='live'");
        existing.bind(1,msg.targetId);existing.bind(2,msg.adapterId);
        if(existing.step()){
            if (!sourceKey.empty()) return "本群已有正在记录的云日志，请先结束或暂停后再续写。";
            id=static_cast<int>(existing.number(0));return link(id);
        }
    }
    const bool forking = !sourceKey.empty();
    if (!forking && name.empty()) name = "跑团日志";
    Json request = {{"name",name},{"group_id",msg.targetId},{"bot_id",msg.selfId},{"uniform_id",msg.platform+":"+msg.targetId}};
    if (forking) request["password"] = password;
    auto [status,body] = post(impl_->origin, forking ? sourceKey + "/fork" : "start", secret, request);
    if (status == 401 || status == 403) return "日志站未接受当前适配器的 BDC 骰娘密钥。";
    if (forking && status == 404) return "找不到原日志，请核对日志编码与查看密码。";
    if (forking && status == 409) return "这份日志的格式不支持续写。";
    if (forking && status == 413) return "原日志超过日志站的实时日志大小上限。";
    if (status == 429) return "日志站已达到同时记录的份数或请求次数上限，请稍后再试。";
    if (status != 200) return "云日志开启失败：" + (status ? "日志站返回 HTTP " + std::to_string(status) : std::string("无法连接日志站"));
    if (!body.is_object() || !body.contains("last_seq") || !body["last_seq"].is_number_integer())
        return "日志站返回的会话信息不完整。";
    const auto seq = body["last_seq"].get<int64_t>();
    for (const auto* field : {"key", "token", "url"})
        if (!body.contains(field) || !body[field].is_string() || body[field].get_ref<const std::string&>().empty())
            return "日志站返回的会话信息不完整。";
    const auto key=body["key"].get<std::string>(), token=body["token"].get<std::string>(),url=body["url"].get<std::string>();
    if (!validKey(key) || seq < 0 || (!forking && seq != 0) ||
        (forking && (body.value("forked_from",std::string()) != sourceKey || key == sourceKey ||
                     !body.contains("name") || !body["name"].is_string())))
        return "日志站返回的会话信息不完整。";
    if (forking) name = body["name"].get<std::string>();
    db.exec("BEGIN IMMEDIATE");
    Statement log(db,"INSERT INTO game_logs(group_id,gm_id,name,players,custom_rules,status,created_at) VALUES(?,?,?,'[]','{}',0,strftime('%Y-%m-%dT%H:%M:%SZ','now'))");
    log.bind(1,msg.targetId);log.bind(2,msg.senderId);log.bind(3,name);log.step();
    const auto localId=sqlite3_last_insert_rowid(db.db);
    Statement session(db,"INSERT INTO live_sessions(log_id,adapter,bot,key,url,token,platform,seq,msg_prefix) VALUES(?,?,?,?,?,?,?,?,?)");
    session.bind(1,localId);session.bind(2,msg.adapterId);session.bind(3,msg.selfId);session.bind(4,key);session.bind(5,url);session.bind(6,token);session.bind(7,msg.platform);
    session.bind(8,seq);session.bind(9,key + ":");session.step();
    db.exec("COMMIT");id=static_cast<int>(localId);
    if (forking) return "已从云日志 " + sourceKey + " 复制 " + std::to_string(seq) + " 条历史消息。后续记录请使用新链接。\n" + link(id);
    return link(id);
}
}
