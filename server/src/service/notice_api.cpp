#include "notice_api.h"
#include "../storage/database.h"
#include "identity_email_service.h"
#include "notice_manager.h"
#include "notice_http.h"
#include "notice_windows.h"
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
void validateDestination(ConfigManager& cfg, AdapterManager& adapters, const Json& window) {
    const auto& channel = window["channel"];
    if (channel["kind"] == "smtp") {
        const auto transport = notice_rules::smtpTransport(cfg.getAll(), notice::conf(cfg), channel);
        if (!identity_email::validSettings(transport)) throw std::invalid_argument("SMTP 配置无效，请先前往权限设置配置邮件服务器");
    }
    if (channel["kind"] != "chat") return;
    const auto platform = channel["platform"].get<std::string>();
    for (const auto& policy : window["subscriptions"]) {
        const auto sender = policy.value("sender", Json::object());
        if (sender.value("delivery", std::string("auto")) != "selected") continue;
        for (const auto& id : sender.value("adapter_ids", Json::array())) {
            const auto account = id.get<std::string>(); const auto live = adapters.getAdapter(account);
            bool found = live && live->platform() == platform;
            for (const auto& configured : cfg.getAll().value("adapters", Json::array())) {
                const auto value = configured.value("id", Json());
                if ((value.is_string() ? value.get<std::string>() : value.dump()) == account && configured.value("type", std::string()) == platform) found = true;
            }
            if (!found) throw std::invalid_argument("请选择属于通知窗口平台的有效发送帐号");
        }
    }
}
void registerWindows(Database& db, ConfigManager& cfg, AdapterManager& adapters) {
    auto decorate = [&db, &adapters](Json& window) {
        auto& channel = window["channel"];
        if (channel.value("kind", std::string()) == "chat") channel["display_name"] = label(db, adapters, channel);
    };
    drogon::app().registerHandler("/api/system/notice/windows", [&cfg, &adapters, decorate](Req req, CB&& cb) {
        try {
            std::lock_guard lock(notice_rules::configMutex()); const auto before = notice::conf(cfg);
            if (req->method() == drogon::Post) {
                if (req->body().size() > 1024 * 1024) throw std::invalid_argument("通知窗口配置过大");
                auto window = notice_windows::normalize(Json::parse(req->body())); const auto key = notice_windows::key(window["channel"]);
                for (const auto& existing : notice_windows::all(before))
                    if (notice_windows::key(existing["channel"]) == key) throw std::invalid_argument("该通知窗口已存在，请编辑已有窗口");
                if (window["channel"]["kind"] == "chat" && window["channel"].value("delivery", std::string("auto")) != "selected")
                    throw std::invalid_argument("请选择通知窗口使用的发送帐号");
                validateDestination(cfg, adapters, window);
                auto next = before; notice_windows::store(next, window, "rule-" + drogon::utils::genRandomString(24)); save(cfg, before, next);
                notice::webManagementNotice(req, cfg, adapters, [&](const auto&) { return notice::ruleChanged(window["name"].get<std::string>(), "新增", 1); });
                reply(std::move(cb), {{"saved", true}}); return;
            }
            Json rows = Json::array(); const auto q = lower(req->getParameter("q"));
            for (auto window : notice_windows::all(before)) {
                decorate(window);
                Json searchable{{"name", window["name"]}, {"channels", Json::array({window["channel"]})}, {"subscriptions", window["subscriptions"]}};
                if (notice_rules::matchesSearch(searchable, q)) rows.push_back(std::move(window));
            }
            const int size = number(req->getParameter("pageSize"), 10, 50), total = static_cast<int>(rows.size());
            const int page = (std::min)(number(req->getParameter("page"), 1, 1000000), (std::max)(1, (total + size - 1) / size));
            Json items = Json::array();
            for (int i = (page - 1) * size; i < (std::min)(page * size, total); ++i) items.push_back(rows[i]);
            reply(std::move(cb), {{"items", items}, {"total", total}, {"page", page}, {"size", size}});
        } catch (const std::exception& e) { fail(std::move(cb), e); }
    }, {drogon::Get, drogon::Post});
    drogon::app().registerHandler("/api/system/notice/windows/{1}", [&cfg, &adapters](Req req, CB&& cb, const std::string& id) {
        try {
            std::lock_guard lock(notice_rules::configMutex()); const auto before = notice::conf(cfg);
            Json old; for (const auto& w : notice_windows::all(before)) if (w["id"] == id) { old = w; break; }
            if (old.is_null()) throw std::invalid_argument("通知窗口已变更或不存在，请刷新列表");
            auto next = before; notice_windows::erase(next, old["channel"]);
            Json window = old;
            if (req->method() != drogon::Delete) {
                if (req->body().size() > 1024 * 1024) throw std::invalid_argument("通知窗口配置过大");
                window = notice_windows::normalize(Json::parse(req->body()));
                for (const auto& other : notice_windows::all(next)) if (notice_windows::key(other["channel"]) == notice_windows::key(window["channel"]))
                    throw std::invalid_argument("该通知窗口已存在，请编辑已有窗口");
                validateDestination(cfg, adapters, window);
                notice_windows::store(next, window, "rule-" + drogon::utils::genRandomString(24));
            }
            save(cfg, before, next);
            notice::webManagementNotice(req, cfg, adapters, [&](const auto&) { return notice::ruleChanged(window.value("name", std::string()), req->method() == drogon::Delete ? "删除" : "修改", 1); });
            reply(std::move(cb), {{"saved", true}});
        } catch (const std::exception& e) { fail(std::move(cb), e); }
    }, {drogon::Put, drogon::Delete});
    drogon::app().registerHandler("/api/system/notice/windows/{1}/test", [&cfg, &adapters](Req, CB&& cb, const std::string& id) {
        try {
            Json window; { std::lock_guard lock(notice_rules::configMutex());
                for (const auto& w : notice_windows::all(notice::conf(cfg))) if (w["id"] == id) { window = w; break; }
            }
            if (window.is_null()) throw std::invalid_argument("通知窗口不存在，请刷新列表");
            validateDestination(cfg, adapters, window);
            std::set<std::string> ids; for (const auto& value : window["rule_ids"]) ids.insert(value.get<std::string>());
            notice::notify(cfg, adapters, notice::kImportant, "这是一条测试通知。", "", "", "test", "", "", "", &ids, &window["channel"]);
            reply(std::move(cb), {{"submitted", true}});
        } catch (const std::exception& e) { fail(std::move(cb), e); }
    }, {drogon::Post});
}
} // namespace
void registerNoticeRuleRoutes(Database &db, ConfigManager &cfg, AdapterManager &adapters) {
    registerWindows(db, cfg, adapters);
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
                                    notice::webManagementNotice(req, cfg, adapters, [&](const auto&) {
                                        return notice::ruleChanged(r.value("name", std::string()), "新增", r["channels"].size());
                                    });
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
                        [&cfg, &adapters](Req req, CB &&cb, const std::string &id) {
                            try {
                                std::lock_guard lock(notice_rules::configMutex());
                                auto before = notice::conf(cfg), next = before;
                                Json changed;
                                for (const auto& rule : notice_rules::all(before))
                                    if (rule.value("id", std::string()) == id) changed = rule;
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
                                    changed = r;
                                }
                                save(cfg, before, next);
                                notice::webManagementNotice(req, cfg, adapters, [&](const auto&) {
                                    return notice::ruleChanged(changed.value("name", std::string()),
                                        req->method() == drogon::Delete ? "删除" : "修改", changed["channels"].size());
                                });
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
                                      {"legacy_smtp_ready", identity_email::validSettings(notice::conf(cfg).value("smtp", Json::object()))},
                                      {"webhook_profiles", Json::array({"generic", "wecom", "dingtalk", "feishu", "custom"})},
                                      {"webhook_variables", notice_webhook::variables()},
                                      {"smtp_host", transport.value("host", std::string())},
                                      {"smtp_from", transport.value("from", std::string())}});
            } catch (const std::exception &e) {
                fail(std::move(cb), e);
            }
        },
        {drogon::Get});
    app.registerHandler(
        "/api/system/notice/targets",
        [&db, &adapters, &cfg](Req req, CB &&cb) {
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
                const auto windows = notice_windows::all(notice::conf(cfg));
                for (auto &channel : rows) {
                    channel["display_name"] = label(db, adapters, channel);
                    for (const auto& window : windows) if (notice_windows::key(window["channel"]) == notice_windows::key(channel)) {
                        channel["notice_window_id"] = window["id"]; break;
                    }
                }
                reply(std::move(cb), {{"items", rows}, {"total", total}, {"page", page}, {"pageSize", size}});
            } catch (const std::exception &e) {
                fail(std::move(cb), e);
            }
        },
        {drogon::Get});
}
} // namespace dice
