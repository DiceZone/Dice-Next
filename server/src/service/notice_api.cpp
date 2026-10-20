#include "notice_api.h"
#include "../storage/database.h"
#include "identity_email_service.h"
#include "notice_manager.h"
#include "notice_search.h"
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

namespace dice {
namespace {
using Json = nlohmann::json;
using Req = const drogon::HttpRequestPtr &;
using CB = std::function<void(const drogon::HttpResponsePtr &)>;
namespace orm = sqlite_orm;
void reply(CB &&cb, const Json &data) {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->addHeader("Cache-Control", "no-store");
    r->setBody(Json{{"code", 0}, {"data", data}}.dump());
    cb(r);
}
void fail(CB &&cb, const std::exception &e) {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setStatusCode(drogon::k400BadRequest);
    r->setBody(Json{{"code", 1}, {"message", e.what()}}.dump());
    cb(r);
}
int number(const std::string &s, int value, int max) {
    try {
        size_t n = 0;
        const int v = std::stoi(s, &n);
        return n == s.size() ? std::clamp(v, 1, max) : value;
    } catch (...) {
        return value;
    }
}
std::string lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string label(Database &db, AdapterManager &adapters, const Json &ch) {
    const auto p = ch.value("platform", std::string()), id = ch.value("chat_id", std::string());
    const bool group = ch.value("is_group", true);
    for (auto &a : adapters.allAdapters())
        if (a && (p.empty() || a->platform() == p)) {
            auto name = group ? a->cachedGroupName(id) : a->cachedUserName(id);
            if (!name.empty() && name != id && name != "用户")
                return name;
        }
    if (auto *st = db.getStorage())
        try {
            if (group) {
                auto rows = st->get_all<GroupSettingRow>(
                    orm::where(orm::c(&GroupSettingRow::platform) == p &&
                               orm::c(&GroupSettingRow::groupId) == id &&
                               orm::c(&GroupSettingRow::key) == std::string("name")),
                    orm::limit(1));
                if (!rows.empty() && !rows[0].value.empty() && rows[0].value != id)
                    return rows[0].value;
            } else {
                auto rows =
                    st->get_all<PlayerProfileRow>(orm::where(orm::c(&PlayerProfileRow::platform) == p &&
                                                             orm::c(&PlayerProfileRow::userId) == id),
                                                  orm::limit(1));
                if (!rows.empty() && !rows[0].nickname.empty() && rows[0].nickname != id)
                    return rows[0].nickname;
            }
        } catch (...) {
        }
    auto name = ch.value("name", std::string());
    return name == id ? std::string() : name;
}
void save(ConfigManager &cfg, const Json &before, const Json &after) {
    cfg.set<Json>("dice/notice", after);
    if (!cfg.save()) {
        cfg.set<Json>("dice/notice", before);
        throw std::runtime_error("保存通知配置失败");
    }
}
} // namespace
void registerNoticeRuleRoutes(Database &db, ConfigManager &cfg, AdapterManager &adapters) {
    auto &app = drogon::app();
    app.registerHandler("/api/system/notice/rules",
                        [&db, &cfg, &adapters](Req req, CB &&cb) {
                            try {
                                std::lock_guard lock(notice_rules::configMutex());
                                auto conf = notice::conf(cfg);
                                if (req->method() == drogon::Post) {
                                    if (req->body().size() > 1024 * 1024)
                                        throw std::invalid_argument("通知规则过大");
                                    auto r = notice_rules::normalize(Json::parse(req->body()));
                                    r["id"] = "rule-" + drogon::utils::genRandomString(24);
                                    auto next = conf;
                                    if (!next.contains("rules") || !next["rules"].is_array())
                                        next["rules"] = Json::array();
                                    next["rules"].push_back(r);
                                    save(cfg, conf, next);
                                    reply(std::move(cb), r);
                                    return;
                                }
                                Json rows = Json::array();
                                const auto q = lower(req->getParameter("q"));
                                auto enrich = [&](Json &r) {
                                    for (auto &ch : r["channels"])
                                        if (ch.value("kind", std::string()) == "chat")
                                            ch["display_name"] = label(db, adapters, ch);
                                };
                                for (auto r : notice_rules::all(conf)) {
                                    if (!q.empty())
                                        enrich(r);
                                    if (notice_rules::matchesSearch(r,q))
                                        rows.push_back(std::move(r));
                                }
                                const int size = number(req->getParameter("pageSize"), 20, 50),
                                          total = static_cast<int>(rows.size());
                                const int page = (std::min)(number(req->getParameter("page"), 1, 1000000),
                                                            (std::max)(1, (total + size - 1) / size));
                                Json items = Json::array();
                                for (int i = (page - 1) * size; i < (std::min)(page * size, total); ++i) {
                                    auto r = rows[i];
                                    if (q.empty())
                                        enrich(r);
                                    items.push_back(std::move(r));
                                }
                                reply(std::move(cb),
                                      {{"items", items}, {"total", total}, {"page", page}, {"size", size}});
                            } catch (const std::exception &e) {
                                fail(std::move(cb), e);
                            }
                        },
                        {drogon::Get, drogon::Post});
    app.registerHandler("/api/system/notice/rules/{1}",
                        [&cfg](Req req, CB &&cb, const std::string &id) {
                            try {
                                std::lock_guard lock(notice_rules::configMutex());
                                auto before = notice::conf(cfg), next = before;
                                if (!notice_rules::remove(next, id))
                                    throw std::invalid_argument("通知规则不存在，请刷新列表");
                                if (req->method() != drogon::Delete) {
                                    if (req->body().size() > 1024 * 1024)
                                        throw std::invalid_argument("通知规则过大");
                                    auto r = notice_rules::normalize(Json::parse(req->body()));
                                    r["id"] = id.rfind("rule-", 0) == 0
                                                  ? id
                                                  : "rule-" + drogon::utils::genRandomString(24);
                                    next["rules"].push_back(r);
                                }
                                save(cfg, before, next);
                                reply(std::move(cb), Json::object());
                            } catch (const std::exception &e) {
                                fail(std::move(cb), e);
                            }
                        },
                        {drogon::Put, drogon::Delete});
    app.registerHandler(
        "/api/system/notice/meta",
        [&cfg, &adapters](Req, CB &&cb) {
            try {
                Json accounts = Json::array(), events = Json::array();
                for (const auto &[op, area] : notice_rules::catalog())
                    events.push_back({{"op", op}, {"area", area}});
                for (const auto &a : cfg.getAll().value("adapters", Json::array())) {
                    const auto id =
                        a["id"].is_string() ? a["id"].get<std::string>() : std::to_string(a["id"].get<int>());
                    const auto live = adapters.getAdapter(id);
                    accounts.push_back({{"id", id},
                                        {"platform", a.value("type", std::string("onebot_v11"))},
                                        {"name", a.value("name", std::string())},
                                        {"login_id", live ? live->getLoginId() : std::string()},
                                        {"login_name", live ? live->getLoginName() : std::string()}});
                }
                auto transport =
                    notice_rules::smtpTransport(cfg.getAll(), notice::conf(cfg), {{"kind", "smtp"}});
                reply(std::move(cb), {{"accounts", accounts},
                                      {"catalog", events},
                                      {"smtp_ready", identity_email::validSettings(transport)},
                                      {"smtp_host", transport.value("host", std::string())},
                                      {"smtp_from", transport.value("from", std::string())}});
            } catch (const std::exception &e) {
                fail(std::move(cb), e);
            }
        },
        {drogon::Get});
    app.registerHandler(
        "/api/system/notice/targets",
        [&db, &adapters](Req req, CB &&cb) {
            try {
                auto *st = db.getStorage();
                if (!st)
                    throw std::runtime_error("数据库不可用");
                const bool group = req->getParameter("kind") == "group";
                const auto q = "%" + req->getParameter("q") + "%";
                const int size = number(req->getParameter("pageSize"), 10, 20),
                          page = number(req->getParameter("page"), 1, 100000);
                Json rows = Json::array();
                int total = 0;
                if (group) {
                    const auto filter =
                        orm::c(&GroupSettingRow::key) == std::string("name") &&
                        (orm::like(&GroupSettingRow::value, q) || orm::like(&GroupSettingRow::groupId, q));
                    total = st->count<GroupSettingRow>(orm::where(filter));
                    for (const auto &g : st->get_all<GroupSettingRow>(
                             orm::where(filter), orm::order_by(&GroupSettingRow::groupId),
                             orm::limit(size, orm::offset((page - 1) * size))))
                        rows.push_back({{"platform", g.platform},
                                        {"chat_id", g.groupId},
                                        {"name", g.value},
                                        {"is_group", true},
                                        {"kind", "chat"},
                                        {"delivery", "auto"},
                                        {"adapter_ids", Json::array()}});
                } else {
                    const auto filter =
                        orm::like(&PlayerProfileRow::nickname, q) || orm::like(&PlayerProfileRow::userId, q);
                    total = st->count<PlayerProfileRow>(orm::where(filter));
                    for (const auto &p : st->get_all<PlayerProfileRow>(
                             orm::where(filter), orm::order_by(&PlayerProfileRow::userId),
                             orm::limit(size, orm::offset((page - 1) * size))))
                        rows.push_back({{"platform", p.platform},
                                        {"chat_id", p.userId},
                                        {"name", p.nickname},
                                        {"is_group", false},
                                        {"kind", "chat"},
                                        {"delivery", "auto"},
                                        {"adapter_ids", Json::array()}});
                }
                for (auto &channel : rows)
                    channel["display_name"] = label(db, adapters, channel);
                reply(std::move(cb), {{"items", rows}, {"total", total}, {"page", page}, {"pageSize", size}});
            } catch (const std::exception &e) {
                fail(std::move(cb), e);
            }
        },
        {drogon::Get});
}
} // namespace dice
