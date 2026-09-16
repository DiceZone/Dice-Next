#include "cloud_card_service.h"
#include "../adapter/adapter_interface.h"
#include "../config/config_manager.h"
#include "../core/character/card_store.h"
#include "../core/identity/identity_binding.h"
#include "../storage/database.h"
#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <condition_variable>
#include <deque>
#include <thread>

namespace dice::cloud_cards {
namespace {
constexpr const char* origin = "https://account.dice.zone";
constexpr size_t maxBody = 4 * 1024 * 1024;
using Clock = std::chrono::steady_clock;
std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \r\n\t");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
std::pair<std::string, std::string> split(const std::string& s) {
    auto t = trim(s); auto p = t.find_first_of(" \t\r\n");
    return {t.substr(0, p), p == std::string::npos ? "" : trim(t.substr(p))};
}
std::string encode(const std::string& s) {
    const char* hex = "0123456789ABCDEF"; std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
std::string hash(const std::string& s) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), out);
    std::ostringstream text;
    for (auto c : out) text << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
    return text.str();
}
bool safeSecret(const std::string& value) {
    return !value.empty() && value.size() < 16384 &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
void require(bool ok) { if (!ok) throw std::runtime_error("invalid cloud card document"); }
bool validId(const std::string& s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
void validKey(const std::string& key) { require(!trim(key).empty() && key.size() <= 256); }
Response http(const Request& input) {
    // Separate loop: synchronous cloud commands cannot deadlock an adapter's loop.
    // Credentials stay in memory, never command-line arguments or temporary files.
    trantor::EventLoopThread loop("cloud-card-http"); loop.run();
    auto client = drogon::HttpClient::newHttpClient(origin, loop.getLoop(), false, true);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPath(input.path);
    req->setMethod(input.method == "POST" ? drogon::Post : input.method == "PUT" ? drogon::Put : drogon::Get);
    req->addHeader("Accept", "application/json");
    if (!input.apiKey.empty()) req->addHeader("X-API-Key", input.apiKey);
    if (!input.accessToken.empty()) req->addHeader("Authorization", "Bearer " + input.accessToken);
    if (!input.body.empty()) {
        req->addHeader("Content-Type", input.form ? "application/x-www-form-urlencoded" : "application/json");
        req->setBody(input.body);
    }
    const auto [result, response] = client->sendRequest(req, 8.0);
    if (result != drogon::ReqResult::Ok || !response || response->body().size() > maxBody) return {};
    return {static_cast<int>(response->statusCode()), Json::parse(response->body(), nullptr, false)};
}
Result result(const std::string& key) { return {"cloud_card." + key, {}}; }
Result failure(const Response& r) {
    const auto error = r.body.is_object() && r.body.contains("error") && r.body["error"].is_string()
        ? r.body["error"].get<std::string>() : std::string();
    if (error == "slow_down") return result("wait");
    if (error == "expired_token" || error == "invalid_grant" || error == "invalid_client") return result("expired");
    if (error == "access_denied") return result("denied");
    if (r.status == 401) return result("expired");
    if (r.status == 403) return result("readonly");
    if (r.status == 404) return result("not_found");
    if (r.status == 409) return result("conflict");
    if (r.status == 429) return result("wait");
    // Never echo arbitrary OAuth/server error bodies (may contain credentials).
    return result("network");
}
} // namespace

Response probeOfficialMetadata() { return http({"/.well-known/openid-configuration"}); }

Json toDocument(const Json& raw, const std::string& name, const std::string& system) {
    require(raw.is_object());
    Json doc = {{"schema_version", 1}, {"name", name}, {"system", system},
        {"attrs", Json::object()}, {"meta", {{"locks", Json::array()}, {"texts", Json::object()}}},
        {"extra", Json::object()}};
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        if (it.key() == "__meta" && it->is_object()) {
            Json extra = *it;
            if (extra.contains("locks")) {
                require(extra["locks"].is_string());
                std::istringstream locks(extra["locks"].get<std::string>()); std::string key;
                while (std::getline(locks, key, ',')) {
                    key = trim(key);
                    if (!key.empty() && std::find(doc["meta"]["locks"].begin(), doc["meta"]["locks"].end(), key)
                        == doc["meta"]["locks"].end()) doc["meta"]["locks"].push_back(key);
                }
                extra.erase("locks");
            }
            if (extra.contains("texts")) {
                require(extra["texts"].is_object());
                doc["meta"]["texts"] = extra["texts"]; extra.erase("texts");
            }
            doc["extra"]["__meta"] = extra;
        } else if (it.key() != "__meta" && it->is_number_integer()) doc["attrs"][it.key()] = *it;
        else doc["extra"][it.key()] = *it;
    }
    (void)fromDocument(doc); // Validate against the same contract before upload.
    return doc;
}

Json fromDocument(const Json& doc) {
    require(doc.is_object() && doc.contains("schema_version") && doc["schema_version"].is_number_integer() && doc["schema_version"] == 1);
    require(doc.contains("name") && doc["name"].is_string());
    auto name = doc["name"].get<std::string>();
    require(!trim(name).empty() && name.size() <= 400);
    require(doc.value("system", Json("")).is_string());
    require(doc.contains("attrs") && doc["attrs"].is_object() && doc["attrs"].size() <= 512);
    require(doc.contains("meta") && doc["meta"].is_object());
    const auto& meta = doc["meta"];
    require(meta.contains("locks") && meta["locks"].is_array());
    require(meta.contains("texts") && meta["texts"].is_object() && meta["texts"].size() <= 256);
    require(doc.contains("extra") && doc["extra"].is_object() && doc.dump().size() <= maxBody);
    Json raw = doc["extra"];
    for (auto it = doc["attrs"].begin(); it != doc["attrs"].end(); ++it) {
        validKey(it.key()); require(it.key() != "__meta" && !raw.contains(it.key()) && it->is_number_integer());
        if (it->is_number_unsigned()) require(it->get<uint64_t>() <= 1000000);
        else require(it->get<int64_t>() >= -1000000 && it->get<int64_t>() <= 1000000);
        raw[it.key()] = *it;
    }
    std::string locks;
    for (const auto& entry : meta["locks"]) {
        require(entry.is_string()); auto key = entry.get<std::string>(); validKey(key);
        require(key.find(',') == std::string::npos);
        locks += (locks.empty() ? "" : ",") + key;
    }
    for (auto it = meta["texts"].begin(); it != meta["texts"].end(); ++it) {
        validKey(it.key()); require(it->is_string() && it->get_ref<const std::string&>().size() <= 16384);
    }
    if (!locks.empty() || !meta["texts"].empty()) {
        if (!raw.contains("__meta") || !raw["__meta"].is_object()) raw["__meta"] = Json::object();
        if (!locks.empty()) raw["__meta"]["locks"] = locks;
        if (!meta["texts"].empty()) raw["__meta"]["texts"] = meta["texts"];
    }
    return raw;
}

struct Service::Impl {
    Database& db; ConfigManager& cfg; CharacterCardStore& cards; Transport transport; Now now;
    std::mutex mutex;
    struct Job { Message msg; std::string args; std::function<void(Result)> completed; };
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    bool stopping = false;
    std::thread worker;
    struct Session {
        std::string device, access, scope, apiHash;
        Clock::time_point expires{}, nextPoll{};
        int interval = 5;
    };
    std::unordered_map<std::string, Session> sessions;
    std::string devicePath, tokenPath, userinfoPath;
    Impl(Database& d, ConfigManager& c, CharacterCardStore& s, Transport t, Now n)
        : db(d), cfg(c), cards(s), transport(t ? std::move(t) : http), now(n ? std::move(n) : Clock::now) {}

    Response request(Request r) { return transport(r); }
    std::string apiKey(const Message& msg) {
        auto all = cfg.getAll();
        if (!all.contains("adapters") || !all["adapters"].is_array()) return "";
        for (const auto& a : all["adapters"]) {
            if (!a.is_object() || !a.contains("id")) continue;
            const auto id = a["id"].is_string() ? a["id"].get<std::string>() : a["id"].dump();
            if (id == msg.adapterId) return a.value("heart_api_key", a.value("heartApiKey", std::string()));
        }
        return "";
    }
    std::string context(const Message& msg) {
        std::string native = msg.extra.is_object() ? msg.extra.value("__identity_native_sender", msg.senderId) : msg.senderId;
        return hash(Json::array({origin, msg.adapterId, msg.selfId, msg.platform, native, msg.senderId}).dump());
    }
    bool discovery() {
        if (!devicePath.empty()) return true;
        auto r = request({"/.well-known/openid-configuration"});
        if (r.status != 200 || !r.body.is_object() || r.body.value("issuer", "") != origin) return false;
        auto path = [&](const char* key) {
            auto url = r.body.value(key, std::string()); std::string prefix = std::string(origin) + "/";
            if (url.rfind(prefix, 0) != 0 || url.find_first_of("\r\n\t #?\\") != std::string::npos) return std::string();
            return url.substr(std::char_traits<char>::length(origin));
        };
        auto device = path("device_authorization_endpoint"), token = path("token_endpoint");
        userinfoPath = path("userinfo_endpoint");
        // BDC b894848 serves RFC 8628 but does not yet advertise the device
        // endpoint in discovery. This compatibility fallback is same-origin only.
        if (!r.body.contains("device_authorization_endpoint")) device = "/api/oauth/device_authorization";
        if (device.empty() || token.empty()) return false;
        devicePath = device; tokenPath = token; return true;
    }
    // 本站注册只收纯数字 QQ 邮箱且必须过验证码，所以注册邮箱的前缀就是一个已验证
    // 的真实 QQ 号——QQ 互联的 openid 给不了这个。拿到之后把当前平台身份关联过去，
    // 人物卡与好感随之互通。
    //
    // 会话已经带着另一个真实 QQ 时什么都不做：两边都"是真的"，静默合并会把两个人
    // 的人物卡搅在一起，宁可不动，留给 .bind 让人自己看清楚再决定。
    std::string bindVerifiedQQ(const Message& msg, const std::string& access) {
        if (userinfoPath.empty() || access.empty()) return {};
        auto r = request({userinfoPath, "GET", "", "", access});
        if (r.status != 200 || !r.body.is_object()) return {};
        const auto email = r.body.value("email", std::string());
        const auto at = email.find("@qq.com");
        if (at == std::string::npos || at == 0) return {};
        const std::string qq = email.substr(0, at);
        using identity::BindingStore;
        using identity::Kind;
        if (!BindingStore::isRealQQ(qq)) return {};

        // 已是真实 QQ 的会话（OneBot，或此前已绑定过的官方会话）没有可绑的东西；
        // 对不上就更不能动。
        if (BindingStore::isRealQQ(msg.senderId)) return msg.senderId == qq ? qq : std::string();

        const auto exval = [&msg](const char* key) {
            return msg.extra.is_object() ? msg.extra.value(key, std::string()) : std::string();
        };
        const std::string transportName = exval("__identity_transport");
        const std::string native = exval("__identity_native_sender");
        if (transportName.empty() || native.empty()) return {};

        auto& bindings = BindingStore::instance();
        std::string error;
        if (transportName == "qq_official") {
            const std::string bot = exval("official_bot_id");
            if (bot.empty()) return {};
            if (!bindings.bindOfficialToQQ(db, BindingStore::officialId(bot, native), qq, Kind::User, error))
                return {};
        } else if (!bindings.bindPlatformToQQ(db, transportName, native, qq, Kind::User, error)) {
            return {};
        }
        return qq;
    }

    std::vector<UserSettingRow> links(const Message& msg, const std::string& ctx) {
        auto* st = db.getStorage(); if (!st) throw std::runtime_error("no storage");
        namespace orm = sqlite_orm;
        return st->get_all<UserSettingRow>(orm::where(orm::c(&UserSettingRow::userId) == msg.senderId and
            orm::c(&UserSettingRow::groupId) == "bdc-cloud:" + ctx));
    }
    Json link(const Message& msg, const std::string& ctx, const std::string& cardId) {
        for (const auto& row : links(msg, ctx)) if (row.key == cardId) return Json::parse(row.value);
        return Json();
    }
    void saveLink(const Message& msg, const std::string& ctx, const Json& doc,
                  const CharacterCardStore::Snapshot& local) {
        auto* st = db.getStorage(); if (!st) throw std::runtime_error("no storage");
        auto cardId = doc.at("card_id").get<std::string>();
        UserSettingRow out;
        for (const auto& row : links(msg, ctx)) if (row.key == cardId) { out = row; break; }
        out.userId = msg.senderId; out.groupId = "bdc-cloud:" + ctx; out.key = cardId;
        out.value = Json{{"local_id", local.id}, {"local_name", local.name}, {"local_data", local.data}, {"base", doc}}.dump();
        if (out.id) st->update(out); else st->insert(out);
    }
    bool validRemote(const Json& doc, const std::string& expectedId = "") {
        (void)fromDocument(doc);
        auto id = doc.at("card_id").get<std::string>();
        return validId(id) && (expectedId.empty() || id == expectedId) && doc.at("rev").is_number_integer() && doc.at("rev").get<int64_t>() > 0;
    }
    Result handle(const Message& msg, const std::string& args) {
        if (msg.type != MessageType::kPrivate || msg.fromSelf || msg.senderId.empty() || msg.adapterId.empty())
            return result("private_only");
        auto [action, rest] = split(args);
        std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (action.empty() || action == "help") return result("usage");
        if (action != "auth" && action != "confirm" && action != "status" && action != "logout" &&
            action != "list" && action != "pull" && action != "push" && action != "sync") return result("usage");
        auto ctx = context(msg);
        if (action == "logout") { sessions.erase(ctx); return result("logout"); }
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second.expires <= now()) it = sessions.erase(it); else ++it;
        }
        auto key = apiKey(msg);
        if (!safeSecret(key) || key.rfind("bdc_", 0) != 0) { sessions.erase(ctx); return result("no_key"); }
        auto existing = sessions.find(ctx);
        if (existing != sessions.end() && existing->second.apiHash != hash(key)) sessions.erase(existing);
        if (action == "status") return result(sessions.count(ctx) ?
            (sessions.at(ctx).access.empty() ? "pending" : "authorized") : "expired");
        if (action == "auth") {
            if (!rest.empty() && rest != "read" && rest != "write") return result("usage");
            if (sessions.count(ctx) && sessions.at(ctx).access.empty()) return result("pending");
            if (sessions.size() >= 1024) return result("busy");
            if (!discovery()) return result("network");
            // email 是本站已验证的真实 QQ 号（注册只收纯数字 QQ 邮箱且必须过验证码），
            // 授权后据此把当前平台身份关联到该 QQ。
            auto scopes = std::string("cards.read email") + (rest == "write" ? " cards.write" : "");
            // Do not assert a public QQ identity for an application-scoped OpenID.
            auto native = msg.extra.is_object() ? msg.extra.value("__identity_native_sender", msg.senderId) : msg.senderId;
            Request req{devicePath, "POST", "scope=" + encode(scopes) + "&platform=" + encode(msg.platform) +
                "&platform_user_id=" + encode(native), key, "", true};
            auto r = request(req);
            if (r.status != 200 || !r.body.is_object()) return failure(r);
            const auto device = r.body.value("device_code", ""), code = r.body.value("user_code", "");
            if (!safeSecret(device) || code.empty() || code.size() > 16 ||
                !std::all_of(code.begin(), code.end(), [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'; }) ||
                r.body.value("verification_uri", "") != std::string(origin) + "/device") return result("bad_response");
            Session s; s.device = device; s.apiHash = hash(key); s.scope = scopes;
            s.interval = std::clamp(r.body.value("interval", 5), 5, 60);
            const int ttl = std::clamp(r.body.value("expires_in", 0), 0, 900);
            if (!ttl) return result("bad_response");
            s.expires = now() + std::chrono::seconds(ttl);
            s.nextPoll = now() + std::chrono::seconds(s.interval); sessions[ctx] = s;
            return {"cloud_card.auth_code", {{"url", std::string(origin) + "/device"}, {"code", code}, {"seconds", std::to_string(ttl)}}, true};
        }
        if (!sessions.count(ctx)) return result("expired");
        auto& s = sessions.at(ctx);
        if (s.access.empty()) {
            if (now() < s.nextPoll) return result("wait");
            auto r = request({tokenPath, "POST", "grant_type=" + encode("urn:ietf:params:oauth:grant-type:device_code") +
                "&device_code=" + encode(s.device), key, "", true});
            s.nextPoll = now() + std::chrono::seconds(s.interval);
            if (r.status != 200) {
                const auto err = r.body.is_object() ? r.body.value("error", "") : "";
                if (err == "authorization_pending") return result("pending");
                if (err == "slow_down" || r.status == 429) {
                    s.interval = (std::min)(s.interval + 5, 120); s.nextPoll = now() + std::chrono::seconds(s.interval);
                    return result("wait");
                }
                sessions.erase(ctx); return err == "access_denied" ? result("denied") : failure(r);
            }
            if (!r.body.is_object() || !safeSecret(r.body.value("access_token", "")) ||
                r.body.value("token_type", "") != "Bearer") { sessions.erase(ctx); return result("bad_response"); }
            s.access = r.body["access_token"]; s.device.clear();
            const auto granted = " " + r.body.value("scope", std::string()) + " ";
            const auto requested = " " + s.scope + " ";
            s.scope.clear();
            for (const auto* allowed : {"cards.read", "cards.write", "email"}) {
                const auto term = std::string(" ") + allowed + " ";
                if (granted.find(term) != std::string::npos && requested.find(term) != std::string::npos)
                    s.scope += (s.scope.empty() ? "" : " ") + std::string(allowed);
            }
            const int ttl = std::clamp(r.body.value("expires_in", 0), 0, 86400);
            if (!ttl) { sessions.erase(ctx); return result("expired"); }
            s.expires = now() + std::chrono::seconds(ttl);
            if ((" " + s.scope + " ").find(" email ") != std::string::npos) {
                const auto boundQQ = bindVerifiedQQ(msg, s.access);
                if (!boundQQ.empty()) return {"cloud_card.authorized_bound", {{"qq", boundQQ}}, true};
            }
            return result("authorized");
        }
        if (action == "confirm") return result("authorized");
        auto hasScope = [&](const char* scope) { return (" " + s.scope + " ").find(std::string(" ") + scope + " ") != std::string::npos; };
        if (!hasScope("cards.read")) return result("readonly");
        auto call = [&](std::string path, std::string method = "GET", std::string body = "") {
            auto r = request({std::move(path), std::move(method), std::move(body), "", s.access});
            return r;
        };
        if (action == "list") {
            auto r = call("/api/v1/cards");
            if (r.status != 200) { if (r.status == 401) sessions.erase(ctx); return failure(r); }
            if (!r.body.is_object() || !r.body.contains("items") || !r.body["items"].is_array()) return result("bad_response");
            std::string list;
            for (const auto& doc : r.body["items"]) {
                if (!validRemote(doc)) return result("bad_response");
                list += doc["card_id"].get<std::string>() + " | " + doc["name"].get<std::string>() + " | rev " + doc["rev"].dump() + "\n";
            }
            return {"cloud_card.list", {{"list", list.empty() ? "(0)" : list}}};
        }
        if (action == "pull") {
            auto [id, localName] = split(rest); if (!validId(id)) return result("usage");
            auto old = link(msg, ctx, id);
            std::optional<CharacterCardStore::Snapshot> local;
            bool separateCopy = false;
            if (old.is_object()) {
                local = cards.snapshotById(msg.senderId, old.at("local_id").get<int>());
                separateCopy = !localName.empty() && (!local || localName != local->name);
                if (separateCopy) local.reset();
                else if (!local || local->data != old.at("local_data") || local->name != old.at("local_name").get<std::string>()) return result("local_changes");
            }
            auto r = call("/api/v1/cards/" + id);
            if (r.status != 200) { if (r.status == 401) sessions.erase(ctx); return failure(r); }
            if (!validRemote(r.body, id)) return result("bad_response");
            if (local) localName = local->name;
            else if (localName.empty()) localName = r.body["name"].get<std::string>();
            auto saved = cards.importSnapshot(msg.senderId, localName, fromDocument(r.body), local);
            if (!saved) return result("local_changes");
            if (!separateCopy) saveLink(msg, ctx, r.body, *saved);
            return {"cloud_card.pulled", {{"name", localName}, {"id", id}}};
        }
        if (rest.empty()) return result("usage");
        auto local = cards.snapshot(msg.senderId, rest);
        if (!local || local->name.empty()) return result("local_missing");
        if (cards.cardLockedByName(msg.senderId, local->name, "r") || cards.cardLockedByName(msg.senderId, local->name, "w"))
            return result("locked");
        Json old;
        for (const auto& row : links(msg, ctx)) {
            auto l = Json::parse(row.value);
            if (l.at("local_id").get<int>() == local->id) { old = std::move(l); break; }
        }
        if (action == "sync" && !old.is_object()) return result("not_linked");
        if (!hasScope("cards.write")) return result("readonly");
        std::string id, name = local->name, system;
        int64_t rev = 0;
        if (old.is_object()) {
            id = old["base"].at("card_id"); if (!validId(id)) return result("bad_response");
            rev = old["base"].at("rev").get<int64_t>(); system = old["base"].value("system", "");
            if (local->name == old.at("local_name").get<std::string>()) name = old["base"].at("name");
        }
        auto doc = toDocument(local->data, name, system); doc.erase("schema_version"); doc["base_rev"] = rev;
        auto r = call("/api/v1/cards" + (id.empty() ? "" : "/" + id), id.empty() ? "POST" : "PUT", doc.dump());
        if (r.status != 200 && r.status != 201) {
            if (r.status == 401) sessions.erase(ctx);
            return failure(r); // In particular, NEVER retry 409 with a newer base_rev.
        }
        if (!validRemote(r.body, id)) return result("bad_response");
        auto saved = cards.importSnapshot(msg.senderId, local->name, fromDocument(r.body), local);
        if (!saved) {
            // On a first upload remember the remote ID even if .st raced the HTTP request.
            // On updates retain the previous merge base: the next sync still detects both edits.
            if (!old.is_object()) saveLink(msg, ctx, r.body, *local);
            return result("remote_saved");
        }
        saveLink(msg, ctx, r.body, *saved);
        return {"cloud_card.synced", {{"name", saved->name}, {"id", r.body["card_id"].get<std::string>()}, {"rev", r.body["rev"].dump()}}};
    }
};

Service::Service(Database& db, ConfigManager& cfg, CharacterCardStore& cards, Transport transport, Now now)
    : impl_(std::make_unique<Impl>(db, cfg, cards, std::move(transport), std::move(now))) {}
Service::~Service() {
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->stopping = true; impl_->jobs.clear();
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}
bool Service::dispatch(Message msg, std::string args, std::function<void(Result)> completed) {
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    if (impl_->stopping || impl_->jobs.size() >= 16) return false;
    if (!impl_->worker.joinable()) {
        impl_->worker = std::thread([this] {
            while (true) {
                Impl::Job job;
                {
                    std::unique_lock<std::mutex> guard(impl_->queueMutex);
                    impl_->wake.wait(guard, [this] { return impl_->stopping || !impl_->jobs.empty(); });
                    if (impl_->stopping) return;
                    job = std::move(impl_->jobs.front()); impl_->jobs.pop_front();
                }
                auto out = handle(job.msg, job.args);
                try { job.completed(std::move(out)); } catch (...) { /* Never log authorization data. */ }
            }
        });
    }
    impl_->jobs.push_back({std::move(msg), std::move(args), std::move(completed)});
    impl_->wake.notify_one();
    return true;
}
Result Service::handle(const Message& msg, const std::string& args) {
    std::unique_lock<std::mutex> lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return result("busy");
    try { return impl_->handle(msg, args); }
    catch (...) { return result("bad_response"); } // No token/document contents in logs or exceptions.
}
} // namespace dice::cloud_cards
