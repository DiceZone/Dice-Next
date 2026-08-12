#pragma once
// ─── Dice!Next v3.0.0 — Real API Service ─────────────────────
// Direct Drogon handler implementations backed by Database + ConfigManager.
// Provides full CRUD for adapters, replies, dice rules, and system status.

#include "../storage/database.h"
#include "../storage/group_account_settings.h"
#include "../config/config_manager.h"
#include "../config/scoped_settings.h"
#include "../core/dice/dice_engine.h"
#include "../core/dice/dice_expression.h"
#include "../core/dice/dice_rules.h"
#include "../common/utils.h"
#include "../common/version.h"
#include "../adapter/adapter_interface.h"
#include "../adapter/adapter_manager.h"
#include "../adapter/onebot_v11_adapter.h"
#include "../adapter/qq_official_adapter.h"
#include "../adapter/discord_adapter.h"
#include "../adapter/kook_adapter.h"
#include "../core/deck/card_deck.h"
#include "../core/reply/reply_manager.h"
#include "../core/mod/js_plugin_manager.h"
#include "../core/mod/lua_plugin_manager.h"
#include "../core/command_router.h"
#include "../core/identity/identity_binding.h"
#include "../platform/system_info.h"
#include "../i18n/i18n.h"
#include "../common/types.h"
#include "group_chat_buffer.h"
#include "broadcast_manager.h"
#include "log_service.h"
#include "ai_gateway.h"     // AI 网关
#include "ai_polish.h"      // 润色（默认提示词）
#include "ai_translate.h"   // 翻译（默认提示词）
#include "ai_chat.h"        // 智能化阶段A：对话（默认提示词）
#include "ai_memory.h"      // 智能化阶段B：记忆（摘要默认提示词）
#include "ai_npc.h"         // 智能化阶段E：NPC 扮演
#include "ai_vision.h"      // 多模态图像识别（默认提示词）
#include "notice_manager.h" // B：通知系统（全局开关变更推送）
#include "heart_service.h"  // 心跳上报（heart.dice.zone）
#include "cloudban_service.h" // 云黑名单同步（cloudban.dice.zone）
#include "backup_service.h"
#include "../storage/legacy_import_v2.h"
#include "../core/causal/causal_rule_manager.h"
#include "../core/causal/cooldown_manager.h"
#include "../core/causal/counter_store.h"
#include "../core/persona/persona_manager.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>   // base64Encode（网页带图导出内嵌图片）
#include <nlohmann/json.hpp>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <ctime>
#include <thread>
#include <chrono>
#include <cstdint>

namespace dice::api {

// 路径 → UTF-8 窄串（Windows .string() 走 ANSI，特殊字符文件名会抛；u8string 永不抛）。
static inline std::string dnx_u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

using J = nlohmann::json;
using CB = std::function<void(const drogon::HttpResponsePtr&)>;
using Req = const drogon::HttpRequestPtr&;

static J ok(const J& d = nullptr) { return J{{"code",0},{"message","ok"},{"data",d}}; }
static J fail(const std::string& msg, int c = 1) { return J{{"code",c},{"message",msg},{"data",nullptr}}; }
static void jsonReply(const J& j, CB&& cb) {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    // error_handler::replace：遇到非法 UTF-8 字节用 U+FFFD 替换而非抛异常，
    // 避免单条脏数据（如旧库里编码异常的备注）导致整个接口 500。
    r->setBody(j.dump(-1, ' ', false, J::error_handler_t::replace));
    cb(r);
}

static MatchType matchTypeFromStr(const std::string& mt) {
    if (mt == "prefix") return MatchType::kPrefix;
    if (mt == "regex")  return MatchType::kRegex;
    if (mt == "search") return MatchType::kSearch;
    return MatchType::kKeyword;
}

// Build a ReplyRule from the API JSON. Accepts the enhanced shape
// (conditions[]/logic/results[]) and falls back to the legacy single fields.
static ReplyRule replyRuleFromJson(const J& j) {
    ReplyRule rule;
    rule.priority = j.value("priority", 100);
    rule.enabled  = j.value("enabled", true);
    rule.logic    = j.value("logic", std::string("or")) == "and" ? "and" : "or";
    if (j.contains("conditions") && j["conditions"].is_array() && !j["conditions"].empty()) {
        for (auto& c : j["conditions"]) {
            std::string content = c.value("content", std::string());
            if (content.empty()) continue;
            rule.conditions.push_back({matchTypeFromStr(c.value("type", std::string("keyword"))), content});
        }
    }
    if (rule.conditions.empty())
        rule.conditions.push_back({matchTypeFromStr(j.value("matchType", std::string("keyword"))),
                                   j.value("matchContent", std::string())});
    if (j.contains("results") && j["results"].is_array() && !j["results"].empty()) {
        for (auto& r : j["results"]) if (r.is_string() && !r.get<std::string>().empty())
            rule.results.push_back(r.get<std::string>());
    }
    if (rule.results.empty()) rule.results.push_back(j.value("replyContent", std::string()));
    rule.matchType = rule.conditions[0].type;
    rule.matchContent = rule.conditions[0].content;
    rule.replyContent = rule.results[0];
    // 触发限制（原版 DiceTriggerLimit 常用子集）。
    rule.prob        = (std::clamp)(j.value("prob", 100), 0, 100);
    rule.cooldownSec = (std::max)(0, j.value("cooldownSec", 0));
    rule.scopeMode   = j.value("scopeMode", std::string());
    if (rule.scopeMode != "allow" && rule.scopeMode != "deny") rule.scopeMode.clear();
    rule.scopeIds    = j.value("scopeIds", std::string());
    rule.cooldownNotice = j.value("cooldownNotice", std::string());
    rule.dayLimit       = (std::max)(0, j.value("dayLimit", 0));
    rule.dayLimitNotice = j.value("dayLimitNotice", std::string());
    rule.scopeUsersMode = j.value("scopeUsersMode", std::string());
    if (rule.scopeUsersMode != "allow" && rule.scopeUsersMode != "deny") rule.scopeUsersMode.clear();
    rule.scopeUsers     = j.value("scopeUsers", std::string());
    return rule;
}

// 保存前校验规则（返回空串=通过）。正则写错以前会静默存库，变成永不命中的死规则。
static std::string replyRuleValidate(const ReplyRule& rule) {
    for (const auto& c : rule.conditions) {
        if (c.type == MatchType::kRegex) {
            std::string err;
            if (!ReplyMatcher::validateRegex(c.content, &err))
                return "invalid regex '" + c.content + "': " + err;
        }
    }
    bool hasResult = false;
    for (const auto& r : rule.results) if (!r.empty()) { hasResult = true; break; }
    if (!hasResult) return "reply content required";
    if (rule.conditions.empty() || rule.conditions[0].content.empty()) return "match content required";
    return {};
}

static J adapterToJson(const AdapterRow& a, const std::string& lastActive = std::string()) {
    J cfg = J::parse(a.config, nullptr, false);
    if (!cfg.is_object()) cfg = J::object();
    const bool official = a.type == static_cast<int>(AdapterType::kQQOfficial);
    const char* typeStr = adapterTypeToString(static_cast<AdapterType>(a.type));
    const std::string heartApiKey = cfg.value("heartApiKey", std::string());
    return J{
        {"id", std::to_string(a.id)},
        {"name", a.name},
        {"type", std::string(typeStr) == "unknown" ? "onebot_v11" : typeStr},
        {"connectionMode", a.connectionMode == 0 ? "forward_ws" : a.connectionMode == 1 ? "reverse_ws" : "http"},
        {"endpoint", a.endpoint},
        {"accessToken", official ? "" : a.accessToken},
        {"appId", official && cfg.is_object() ? cfg.value("appId", std::string()) : std::string()},
        {"qqNumber", official && cfg.is_object() ? cfg.value("qqNumber", std::string()) : std::string()},
        {"forceVerifyImageResource", official && cfg.is_object() ? cfg.value("forceVerifyImageResource", false) : false},
        {"heartApiKeyConfigured", !heartApiKey.empty()},
        {"heartApiKeyTail", heartApiKey.size() > 4 ? heartApiKey.substr(heartApiKey.size() - 4) : std::string()},
        {"enabled", a.enabled},
        {"status", "disconnected"},
        {"lastActive", lastActive.empty() ? J(nullptr) : J(lastActive)},
        {"createdAt", "2026-06-14T00:00:00.000Z"}
    };
}

static J adapterToConfigJson(const AdapterRow& a) {
    J extra = J::parse(a.config, nullptr, false); if (!extra.is_object()) extra = J::object();
    J out{{"id", a.id}, {"name", a.name}, {"type", adapterTypeToString(static_cast<AdapterType>(a.type))},
          {"connection_mode", a.connectionMode == 1 ? "reverse_ws" : a.connectionMode == 2 ? "http" : "forward_ws"},
          {"endpoint", a.endpoint}, {"access_token", a.accessToken}, {"enabled", a.enabled},
          {"heart_api_key", extra.value("heartApiKey", std::string())}};
    if (a.type == static_cast<int>(AdapterType::kQQOfficial)) {
        out["app_id"] = extra.value("appId", std::string());
        out["app_secret"] = extra.value("appSecret", std::string());
        out["qq_number"] = extra.value("qqNumber", std::string());
        out["force_verify_image_resource"] = extra.value("forceVerifyImageResource", false);
    }
    return out;
}

template <typename Storage>
static void persistAdaptersToConfig(Storage* st, ConfigManager& cfg) {
    J adapters = J::array();
    for (const auto& adapter : st->template get_all<AdapterRow>()) adapters.push_back(adapterToConfigJson(adapter));
    cfg.set<J>("adapters", adapters);
    if (!cfg.save()) throw std::runtime_error("适配器已变更，但写入 adapters.json 失败");
}

static AdapterPtr makeRuntimeAdapter(const AdapterRow& a) {
    if (a.type == static_cast<int>(AdapterType::kQQOfficial)) {
        J cfg = J::parse(a.config, nullptr, false);
        if (!cfg.is_object()) cfg = J::object();
        auto adapter = std::make_shared<QQOfficialAdapter>(std::to_string(a.id));
        adapter->configure({{"name", a.name}, {"appId", cfg.value("appId", std::string())},
                            {"appSecret", cfg.value("appSecret", std::string())},
                            {"qqNumber", cfg.value("qqNumber", std::string())},
                            {"forceVerifyImageResource", cfg.value("forceVerifyImageResource", false)},
                            {"message_format", cfg.value("message_format", std::string())}});
        return adapter;
    }
    if (a.type == static_cast<int>(AdapterType::kDiscord)) {
        J cfg = J::parse(a.config, nullptr, false);
        if (!cfg.is_object()) cfg = J::object();
        auto adapter = std::make_shared<DiscordAdapter>(std::to_string(a.id));
        adapter->configure({{"name", a.name}, {"token", a.accessToken},
                            {"message_format", cfg.value("message_format", std::string())}});
        return adapter;
    }
    if (a.type == static_cast<int>(AdapterType::kKook)) {
        J cfg = J::parse(a.config, nullptr, false);
        if (!cfg.is_object()) cfg = J::object();
        auto adapter = std::make_shared<KookAdapter>(std::to_string(a.id));
        adapter->configure({{"name", a.name}, {"token", a.accessToken},
                            {"message_format", cfg.value("message_format", std::string())}});
        return adapter;
    }
    auto adapter = std::make_shared<OneBotV11Adapter>(std::to_string(a.id));
    J cfg = J::parse(a.config, nullptr, false);
    if (!cfg.is_object()) cfg = J::object();
    std::string mode = (a.connectionMode == 1) ? "reverse_ws" : (a.connectionMode == 2) ? "http" : "forward_ws";
    adapter->configure({{"name", a.name}, {"endpoint", a.endpoint},
                        {"accessToken", a.accessToken}, {"connectionMode", mode},
                        {"message_format", cfg.value("message_format", std::string())}});
    return adapter;
}

static J replyToJson(const ReplyRuleRow& r) {
    const char* modes[] = {"keyword","prefix","regex","search"};
    // conditions[]: parse stored JSON, else synthesize from the legacy fields.
    J conditions = J::array();
    if (!r.conditions.empty()) {
        try { J a = J::parse(r.conditions); if (a.is_array()) conditions = a; } catch (...) {}
    }
    if (conditions.empty())
        conditions.push_back(J{{"type", modes[static_cast<size_t>(r.matchType) % 4]}, {"content", r.matchContent}});
    // results[]: parse stored JSON, else the single replyContent.
    J results = J::array();
    if (!r.results.empty()) {
        try { J a = J::parse(r.results); if (a.is_array()) results = a; } catch (...) {}
    }
    if (results.empty()) results.push_back(r.replyContent);
    return J{
        {"id", std::to_string(r.id)},
        {"matchType", modes[static_cast<size_t>(r.matchType) % 4]},
        {"matchContent", r.matchContent},
        {"replyContent", r.replyContent},
        {"conditions", conditions},
        {"logic", r.logic == "and" ? "and" : "or"},
        {"results", results},
        {"priority", r.priority},
        {"enabled", r.enabled},
        {"prob", r.prob},
        {"cooldownSec", r.cooldownSec},
        {"scopeMode", r.scopeMode},
        {"scopeIds", r.scopeIds},
        {"cooldownNotice", r.cooldownNotice},
        {"dayLimit", r.dayLimit},
        {"dayLimitNotice", r.dayLimitNotice},
        {"scopeUsersMode", r.scopeUsersMode},
        {"scopeUsers", r.scopeUsers},
        {"createdAt", r.createdAt.empty() ? "2026-06-14T00:00:00.000Z" : r.createdAt},
        {"updatedAt", r.updatedAt.empty() ? "2026-06-14T00:00:00.000Z" : r.updatedAt}
    };
}

// Host CPU / memory / disk snapshot for the dashboard server-info panel and the
// live /api/system/sysinfo poll. Static hardware details are cached in gather().
static J sysInfoJson() {
    auto si = dice::sysinfo::gather();
    J disks = J::array();
    for (const auto& d : si.disks) {
        disks.push_back(J{
            {"mount", d.mount}, {"label", d.label}, {"fs", d.fs}, {"model", d.model},
            {"total_gb", d.totalGB}, {"used_gb", d.usedGB}, {"load", d.loadPct},
        });
    }
    return J{
        {"os", si.os}, {"os_id", si.osId},
        {"cpu_model", si.cpuModel}, {"cpu_cores", si.cpuCores}, {"cpu_physical", si.cpuPhysical},
        {"cpu_mhz", si.cpuMhz}, {"cpu_load", si.cpuLoadPct},
        {"mem_total_mb", si.memTotalMB}, {"mem_used_mb", si.memUsedMB}, {"mem_load", si.memLoadPct},
        {"mem_speed_mhz", si.memSpeedMhz}, {"proc_mem_mb", si.procMemMB},
        {"disks", disks},
    };
}

// ═══════════════════════════════════════════════════════════════
// Public: register all API routes on the Drogon app
// ═══════════════════════════════════════════════════════════════

inline void registerApiRoutes(Database& db, ConfigManager& cfg, AdapterManager& adapterMgr,
                              DiceEngine& engine, CardDeck& cardDeck, ReplyManager& replyMgr, I18n& i18n,
                              JsPluginManager& jsMod, LuaPluginManager& luaMod,
                              CausalRuleManager& causalMgr, CooldownManager& cooldownMgr,
                              CounterStore& counterStore, PersonaManager& personaMgr) {
    auto& app = drogon::app();
    auto* st = db.getStorage();
    auto* lst = db.getLogStorage();   // game_logs / game_log_messages live in logs.db

    // ── System ────────────────────────────────────────────────
    app.registerHandler("/api/system/status", [](Req, CB&& cb) {
        std::string bn = std::to_string(buildNumber());
        while (bn.size() < 3) bn = "0" + bn;
        jsonReply(ok(J{
            {"status","running"},
            {"version",versionString()},
            {"buildNumber",bn},
            {"buildTime",buildTime()},
            {"uptime", static_cast<int>(std::time(nullptr) - utils::getStartupEpoch())}
        }), std::move(cb));
    }, {drogon::Get});

    // ── JS 插件管理 ────────────────────────────────────────────
    // Management for plugins/js/*.js: list / upload / toggle / delete / reload.
    static auto pluginsJson = [](JsPluginManager& jm) {
        auto allCfg = jm.configs();   // metadata for register*Config items
        J arr = J::array();
        for (auto& p : jm.listAll()) {
            // Attach this plugin's config items (with current values) so the WebUI
            // can render an editable form per plugin.
            J cfgs = J::array();
            for (auto& c : allCfg) {
                if (c.file != p.file) continue;
                J item = J{{"ext", c.ext}, {"key", c.key}, {"type", c.type},
                           {"default", c.def}, {"description", c.description},
                           {"value", jm.kvGet(jm.configKey(c.ext, c.key), c.def)}};
                if (!c.optionsJson.empty()) {
                    try { item["options"] = J::parse(c.optionsJson); } catch (...) {}
                }
                cfgs.push_back(item);
            }
            // language from extension (strip a trailing .disabled first)
            std::string fn = p.file;
            if (fn.size() > 9 && fn.substr(fn.size() - 9) == ".disabled") fn = fn.substr(0, fn.size() - 9);
            std::string lang = "js";
            if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".lua") lang = "lua";
            else if (fn.size() > 3 && fn.substr(fn.size() - 3) == ".py") lang = "python";
            J item = J{{"name", p.name}, {"author", p.author}, {"version", p.version},
                       {"file", p.file}, {"description", p.desc}, {"lang", lang},
                       {"homepage", p.homepage}, {"updateUrl", p.updateUrl}, {"license", p.license},
                       {"commandList", p.commandList},
                       {"superseded", p.superseded}, {"supersededBy", p.supersededBy},
                       {"ruleCompat", p.ruleCompat}, {"inMod", p.inMod},
                       {"commands", p.commands}, {"enabled", p.enabled}, {"configs", cfgs}};
            if (auto owner = CommandRouter::pluginOwnerBundle("js:" + fn)) {
                item["ownerBundle"] = owner->first;
                item["ownerBundleFolder"] = owner->second;
            }
            arr.push_back(std::move(item));
        }
        return arr;
    };
    // Keep only the basename (defend against path traversal in filenames).
    static auto baseName = [](std::string f) -> std::string {
        auto p = f.find_last_of("/\\");
        return p == std::string::npos ? f : f.substr(p + 1);
    };
    // 按 UTF-8 构造路径：Windows 上 narrow ifstream/ofstream 把路径当系统码页(中文=GBK)，
    // 用它打开 UTF-8 的中文文件名会失败；改用 u8string 构造的 fs::path 即可正确开。
    static auto u8p = [](const std::string& s) {
        return std::filesystem::path(std::u8string(s.begin(), s.end()));
    };

    app.registerHandler("/api/plugins/js", [&jsMod](Req, CB&& cb) {
        jsonReply(ok(pluginsJson(jsMod)), std::move(cb));
    }, {drogon::Get});

    // Body: { items: [ { ext, key, value }, ... ] }. Saves plugin config values
    // (persisted to the JS KV store; plugins read them via get*Config). No reload
    // needed — get*Config reads live values.
    app.registerHandler("/api/plugins/js/config", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            if (!j.contains("items") || !j["items"].is_array()) { jsonReply(fail("items[] required"), std::move(cb)); return; }
            int n = 0;
            for (auto& it : j["items"]) {
                std::string ext = it.value("ext", std::string());
                std::string key = it.value("key", std::string());
                if (ext.empty() || key.empty()) continue;
                std::string val;
                if (it.contains("value")) {
                    val = it["value"].is_string() ? it["value"].get<std::string>() : it["value"].dump();
                }
                jsMod.setConfig(ext, key, val);
                ++n;
            }
            jsonReply(ok(J{{"saved", n}, {"plugins", pluginsJson(jsMod)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Body: { file }. Fetches the plugin's @updateUrl and compares versions.
    app.registerHandler("/api/plugins/js/check-update", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            auto info = jsMod.checkUpdate(file);
            jsonReply(ok(J{{"ok", info.ok}, {"hasUpdate", info.hasUpdate},
                          {"current", info.current}, {"latest", info.latest},
                          {"updateUrl", info.updateUrl}, {"error", info.error}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Body: { file }. Downloads the latest from @updateUrl, overwrites, reloads.
    app.registerHandler("/api/plugins/js/update", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            std::string err;
            if (!jsMod.updatePlugin(file, err)) { jsonReply(fail(err.empty() ? "update failed" : err), std::move(cb)); return; }
            int n = jsMod.reload(jsMod.pluginDir().empty() ? "data/plugins/js" : jsMod.pluginDir());
            CommandRouter::reloadJsGameSystems(jsMod.gameSystemTemplates());   // JS 规则插件属性模板同步
            jsonReply(ok(J{{"loaded", n}, {"plugins", pluginsJson(jsMod)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    app.registerHandler("/api/plugins/js/reload", [&jsMod](Req, CB&& cb) {
        int n = jsMod.reload(jsMod.pluginDir().empty() ? "data/plugins/js" : jsMod.pluginDir());
        jsonReply(ok(J{{"loaded", n}, {"plugins", pluginsJson(jsMod)}}), std::move(cb));
    }, {drogon::Post});

    // Body: { filename: "foo.js", content: "<js source>" }. Writes the file then reloads.
    app.registerHandler("/api/plugins/js/upload", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string fname = baseName(j.value("filename", std::string()));
            std::string content = j.value("content", std::string());
            if (fname.empty() || fname.size() < 4 || fname.substr(fname.size() - 3) != ".js") {
                jsonReply(fail("filename must end with .js"), std::move(cb)); return;
            }
            if (content.empty()) { jsonReply(fail("empty content"), std::move(cb)); return; }
            std::string dir = jsMod.pluginDir().empty() ? "data/plugins/js" : jsMod.pluginDir();
            std::error_code ec; std::filesystem::create_directories(dir, ec);
            { std::ofstream f(std::filesystem::path(dir) / fname, std::ios::binary); f << content; }
            int n = jsMod.reload(dir);
            CommandRouter::reloadJsGameSystems(jsMod.gameSystemTemplates());   // JS 规则插件属性模板同步
            jsonReply(ok(J{{"loaded", n}, {"plugins", pluginsJson(jsMod)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Body: { file: "foo.js" | "foo.js.disabled", enabled: bool }. Renames + reloads.
    app.registerHandler("/api/plugins/js/toggle", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            bool enabled = j.value("enabled", true);
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            std::string dir = jsMod.dirForFile(file);   // 文件可能在 data/mod
            namespace fs = std::filesystem;
            // Normalize to the base "*.js" name regardless of which form was sent.
            std::string base = file;
            if (base.size() > 9 && base.substr(base.size() - 9) == ".disabled") base = base.substr(0, base.size() - 9);
            fs::path on = fs::path(dir) / base, off = fs::path(dir) / (base + ".disabled");
            std::error_code ec;
            if (enabled) { if (fs::exists(off)) fs::rename(off, on, ec); }
            else         { if (fs::exists(on))  fs::rename(on, off, ec); }
            if (ec) { jsonReply(fail(ec.message()), std::move(cb)); return; }
            int n = jsMod.reload(dir);
            CommandRouter::reloadJsGameSystems(jsMod.gameSystemTemplates());   // JS 规则插件属性模板同步
            jsonReply(ok(J{{"loaded", n}, {"plugins", pluginsJson(jsMod)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Body: { file: "foo.js" | "foo.js.disabled" }. Deletes the file then reloads.
    app.registerHandler("/api/plugins/js/delete", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            std::string dir = jsMod.dirForFile(file);   // 文件可能在 data/mod
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::remove(fs::path(dir) / file, ec);  // remove exactly what was listed
            int n = jsMod.reload(dir);
            CommandRouter::reloadJsGameSystems(jsMod.gameSystemTemplates());   // JS 规则插件属性模板同步
            jsonReply(ok(J{{"loaded", n}, {"plugins", pluginsJson(jsMod)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 查看某插件存储的数据（ext.storage*，命名空间 ext:<ext.name>）。供详情页面板。
    // ext.name（注册名）可能 ≠ @name 显示名，故按文件解析其注册的 ext 名再并集。
    // GET ?file=<文件名> → { entries:[{ext,key,fullKey,value}], count }
    app.registerHandler("/api/plugins/js/storage", [&jsMod](Req req, CB&& cb) {
        std::string file = baseName(req->getParameter("file"));
        if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
        J entries = J::array();
        for (const auto& ext : jsMod.extNamesForFile(file)) {
            std::string ns = "ext:" + ext, prefix = ns + ":";
            for (auto& [k, v] : jsMod.kvByNamespace(ns)) {
                std::string shortKey = (k.rfind(prefix, 0) == 0) ? k.substr(prefix.size()) : k;
                entries.push_back(J{{"ext", ext}, {"key", shortKey}, {"fullKey", k}, {"value", v}});
            }
        }
        jsonReply(ok(J{{"count", entries.size()}, {"entries", entries}}), std::move(cb));
    }, {drogon::Get});

    // 清空某插件的存储数据（其注册的全部 ext:<name>）。POST { file }
    app.registerHandler("/api/plugins/js/storage/clear", [&jsMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            int n = 0;
            for (const auto& ext : jsMod.extNamesForFile(file)) n += jsMod.kvClearNamespace("ext:" + ext);
            jsonReply(ok(J{{"removed", n}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Console log display mode (human-readable vs raw OneBot JSON).
    app.registerHandler("/api/system/log-mode", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                bool raw = j.value("raw", false);
                OneBotV11Adapter::s_rawEventLog = raw;
                cfg.set<bool>("log/raw_events", raw);
                cfg.save();
            }
            jsonReply(ok(J{{"raw", OneBotV11Adapter::s_rawEventLog.load()}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 自响应消息（用骰娘账号自身发指令自控）。热更新到适配器静态开关。
    app.registerHandler("/api/system/respond-self", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                bool en = j.value("enabled", false);
                OneBotV11Adapter::s_respondSelf = en;
                cfg.set<bool>("dice/respond_self", en);
                cfg.save();
            }
            jsonReply(ok(J{{"enabled", OneBotV11Adapter::s_respondSelf.load()}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // AI 模型管理（配置 dice/ai）。GET 取配置；PUT 规范化后保存。
    app.registerHandler("/api/system/ai", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                J out = J::object();
                out["enabled"] = j.value("enabled", false);
                J ms = J::array();
                if (j.contains("models") && j["models"].is_array()) {
                    for (auto& m : j["models"]) {
                        if (!m.is_object()) continue;
                        J mm = J::object();
                        std::string id = m.value("id", std::string());
                        if (id.empty()) id = "m" + drogon::utils::genRandomString(8);
                        mm["id"] = id;
                        mm["name"] = m.value("name", std::string());
                        mm["base_url"] = m.value("base_url", std::string());
                        mm["api_key"] = m.value("api_key", std::string());
                        mm["model"] = m.value("model", std::string());
                        mm["enabled"] = m.value("enabled", true);
                        mm["price_in"] = m.value("price_in", 0.0);
                        mm["price_out"] = m.value("price_out", 0.0);
                        mm["token_limit"] = m.value("token_limit", (long long)0);
                        mm["cost_limit"] = m.value("cost_limit", 0.0);
                        // Preserve existing usage stats (don't reset on WebUI save)
                        auto existing = cfg.get<json>("dice/ai/models", json::array());
                        for (auto& em : existing) { if (em.is_object() && em.value("id", std::string()) == id) { mm["used_tokens"] = em.value("used_tokens", (long long)0); mm["used_cost"] = em.value("used_cost", 0.0); break; } }
                        ms.push_back(mm);
                    }
                }
                out["models"] = ms;
                // 覆盖范围规范化（roll/deck/fun/custom/plugin）。
                auto normCov = [](const J& src, bool defRoll, bool defOthers) {
                    const J& cv = (src.contains("cov") && src["cov"].is_object()) ? src["cov"] : J::object();
                    return J{{"roll", cv.value("roll", defRoll)}, {"deck", cv.value("deck", defOthers)},
                             {"fun", cv.value("fun", defOthers)}, {"custom", cv.value("custom", defOthers)},
                             {"plugin", cv.value("plugin", defOthers)}};
                };
                // 全局请求参数。
                J prm = (j.contains("params") && j["params"].is_object()) ? j["params"] : J::object();
                out["params"] = J{
                    {"temperature", prm.value("temperature", 0.7)},
                    {"top_p", prm.value("top_p", 1.0)},
                    {"max_tokens", prm.value("max_tokens", (long long)1024)},
                    {"frequency_penalty", prm.value("frequency_penalty", 0.0)},
                    {"presence_penalty", prm.value("presence_penalty", 0.0)}
                };
                // 润色配置（enabled / model_id / mode text|rp / persona / cov）。
                J pj = (j.contains("polish") && j["polish"].is_object()) ? j["polish"] : J::object();
                std::string pmode = pj.value("mode", std::string("text"));
                if (pmode != "text" && pmode != "rp") pmode = "text";
                out["polish"] = J{
                    {"enabled", pj.value("enabled", false)},
                    {"model_id", pj.value("model_id", std::string())},
                    {"mode", pmode},
                    {"persona", pj.value("persona", std::string())},
                    {"prompt", pj.value("prompt", std::string())},   // 可编辑系统提示词（空=内置默认）
                    {"cov", normCov(pj, true, false)}
                };
                // 翻译配置（enabled / model_id / cov / 自定义语言列表）。
                J tj = (j.contains("translate") && j["translate"].is_object()) ? j["translate"] : J::object();
                J langs = J::array();
                if (tj.contains("langs") && tj["langs"].is_array()) {
                    for (auto& l : tj["langs"]) {
                        if (!l.is_object()) continue;
                        std::string nm = l.value("name", std::string());
                        if (nm.empty()) continue;
                        J kws = J::array();
                        if (l.contains("keywords") && l["keywords"].is_array())
                            for (auto& kw : l["keywords"])
                                if (kw.is_string() && !kw.get<std::string>().empty()) kws.push_back(kw);
                        langs.push_back(J{{"name", nm}, {"keywords", kws}});
                    }
                }
                out["translate"] = J{
                    {"enabled", tj.value("enabled", false)},
                    {"model_id", tj.value("model_id", std::string())},
                    {"prompt", tj.value("prompt", std::string())},   // 可编辑翻译提示词（{lang} 占位，空=内置默认）
                    {"cov", normCov(tj, true, true)},
                    {"langs", langs}
                };
                // 智能化阶段A：对话配置。
                J chj = (j.contains("chat") && j["chat"].is_object()) ? j["chat"] : J::object();
                J ckws = J::array();
                if (chj.contains("keywords") && chj["keywords"].is_array())
                    for (auto& kw : chj["keywords"])
                        if (kw.is_string() && !kw.get<std::string>().empty()) ckws.push_back(kw);
                J cfilters = J::array();   // 用户自定义过滤词
                if (chj.contains("filters") && chj["filters"].is_array())
                    for (auto& fw : chj["filters"])
                        if (fw.is_string() && !fw.get<std::string>().empty()) cfilters.push_back(fw);
                int sprob = chj.value("standby_prob", 0); if (sprob < 0) sprob = 0; if (sprob > 100) sprob = 100;
                out["chat"] = J{
                    {"enabled", chj.value("enabled", false)},
                    {"model_id", chj.value("model_id", std::string())},
                    {"persona", chj.value("persona", std::string())},
                    {"prompt", chj.value("prompt", std::string())},
                    {"at_bot", chj.value("at_bot", true)},
                    {"keywords", ckws},
                    {"standby_prob", sprob},
                    {"context_rounds", chj.value("context_rounds", (long long)10)},
                    {"max_chars", chj.value("max_chars", (long long)200)},
                    {"cooldown_sec", chj.value("cooldown_sec", (long long)5)},
                    {"reply_at", chj.value("reply_at", false)},
                    {"no_emoji", chj.value("no_emoji", true)},   // 默认不发 emoji
                    {"filters", cfilters}                        // 用户自定义过滤词
                };
                // 智能化阶段B/C：记忆配置（memory.short 滚动摘要 + memory.long 长期事实向量检索）。
                J mmj = (j.contains("memory") && j["memory"].is_object()) ? j["memory"] : J::object();
                J msh = (mmj.contains("short") && mmj["short"].is_object()) ? mmj["short"] : J::object();
                int mrounds = msh.value("rounds", 20); if (mrounds < 4) mrounds = 4; if (mrounds > 200) mrounds = 200;
                int mmax = msh.value("max_chars", 400); if (mmax < 80) mmax = 80; if (mmax > 2000) mmax = 2000;
                J mlo = (mmj.contains("long") && mmj["long"].is_object()) ? mmj["long"] : J::object();
                int mtopk = mlo.value("top_k", 4); if (mtopk < 1) mtopk = 1; if (mtopk > 20) mtopk = 20;
                double msim = mlo.value("min_similarity", 0.75); if (msim < 0) msim = 0; if (msim > 1) msim = 1;
                int mcap = mlo.value("max_facts", 300); if (mcap < 20) mcap = 20; if (mcap > 5000) mcap = 5000;
                out["memory"] = J{
                    {"short", J{
                        {"enabled", msh.value("enabled", false)},
                        {"rounds", (long long)mrounds},
                        {"max_chars", (long long)mmax},
                        {"summary_model_id", msh.value("summary_model_id", std::string())},
                        {"summary_prompt", msh.value("summary_prompt", std::string())}
                    }},
                    {"long", J{
                        {"enabled", mlo.value("enabled", false)},
                        {"embed_model_id", mlo.value("embed_model_id", std::string())},
                        {"embed_model", mlo.value("embed_model", std::string())},
                        {"top_k", (long long)mtopk},
                        {"min_similarity", msim},
                        {"max_facts", (long long)mcap},
                        {"extract_model_id", mlo.value("extract_model_id", std::string())},
                        {"extract_prompt", mlo.value("extract_prompt", std::string())}
                    }}
                };
                // 智能化阶段D：工具调用配置（function-calling 掷骰/抽牌/查卡）。
                J tlj = (j.contains("tools") && j["tools"].is_object()) ? j["tools"] : J::object();
                int tmr = tlj.value("max_rounds", 3); if (tmr < 1) tmr = 1; if (tmr > 6) tmr = 6;
                out["tools"] = J{
                    {"enabled", tlj.value("enabled", false)},
                    {"roll_dice", tlj.value("roll_dice", true)},
                    {"draw_deck", tlj.value("draw_deck", true)},
                    {"get_attr", tlj.value("get_attr", true)},
                    {"set_attr", tlj.value("set_attr", false)},        // AI深化：写卡（默认关，有风险）
                    {"run_command", tlj.value("run_command", true)},   // 
                    {"search_help", tlj.value("search_help", true)},   // 
                    {"max_rounds", (long long)tmr}
                };
                // 智能化阶段E：NPC 扮演配置（enabled + list[]）。
                J npj = (j.contains("npc") && j["npc"].is_object()) ? j["npc"] : J::object();
                J npList = J::array();
                if (npj.contains("list") && npj["list"].is_array()) {
                    for (auto& np : npj["list"]) {
                        if (!np.is_object()) continue;
                        std::string nm = np.value("name", std::string());
                        if (nm.empty()) continue;
                        std::string nid = np.value("id", std::string());
                        if (nid.empty()) nid = "npc" + drogon::utils::genRandomString(8);
                        J trg = J::array();
                        if (np.contains("triggers") && np["triggers"].is_array())
                            for (auto& tg : np["triggers"])
                                if (tg.is_string() && !tg.get<std::string>().empty()) trg.push_back(tg);
                        npList.push_back(J{
                            {"id", nid}, {"name", nm},
                            {"persona", np.value("persona", std::string())},
                            {"knowledge", np.value("knowledge", std::string())},
                            {"triggers", trg},
                            {"model_id", np.value("model_id", std::string())},
                            {"group", np.value("group", std::string())},
                            {"enabled", np.value("enabled", true)},
                            {"mood_enabled", np.value("mood_enabled", false)}   // A1：情绪/关系记忆
                        });
                    }
                }
                out["npc"] = J{{"enabled", npj.value("enabled", false)}, {"list", npList}};
                // AI 白名单模式：开启后仅白名单群/私聊可用 AI（非白名单群 .ai on 也拒绝）。
                J wlj = (j.contains("whitelist") && j["whitelist"].is_object()) ? j["whitelist"] : J::object();
                J wlList = J::array();
                if (wlj.contains("list") && wlj["list"].is_array())
                    for (auto& e : wlj["list"]) {
                        if (!e.is_object()) continue;
                        std::string wid = e.value("id", std::string());
                        if (wid.empty()) continue;
                        wlList.push_back(J{{"platform", e.value("platform", std::string())}, {"id", wid},
                                           {"is_group", e.value("is_group", true)}, {"name", e.value("name", std::string())}});
                    }
                out["whitelist"] = J{{"enabled", wlj.value("enabled", false)}, {"list", wlList}};
                // 图像识别（多模态）配置。
                J vsj = (j.contains("vision") && j["vision"].is_object()) ? j["vision"] : J::object();
                int vmi = vsj.value("max_images", 2); if (vmi < 1) vmi = 1; if (vmi > 4) vmi = 4;
                out["vision"] = J{
                    {"enabled", vsj.value("enabled", false)},
                    {"model_id", vsj.value("model_id", std::string())},
                    {"prompt", vsj.value("prompt", std::string())},
                    {"max_images", (long long)vmi}
                };
                cfg.set<J>("dice/ai", out);
                cfg.save();
            }
            J cur = cfg.get<J>("dice/ai", J::object());
            if (!cur.is_object()) cur = J::object();
            if (!cur.contains("enabled")) cur["enabled"] = false;
            if (!cur.contains("models") || !cur["models"].is_array()) cur["models"] = J::array();
            const J covRollOnly = J{{"roll", true}, {"deck", false}, {"fun", false}, {"custom", false}, {"plugin", false}};
            const J covAll = J{{"roll", true}, {"deck", true}, {"fun", true}, {"custom", true}, {"plugin", true}};
            if (!cur.contains("params") || !cur["params"].is_object())
                cur["params"] = J{{"temperature", 0.7}, {"top_p", 1.0}, {"max_tokens", 1024}, {"frequency_penalty", 0.0}, {"presence_penalty", 0.0}};
            if (!cur.contains("polish") || !cur["polish"].is_object())
                cur["polish"] = J{{"enabled", false}, {"model_id", std::string()}, {"mode", "text"}, {"persona", std::string()}, {"cov", covRollOnly}};
            else if (!cur["polish"].contains("cov")) cur["polish"]["cov"] = covRollOnly;
            if (!cur.contains("translate") || !cur["translate"].is_object())
                cur["translate"] = J{{"enabled", false}, {"model_id", std::string()}, {"cov", covAll}, {"langs", J::array()}};
            else if (!cur["translate"].contains("cov")) cur["translate"]["cov"] = covAll;
            if (!cur.contains("chat") || !cur["chat"].is_object())
                cur["chat"] = J{{"enabled", false}, {"model_id", std::string()}, {"persona", std::string()},
                                {"prompt", std::string()}, {"at_bot", true}, {"keywords", J::array()},
                                {"standby_prob", 0}, {"context_rounds", 10}, {"max_chars", 200},
                                {"cooldown_sec", 5}, {"reply_at", false}, {"no_emoji", true}, {"filters", J::array()}};
            else { if (!cur["chat"].contains("no_emoji")) cur["chat"]["no_emoji"] = true;
                   if (!cur["chat"].contains("filters")) cur["chat"]["filters"] = J::array(); }
            if (!cur.contains("memory") || !cur["memory"].is_object()) cur["memory"] = J::object();
            if (!cur["memory"].contains("short") || !cur["memory"]["short"].is_object())
                cur["memory"]["short"] = J{{"enabled", false}, {"rounds", 20}, {"max_chars", 400},
                                           {"summary_model_id", std::string()}, {"summary_prompt", std::string()}};
            if (!cur["memory"].contains("long") || !cur["memory"]["long"].is_object())
                cur["memory"]["long"] = J{{"enabled", false}, {"embed_model_id", std::string()}, {"embed_model", std::string()},
                                          {"top_k", 4}, {"min_similarity", 0.75}, {"max_facts", 300},
                                          {"extract_model_id", std::string()}, {"extract_prompt", std::string()}};
            if (!cur.contains("tools") || !cur["tools"].is_object())
                cur["tools"] = J{{"enabled", false}, {"roll_dice", true}, {"draw_deck", true}, {"get_attr", true},
                                 {"set_attr", false}, {"run_command", true}, {"search_help", true}, {"max_rounds", 3}};
            else { if (!cur["tools"].contains("run_command")) cur["tools"]["run_command"] = true;
                   if (!cur["tools"].contains("search_help")) cur["tools"]["search_help"] = true;
                   if (!cur["tools"].contains("set_attr")) cur["tools"]["set_attr"] = false; }
            if (!cur.contains("npc") || !cur["npc"].is_object())
                cur["npc"] = J{{"enabled", false}, {"list", J::array()}};
            if (!cur.contains("whitelist") || !cur["whitelist"].is_object())
                cur["whitelist"] = J{{"enabled", false}, {"list", J::array()}};
            if (!cur.contains("vision") || !cur["vision"].is_object())
                cur["vision"] = J{{"enabled", false}, {"model_id", std::string()}, {"prompt", std::string()}, {"max_images", 2}, {"pass_url", true}};
            else if (!cur["vision"].contains("pass_url"))
                cur["vision"]["pass_url"] = true;
            // 内置默认提示词（只读参考，供前端「载入默认」按钮填入编辑框）。
            cur["defaults"] = J{
                {"polish_text", dice::aipolish::defaultPromptText()},
                {"polish_rp", dice::aipolish::defaultPromptRp()},
                {"translate", dice::aitrans::defaultPrompt()},
                {"chat", dice::aichat::defaultPrompt()},
                {"summary", dice::aimemory::defaultSummaryPrompt()},
                {"extract", dice::aimemory::defaultExtractPrompt()},
                {"vision", dice::aivision::defaultPrompt()}
            };
            jsonReply(ok(cur), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 智能化阶段B/C：查看/清空 AI 记忆。GET ?kind=summary|fact 列出群摘要或持久事实；
    // POST {scope_id} 立即折叠一批（摘要+抽取事实）；DELETE 清空（?scope_id= 限某群，
    // ?kind= 限类型，缺省清全部）。供 WebUI「群历史记忆」卡片查看与重置。
    app.registerHandler("/api/system/ai/memory", [&cfg, &db](Req req, CB&& cb) {
        try {
            auto* cst = db.getChatStorage();
            if (!cst) { jsonReply(fail("no chat store"), std::move(cb)); return; }
            if (req->method() == drogon::Post) {
                // 立即为某群折叠一批历史（摘要 + 抽取事实，同步，可能阻塞 ~数十秒）。scope_id = "平台:群号"。
                auto j = J::parse(req->body());
                std::string sid = j.value("scope_id", std::string());
                auto pos = sid.find(':');
                if (pos == std::string::npos) { jsonReply(fail("bad scope_id"), std::move(cb)); return; }
                std::string plat = sid.substr(0, pos), gid = sid.substr(pos + 1);
                dice::aimemory::maybeFold(cfg, cst, plat, gid);
                long long factN = 0;
                try { factN = cst->count<AiMemoryRow>(orm::where(orm::c(&AiMemoryRow::kind) == std::string("fact")
                    and orm::c(&AiMemoryRow::scopeId) == sid)); } catch (...) {}
                jsonReply(ok(J{{"summary", dice::aimemory::currentSummary(cst, "group", sid)}, {"facts", factN}}), std::move(cb));
                return;
            }
            if (req->method() == drogon::Delete) {
                std::string sid = req->getParameter("scope_id");
                std::string kind = req->getParameter("kind");   // 空=summary+fact 都清
                try {
                    auto kindMatch = [&](const std::string& k) { return kind.empty() || kind == k; };
                    for (const std::string& k : {std::string("summary"), std::string("fact")}) {
                        if (!kindMatch(k)) continue;
                        if (sid.empty())
                            cst->remove_all<AiMemoryRow>(orm::where(orm::c(&AiMemoryRow::kind) == k));
                        else
                            cst->remove_all<AiMemoryRow>(orm::where(orm::c(&AiMemoryRow::kind) == k
                                and orm::c(&AiMemoryRow::scopeId) == sid));
                    }
                } catch (...) {}
                jsonReply(ok(nullptr), std::move(cb));
                return;
            }
            std::string kind = req->getParameter("kind"); if (kind.empty()) kind = "summary";
            std::string fsid = req->getParameter("scope_id");   // 可选，只看某群
            J arr = J::array();
            try {
                auto rows = cst->get_all<AiMemoryRow>(
                    orm::where(orm::c(&AiMemoryRow::kind) == kind),
                    orm::order_by(&AiMemoryRow::updatedAt).desc());
                for (auto& r : rows) {
                    if (!fsid.empty() && r.scopeId != fsid) continue;
                    if (kind == "summary" && r.content.empty()) continue;   // 仅做水位线的空摘要行不展示
                    arr.push_back(J{{"scope", r.scope}, {"scope_id", r.scopeId}, {"content", r.content},
                                    {"ref_id", r.refId}, {"updated_at", r.updatedAt}, {"hits", r.hits}});
                }
            } catch (...) {}
            jsonReply(ok(J{{"items", arr}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post, drogon::Delete});

    // 连通性测试——用请求里给的模型配置（未保存也能测）或按 id 取已存模型，
    // 调一次 chat 返回样例回复 + 延迟 + token + 估算费用。不受总开关限制（便于配置时验证）。
    app.registerHandler("/api/system/ai/test", [&cfg](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            dice::ai::Model m;
            if (j.contains("model") && j["model"].is_object()) {
                m = dice::ai::modelFromJson(j["model"]);
            } else if (j.contains("id")) {
                std::string id = j.value("id", std::string());
                for (auto& mm : dice::ai::models(cfg)) if (mm.id == id) { m = mm; break; }
            }
            std::string prompt = j.value("prompt", std::string("\xe7\x94\xa8\xe4\xb8\x80\xe5\x8f\xa5\xe8\xaf\x9d\xe5\x81\x9a\xe4\xb8\xaa\xe8\x87\xaa\xe6\x88\x91\xe4\xbb\x8b\xe7\xbb\x8d\xe3\x80\x82"));  // 用一句话做个自我介绍。
            auto r = dice::ai::chat(cfg, m, "You are a helpful assistant.", prompt, 128, 30);
            jsonReply(ok(J{
                {"ok", r.ok}, {"reply", r.reply}, {"error", r.error},
                {"latencyMs", r.latencyMs}, {"promptTokens", r.promptTokens},
                {"completionTokens", r.completionTokens}, {"totalTokens", r.totalTokens},
                {"cost", r.cost}
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 是否把日志内图片落地到本地（data/logs/images），供网页导出内嵌。
    app.registerHandler("/api/system/save-log-images", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<bool>("dice/save_log_images", j.value("enabled", false));
                cfg.save();
            }
            jsonReply(ok(J{{"enabled", cfg.get<bool>("dice/save_log_images", false)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 回复中用户名包裹前后缀（dice.nick_prefix/nick_suffix，默认 <>，留空=不包裹）。
    app.registerHandler("/api/system/nick-wrap", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<std::string>("dice/nick_prefix", j.value("prefix", std::string("<")));
                cfg.set<std::string>("dice/nick_suffix", j.value("suffix", std::string(">")));
                cfg.save();
            }
            jsonReply(ok(J{{"prefix", cfg.get<std::string>("dice/nick_prefix", std::string("<"))},
                          {"suffix", cfg.get<std::string>("dice/nick_suffix", std::string(">"))}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 分段发送：dice/reply_segment_enabled（总开关，默认开）+
    // dice/reply_segment_len（阈值，默认600，夹[100,1000]）。
    app.registerHandler("/api/system/reply-segment", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("enabled")) cfg.set<bool>("dice/reply_segment_enabled", j.value("enabled", true));
                if (j.contains("len")) {
                    int v = j.value("len", 600);
                    if (v < 100) v = 100; if (v > 1000) v = 1000;
                    cfg.set<int>("dice/reply_segment_len", v);
                }
                cfg.save();
            }
            jsonReply(ok(J{
                {"enabled", cfg.get<bool>("dice/reply_segment_enabled", true)},
                {"len", cfg.get<int>("dice/reply_segment_len", 600)}
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 回复是否引用触发消息（dice.quote_reply，默认 true=引用投掷对象的发言）。
    app.registerHandler("/api/system/quote-reply", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<bool>("dice/quote_reply", j.value("enabled", true));
                cfg.save();
            }
            jsonReply(ok(J{{"enabled", cfg.get<bool>("dice/quote_reply", true)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 自动卡面：.st 改基础属性后落地衍生 HP/SAN/MP 并附摘要（dice.auto_card，默认 true）。
    app.registerHandler("/api/system/auto-card", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<bool>("dice/auto_card", j.value("enabled", true));
                cfg.save();
            }
            jsonReply(ok(J{{"enabled", cfg.get<bool>("dice/auto_card", true)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ═══ Persona switching API ═══════════════════════════

    // List all personas
    app.registerHandler("/api/personas", [&personaMgr](Req, CB&& cb) {
        J arr = J::array();
        for (auto& t : personaMgr.listTemplates()) {
            arr.push_back(J{
                {"id", t.id}, {"name", t.name}, {"description", t.description},
                {"isBuiltin", t.isBuiltin}, {"entryCount", personaMgr.getEntryCount(t.id)},
                {"createdAt", t.createdAt}, {"updatedAt", t.updatedAt}
            });
        }
        jsonReply(ok(arr), std::move(cb));
    }, {drogon::Get});

    // Create a persona
    app.registerHandler("/api/personas", [&personaMgr](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string name = j.value("name", "");
            std::string desc = j.value("description", "");
            if (name.empty()) { jsonReply(fail("name is required"), std::move(cb)); return; }
            int id = personaMgr.createTemplate(name, desc);
            if (id < 0) { jsonReply(fail("name already exists or creation failed"), std::move(cb)); return; }
            jsonReply(ok(J{{"id", id}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Import a persona (specific route — before {1} to avoid path-param conflict)
    app.registerHandler("/api/personas/import", [&personaMgr](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            int id = personaMgr.importTemplate(j);
            if (id < 0) { jsonReply(fail("import failed (name may exist)"), std::move(cb)); return; }
            jsonReply(ok(J{{"id", id}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Get active persona info (specific route — before {1})
    app.registerHandler("/api/personas/active", [&personaMgr, &cfg](Req req, CB&& cb) {
        std::string groupId = req->getParameter("groupId");
        int activeId = personaMgr.getActivePersona(groupId);
        J data;
        data["activeId"] = activeId;
        if (activeId > 0) {
            auto t = personaMgr.getTemplateById(activeId);
            if (t.id > 0) {
                data["name"] = t.name;
                data["description"] = t.description;
            }
        }
        data["globalId"] = cfg.get<int>("persona/global", 0);
        jsonReply(ok(data), std::move(cb));
    }, {drogon::Get});

    // Get persona detail
    app.registerHandler("/api/personas/{1}", [&personaMgr](Req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto t = personaMgr.getTemplateById(id);
            if (t.id <= 0) { jsonReply(fail("persona not found"), std::move(cb)); return; }
            jsonReply(ok(J{
                {"id", t.id}, {"name", t.name}, {"description", t.description},
                {"isBuiltin", t.isBuiltin}, {"entryCount", personaMgr.getEntryCount(id)},
                {"createdAt", t.createdAt}, {"updatedAt", t.updatedAt}
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // Update persona metadata
    app.registerHandler("/api/personas/{1}", [&personaMgr](Req req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto j = J::parse(req->body());
            std::string name = j.value("name", "");
            std::string desc = j.value("description", "");
            if (!personaMgr.updateTemplateMeta(id, name, desc)) {
                jsonReply(fail("update failed (name may already exist)"), std::move(cb)); return;
            }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put});

    // Delete a persona
    app.registerHandler("/api/personas/{1}", [&personaMgr](Req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            if (!personaMgr.deleteTemplate(id)) {
                jsonReply(fail("cannot delete (built-in or not found)"), std::move(cb)); return;
            }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // Copy a persona
    app.registerHandler("/api/personas/{1}/copy", [&personaMgr](Req req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto j = J::parse(req->body());
            std::string newName = j.value("newName", "");
            if (newName.empty()) { jsonReply(fail("newName is required"), std::move(cb)); return; }
            int newId = personaMgr.copyTemplate(id, newName);
            if (newId < 0) { jsonReply(fail("copy failed (source not found or name exists)"), std::move(cb)); return; }
            jsonReply(ok(J{{"id", newId}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Activate a persona (set as current)
    app.registerHandler("/api/personas/{1}/activate", [&personaMgr](Req req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto j = J::parse(req->body());
            std::string groupId = j.value("groupId", "");
            personaMgr.setActivePersona(id, groupId);
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // List persona entries
    app.registerHandler("/api/personas/{1}/entries", [&personaMgr](Req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            J arr = J::array();
            for (auto& e : personaMgr.listEntries(id)) {
                arr.push_back(J{
                    {"id", e.id}, {"personaId", e.personaId},
                    {"locale", e.locale}, {"key", e.key}, {"value", e.value}
                });
            }
            jsonReply(ok(arr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // Set/update a persona entry (upsert)
    app.registerHandler("/api/personas/{1}/entries", [&personaMgr](Req req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto j = J::parse(req->body());
            std::string locale = j.value("locale", "zh-Hans");
            std::string key = j.value("key", "");
            std::string value = j.value("value", "");
            if (key.empty()) { jsonReply(fail("key is required"), std::move(cb)); return; }
            if (!personaMgr.setEntry(id, locale, key, value)) {
                jsonReply(fail("failed to set entry"), std::move(cb)); return;
            }
            // Hot-reload into I18n if this persona is currently active
            personaMgr.loadIntoI18n(id);
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put});

    // Delete a persona entry
    app.registerHandler("/api/personas/{1}/entries", [&personaMgr](Req req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            // For DELETE, pass locale and key as query params or body
            auto j = J::parse(req->body());
            std::string locale = j.value("locale", "zh-Hans");
            std::string key = j.value("key", "");
            if (key.empty()) { jsonReply(fail("key is required"), std::move(cb)); return; }
            if (!personaMgr.deleteEntry(id, locale, key)) {
                jsonReply(fail("failed to delete entry"), std::move(cb)); return;
            }
            personaMgr.loadIntoI18n(id);
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // Export a persona
    app.registerHandler("/api/personas/{1}/export", [&personaMgr](Req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto data = personaMgr.exportTemplate(id);
            if (data.is_null()) { jsonReply(fail("persona not found"), std::move(cb)); return; }
            jsonReply(ok(data), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // #6 合并转发：回复字符数超过阈值(默认1200，应用于所有回复)时强制以聊天记录形式发送
    //（dice.forward_long 开关 + dice.forward_threshold 阈值）。
    app.registerHandler("/api/system/forward-long", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<bool>("dice/forward_long", j.value("enabled", false));
                if (j.contains("threshold") && j["threshold"].is_number()) {
                    int th = j["threshold"].get<int>();
                    if (th < 1) th = 1;
                    cfg.set<int>("dice/forward_threshold", th);
                }
                cfg.save();
            }
            jsonReply(ok(J{{"enabled", cfg.get<bool>("dice/forward_long", false)},
                          {"threshold", cfg.get<int>("dice/forward_threshold", 1200)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 图床配置（mode none/generic/local + url/file_field/headers/result_path/public_base）。
    app.registerHandler("/api/system/image-host", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<J>("dice/image_host", j);
                cfg.save();
            }
            J cur = cfg.get<J>("dice/image_host", J::object());
            if (!cur.is_object()) cur = J::object();
            if (!cur.contains("mode")) cur["mode"] = "none";
            jsonReply(ok(cur), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 图片发送方式（base64 内嵌 / httpurl + 可配 host，默认 localhost:<端口>）。
    app.registerHandler("/api/system/image-send", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                std::string mode = j.value("mode", std::string("base64"));
                if (mode != "base64" && mode != "httpurl") mode = "base64";
                cfg.set<J>("dice/image_send", J{{"mode", mode}, {"host", j.value("host", std::string())}});
                cfg.save();
            }
            J cur = cfg.get<J>("dice/image_send", J::object());
            if (!cur.is_object()) cur = J::object();
            if (!cur.contains("mode") || !cur["mode"].is_string()) cur["mode"] = "base64";
            if (!cur.contains("host") || !cur["host"].is_string()) cur["host"] = "";
            // 前端占位提示用的默认 host。
            cur["default_host"] = "localhost:" + std::to_string(cfg.get<int>("server/port", 18088));
            jsonReply(ok(cur), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 出站消息表现形式。传统模式始终发送纯文本；卡片模式由各适配器按其
    // 官方能力渲染，无法使用富消息的平台会安全回退为传统文本。
    // 支持全局默认 + 按适配器/帐号单独覆盖（适配器配置 message_format：
    // 空=跟随全局 / traditional / card）。
    app.registerHandler("/api/system/message-format", [&cfg, st, &adapterMgr](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                std::string mode = j.value("mode", std::string("traditional"));
                if (mode != "card") mode = "traditional";
                cfg.set<std::string>("dice/message_format", mode);
                if (j.contains("adapters") && j["adapters"].is_array()) {
                    for (auto& e : j["adapters"]) {
                        if (!e.is_object()) continue;
                        const std::string id = e.value("id", std::string());
                        std::string am = e.value("mode", std::string());
                        if (am != "card" && am != "traditional") am = "";
                        int aid = 0; try { aid = std::stoi(id); } catch (...) { continue; }
                        auto row = st->get_pointer<AdapterRow>(aid);
                        if (!row) continue;
                        J cfg2 = J::parse(row->config, nullptr, false);
                        if (!cfg2.is_object()) cfg2 = J::object();
                        if (am.empty()) cfg2.erase("message_format");
                        else cfg2["message_format"] = am;
                        row->config = cfg2.dump();
                        st->update(*row);
                        if (auto a = adapterMgr.getAdapter(id))
                            a->setMessageFormatOverride(IAdapter::parseFormatOverride(am));
                    }
                    persistAdaptersToConfig(st, cfg);
                }
                cfg.save();
                IAdapter::setCardMessageMode(mode == "card");
            }
            const std::string mode = cfg.get<std::string>("dice/message_format", "traditional") == "card"
                ? "card" : "traditional";
            J adapters = J::array();
            for (auto& r : st->get_all<AdapterRow>()) {
                J cfg2 = J::parse(r.config, nullptr, false);
                if (!cfg2.is_object()) cfg2 = J::object();
                adapters.push_back(J{{"id", std::to_string(r.id)}, {"type", r.type == static_cast<int>(AdapterType::kQQOfficial) ? "qq_official" : r.type == static_cast<int>(AdapterType::kDiscord) ? "discord" : r.type == static_cast<int>(AdapterType::kKook) ? "kook" : "onebot_v11"},
                                     {"name", r.name},
                                     {"loginId", [&] { auto a = adapterMgr.getAdapter(std::to_string(r.id)); return a ? a->getLoginId() : std::string(); }()},
                                     {"loginName", [&] { auto a = adapterMgr.getAdapter(std::to_string(r.id)); return a ? a->getLoginName() : std::string(); }()},
                                     {"appId", [&] { auto a = adapterMgr.getAdapter(std::to_string(r.id)); if (auto off = std::dynamic_pointer_cast<QQOfficialAdapter>(a)) return off->appId(); return std::string(); }()},
                                     {"mode", cfg2.value("message_format", std::string())}});
            }
            jsonReply(ok(J{{"mode", mode}, {"adapters", adapters}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // Masters (bot owners), stored in config dice.masters.
    // 条目 {platform, adapter_id, id}：adapter_id 为空 = 该平台全部账号；
    // platform 为空 = 旧版任意平台。dice/master_inherit 控制跨平台绑定身份继承骰主。
    app.registerHandler("/api/masters", [&cfg, st](Req req, CB&& cb) {
        try {
            J arr = cfg.get<J>("dice/masters", J::array());
            if (!arr.is_array()) arr = J::array();
            const bool masterInherit = cfg.get<bool>("dice/master_inherit", true);
            // 玩家昵称（用于前端显示“昵称(id)”）；QQ 官方机器人的 OpenID 先映射到公共号再查。
            auto nickOf = [&](const std::string& plat, const std::string& id) -> std::string {
                std::string uid = id;
                if (plat == "qq_official") {
                    try {
                        auto eps = st->get_all<IdentityEndpointRow>(orm::where(
                            orm::c(&IdentityEndpointRow::adapterType) == std::string("qq_official") and
                            orm::c(&IdentityEndpointRow::kind) == std::string("user") and
                            orm::c(&IdentityEndpointRow::endpointId) == id));
                        if (!eps.empty()) {
                            auto ent = st->get<IdentityRow>(eps.front().identityId);
                            uid = ent.publicId;
                        }
                    } catch (...) {}
                }
                try {
                    auto rows = st->get_all<PlayerProfileRow>(orm::where(
                        orm::c(&PlayerProfileRow::platform) == plat and
                        orm::c(&PlayerProfileRow::userId) == uid), orm::limit(1));
                    if (!rows.empty()) return rows.front().nickname;
                } catch (...) {}
                return {};
            };
            // Normalize legacy bare-string entries to {platform:"", adapter_id:"", id}.
            J norm = J::array();
            for (auto& m : arr) {
                std::string plat, adapter, id;
                if (m.is_string()) id = m.get<std::string>();
                else if (m.is_object()) { plat = m.value("platform", ""); adapter = m.value("adapter_id", m.value("adapterId", "")); id = m.value("id", ""); }
                if (id.empty()) continue;
                norm.push_back(J{{"platform", plat}, {"adapter_id", adapter}, {"id", id}, {"nickname", nickOf(plat, id)}});
            }
            if (req->method() == drogon::Post) {
                auto j = J::parse(req->body(), nullptr, false);
                if (!j.is_object()) { jsonReply(fail("invalid JSON request"), std::move(cb)); return; }
                std::string plat = j.value("platform", "onebot_v11");
                std::string adapter = j.value("adapter_id", j.value("adapterId", std::string()));
                std::string id = j.value("id", "");
                const bool hasInherit = j.contains("master_inherit") && j["master_inherit"].is_boolean();
                if (id.empty() && !hasInherit) { jsonReply(fail("id required"), std::move(cb)); return; }
                if (!id.empty()) {
                    // OpenID 按 AppID 独立：官方机器人的骰主必须落到具体账号，不接受平台级。
                    if (plat == "qq_official" && adapter.empty()) {
                        jsonReply(fail("QQ 官方机器人的 OpenID 按 AppID 独立，必须指定具体适配器账号"), std::move(cb)); return;
                    }
                    bool exists = false;
                    for (auto& m : norm)
                        if (m.value("platform", "") == plat && m.value("adapter_id", "") == adapter && m.value("id", "") == id) exists = true;
                    if (!exists) norm.push_back(J{{"platform", plat}, {"adapter_id", adapter}, {"id", id}});
                    cfg.set<J>("dice/masters", norm); cfg.save();
                }
                // 跨平台骰主继承开关随本接口一并保存。
                if (hasInherit) {
                    cfg.set<bool>("dice/master_inherit", j["master_inherit"].get<bool>()); cfg.save();
                }
            }
            jsonReply(ok(J{{"items", norm}, {"master_inherit", masterInherit}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    // B：骰主通知设置（dice/notice）。GET 读（附事件目录 catalog 供前端分组勾选），
    // PUT 覆盖 windows（含逐项 events + 显示名 name）+ webhook + smtp。
    app.registerHandler("/api/system/notice", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                J out = J::object();
                J wins = J::array();
                if (j.contains("windows") && j["windows"].is_array())
                    for (auto& w : j["windows"]) {
                        if (!w.is_object()) continue;
                        std::string cid = w.value("chat_id", std::string());
                        if (cid.empty()) continue;
                        int mask = w.value("level_mask", (int)dice::notice::kAll);
                        if (mask < 1) mask = 1; if (mask > (int)dice::notice::kAll) mask = dice::notice::kAll;
                        J evs = J::array();
                        if (w.contains("events") && w["events"].is_array())
                            for (auto& e : w["events"]) if (e.is_string() && dice::notice::areaOf(e.get<std::string>())) evs.push_back(e);
                        wins.push_back(J{{"platform", w.value("platform", std::string())},
                                         {"adapter_id", w.value("adapter_id", std::string())}, {"chat_id", cid},
                                         {"is_group", w.value("is_group", true)}, {"name", w.value("name", std::string())},
                                         {"level_mask", mask}, {"events", evs}});
                    }
                out["windows"] = wins;
                // Webhook（POST JSON 到第三方）。
                J wh = (j.contains("webhook") && j["webhook"].is_object()) ? j["webhook"] : J::object();
                int whm = wh.value("level_mask", (int)dice::notice::kAll);
                if (whm < 1) whm = 1; if (whm > (int)dice::notice::kAll) whm = dice::notice::kAll;
                out["webhook"] = J{{"enabled", wh.value("enabled", false)},
                                   {"url", wh.value("url", std::string())}, {"level_mask", whm}};
                // SMTP 邮件。
                J sm = (j.contains("smtp") && j["smtp"].is_object()) ? j["smtp"] : J::object();
                int smm = sm.value("level_mask", (int)dice::notice::kAll);
                if (smm < 1) smm = 1; if (smm > (int)dice::notice::kAll) smm = dice::notice::kAll;
                int smport = sm.value("port", 465); if (smport < 1 || smport > 65535) smport = 465;
                out["smtp"] = J{{"enabled", sm.value("enabled", false)},
                                {"host", sm.value("host", std::string())}, {"port", (long long)smport},
                                {"ssl", sm.value("ssl", true)},
                                {"user", sm.value("user", std::string())}, {"pass", sm.value("pass", std::string())},
                                {"from", sm.value("from", std::string())}, {"to", sm.value("to", std::string())},
                                {"level_mask", smm}};
                cfg.set<J>("dice/notice", out); cfg.save();
            }
            J nc = cfg.get<J>("dice/notice", J::object());
            if (!nc.is_object()) nc = J::object();
            if (!nc.contains("windows") || !nc["windows"].is_array()) nc["windows"] = J::array();
            if (!nc.contains("webhook") || !nc["webhook"].is_object())
                nc["webhook"] = J{{"enabled", false}, {"url", std::string()}, {"level_mask", (int)dice::notice::kAll}};
            if (!nc.contains("smtp") || !nc["smtp"].is_object())
                nc["smtp"] = J{{"enabled", false}, {"host", std::string()}, {"port", 465}, {"ssl", true},
                               {"user", std::string()}, {"pass", std::string()},
                               {"from", std::string()}, {"to", std::string()}, {"level_mask", (int)dice::notice::kAll}};
            // 事件目录（op → 区域），前端据此渲染分区逐项勾选。
            J cat = J::array();
            for (auto& [op, area] : dice::notice::eventCatalog()) cat.push_back(J{{"op", op}, {"area", area}});
            nc["catalog"] = cat;
            jsonReply(ok(nc), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // B：发送测试通知（走完整 notify 链路：窗口 + Webhook + SMTP + 审计）。
    app.registerHandler("/api/system/notice/test", [&cfg, &adapterMgr](Req, CB&& cb) {
        try {
            dice::notice::notify(cfg, adapterMgr, dice::notice::kImportant,
                "\xe8\xbf\x99\xe6\x98\xaf\xe4\xb8\x80\xe6\x9d\xa1\xe6\xb5\x8b\xe8\xaf\x95\xe9\x80\x9a\xe7\x9f\xa5\xe3\x80\x82", "", "", "test");
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // B：审计日志查看（data/audit/notice_*.jsonl，倒序取最近 N 条）。
    app.registerHandler("/api/system/audit", [](Req, CB&& cb) {
        try {
            namespace fs = std::filesystem;
            J arr = J::array();
            fs::path dir = "data/audit";
            std::vector<fs::path> files;
            if (fs::exists(dir)) for (auto& e : fs::directory_iterator(dir))
                if (e.is_regular_file() && e.path().extension() == ".jsonl") files.push_back(e.path());
            std::sort(files.begin(), files.end());
            const int limit = 200;
            std::vector<std::string> lines;
            for (auto it = files.rbegin(); it != files.rend() && (int)lines.size() < limit; ++it) {
                std::ifstream f(*it); std::string ln; std::vector<std::string> fl;
                while (std::getline(f, ln)) if (!ln.empty()) fl.push_back(ln);
                for (auto rit = fl.rbegin(); rit != fl.rend() && (int)lines.size() < limit; ++rit) lines.push_back(*rit);
            }
            for (auto& ln : lines) { try { arr.push_back(J::parse(ln)); } catch (...) {} }
            jsonReply(ok(J{{"items", arr}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // 删除骰主：新三段式 /api/masters/{平台}/{适配器账号}/{ID}（"_" = 空）。
    app.registerHandler("/api/masters/{1}/{2}/{3}", [&cfg](Req, CB&& cb, const std::string& platRaw, const std::string& adapterRaw, const std::string& id) {
        try {
            std::string plat = (platRaw == "_") ? "" : platRaw;   // "_" = legacy empty platform
            std::string adapter = (adapterRaw == "_") ? "" : adapterRaw;
            J arr = cfg.get<J>("dice/masters", J::array());
            J keep = J::array();
            for (auto& m : arr) {
                std::string p = m.is_string() ? std::string("") : m.value("platform", "");
                std::string a = m.is_string() ? std::string("") : m.value("adapter_id", m.value("adapterId", ""));
                std::string i = m.is_string() ? m.get<std::string>() : m.value("id", "");
                if (p == plat && a == adapter && i == id) continue;   // drop the matched one
                keep.push_back(J{{"platform", p}, {"adapter_id", a}, {"id", i}});
            }
            cfg.set<J>("dice/masters", keep); cfg.save();
            jsonReply(ok(keep), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // 兼容旧版二段式删除（adapter 视为空）。
    app.registerHandler("/api/masters/{1}/{2}", [&cfg](Req, CB&& cb, const std::string& platRaw, const std::string& id) {
        try {
            std::string plat = (platRaw == "_") ? "" : platRaw;
            J arr = cfg.get<J>("dice/masters", J::array());
            J keep = J::array();
            for (auto& m : arr) {
                std::string p = m.is_string() ? std::string("") : m.value("platform", "");
                std::string i = m.is_string() ? m.get<std::string>() : m.value("id", "");
                if (p == plat && i == id) continue;
                keep.push_back(J{{"platform", p}, {"id", i}});
            }
            cfg.set<J>("dice/masters", keep); cfg.save();
            jsonReply(ok(keep), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // Triggered announcement broadcast (set/cancel/status).
    app.registerHandler("/api/broadcast", [&adapterMgr](Req req, CB&& cb) {
        try {
            auto& bm = BroadcastManager::instance();
            if (req->method() == drogon::Post) {
                auto j = J::parse(req->body(), nullptr, false);
                if (!j.is_object()) { jsonReply(fail("invalid JSON request"), std::move(cb)); return; }
                std::string c = j.value("content", "");
                // Truncate to 200 Unicode code points (UTF-8 aware).
                int cps = 0; size_t cut = c.size();
                for (size_t i = 0; i < c.size();) {
                    unsigned char ch = c[i];
                    i += ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
                    if (++cps == 200) { cut = i; break; }
                }
                if (cut < c.size()) c = c.substr(0, cut);
                if (c.empty()) { jsonReply(fail("empty"), std::move(cb)); return; }
                bm.set(c);
            } else if (req->method() == drogon::Delete) {
                bm.cancel();
            }
            int total = 0;
            for (auto& a : adapterMgr.allAdapters())
                if (a->isConnected()) total += (int)a->getGroupList().size();
            jsonReply(ok(J{{"active", bm.active()}, {"content", bm.content()},
                          {"pushed", bm.pushedCount()}, {"total", total}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post, drogon::Delete});

    // Command reference catalog, rendered for one locale (?lang=zh-Hant|zh-Hans|en).
    // Merges docs/commands.json with i18n descriptions + editable reply templates.
    app.registerHandler("/api/commands", [st, &i18n](Req req, CB&& cb) {
        try {
            std::string lang = "zh-Hans";
            auto& q = req->getParameters();
            if (auto it = q.find("lang"); it != q.end() && !it->second.empty()) lang = it->second;
            Locale loc = localeFromString(lang);

            J cat;
            { std::ifstream f("docs/commands.json"); if (!f) f.open("../docs/commands.json");
              if (f) f >> cat; }
            if (!cat.is_array()) cat = J::array();

            // overrides for this locale, from DB
            std::map<std::string, std::string> ov;
            if (st) for (auto& r : st->get_all<I18nOverrideRow>())
                if (r.locale == lang) ov[r.key] = r.value;

            // Derive the {placeholder} variables a template uses + their descriptions.
            auto deriveVars = [&i18n, loc](const std::string& tmpl) -> J {
                J vars = J::array();
                std::set<std::string> seen;
                for (size_t i = 0; i < tmpl.size();) {
                    if (tmpl[i] == '{') {
                        size_t e = tmpl.find('}', i + 1);
                        if (e == std::string::npos) break;
                        std::string name = tmpl.substr(i + 1, e - i - 1);
                        if (!name.empty() && seen.insert(name).second) {
                            std::string descKey = "tplvar." + name;
                            std::string desc = i18n.tr(loc, descKey);
                            if (desc == descKey) desc = "";   // no description registered
                            vars.push_back(J{{"name", name}, {"desc", desc}});
                        }
                        i = e + 1;
                    } else ++i;
                }
                return vars;
            };

            J out = J::array();
            const auto allDefaults = i18n.flatten(loc);
            for (auto& c : cat) {
                std::string descKey = c.value("descKey", "");
                J row;
                row["cmd"] = c.value("cmd", "");
                std::string title;
                if (c.contains("title")) {
                    if (c["title"].is_object()) title = c["title"].value(lang, c["title"].value("zh-Hans", std::string()));
                    else if (c["title"].is_string()) title = c["title"].get<std::string>();
                }
                row["title"] = title;
                row["category"] = c.value("category", "通用");
                row["sources"] = c.value("sources", J::array());
                row["example"] = c.value("example", "");
                std::string desc;
                if (c.contains("desc")) {
                    if (c["desc"].is_object())
                        desc = c["desc"].value(lang, c["desc"].value("zh-Hans", std::string()));
                    else if (c["desc"].is_string()) desc = c["desc"].get<std::string>();
                }
                if (desc.empty() && !descKey.empty()) desc = i18n.tr(loc, descKey);
                row["desc"] = desc;
                J replies = J::array();
                std::set<std::string> replyKeys;
                auto appendReply = [&](const std::string& key, const std::string& example = std::string()) {
                    if (!replyKeys.insert(key).second) return;
                    std::string def = i18n.getDefault(loc, key);
                    replies.push_back(J{
                        {"key", key},
                        {"default", def},
                        {"override", ov.count(key) ? J(ov[key]) : J(nullptr)},
                        {"v2key", legacyv2::v2KeyFor(key)},
                        {"example", example},
                        {"vars", deriveVars(def)}
                    });
                };
                if (c.contains("replyKeys") && c["replyKeys"].is_array()) {
                    bool hasRex = c.contains("replyExamples") && c["replyExamples"].is_object();
                    for (auto& rk : c["replyKeys"]) {
                        std::string key = rk.get<std::string>();
                        std::string ex = hasRex ? c["replyExamples"].value(key, std::string()) : std::string();
                        appendReply(key, ex);
                    }
                }
                // Large command families (for example .game/.npc/.send) regularly
                // gain new reply keys. Prefix expansion keeps their WebUI category
                // complete without requiring every locale key to be duplicated here.
                if (c.contains("replyPrefixes") && c["replyPrefixes"].is_array()) {
                    for (auto& rp : c["replyPrefixes"]) {
                        if (!rp.is_string()) continue;
                        const std::string prefix = rp.get<std::string>();
                        for (const auto& [key, _] : allDefaults)
                            if (key.compare(0, prefix.size(), prefix) == 0) appendReply(key);
                    }
                }
                row["replies"] = replies;
                out.push_back(row);
            }
            jsonReply(ok(out), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // Editable reply templates: set (PUT) / reset (DELETE) an override.
    app.registerHandler("/api/templates", [st, &i18n](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string lang = j.value("locale", "");
            std::string key = j.value("key", "");
            std::string value = j.value("value", "");
            if (lang.empty() || key.empty()) { jsonReply(fail("locale & key required"), std::move(cb)); return; }
            // upsert DB
            auto rows = st->get_all<I18nOverrideRow>(
                orm::where(orm::c(&I18nOverrideRow::locale) == lang and orm::c(&I18nOverrideRow::key) == key));
            if (rows.empty()) { I18nOverrideRow r; r.locale = lang; r.key = key; r.value = value; st->insert(r); }
            else { auto r = rows.front(); r.value = value; st->update(r); }
            i18n.setOverride(localeFromString(lang), key, value);
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put});

    app.registerHandler("/api/templates/{1}/{2}", [st, &i18n](Req, CB&& cb, const std::string& lang, const std::string& key) {
        try {
            st->remove_all<I18nOverrideRow>(
                orm::where(orm::c(&I18nOverrideRow::locale) == lang and orm::c(&I18nOverrideRow::key) == key));
            i18n.clearOverride(localeFromString(lang), key);
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // Export all custom reply overrides → { locale: { key: value } }.
    app.registerHandler("/api/templates/export", [st](Req, CB&& cb) {
        try {
            J out = J::object();
            if (st) for (auto& r : st->get_all<I18nOverrideRow>()) out[r.locale][r.key] = r.value;
            jsonReply(ok(out), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // Import custom reply overrides. Body: { data: { locale: { key: value } } }
    // (or the bare { locale: {...} } object). Upserts each and applies live.
    app.registerHandler("/api/templates/import", [st, &i18n](Req req, CB&& cb) {
        try {
            J body = J::parse(req->body());
            J data = body.contains("data") ? body["data"] : body;
            if (!data.is_object()) { jsonReply(fail("invalid json"), std::move(cb)); return; }
            int n = 0;
            for (auto& [lang, keys] : data.items()) {
                if (!keys.is_object()) continue;
                for (auto& [key, val] : keys.items()) {
                    if (!val.is_string()) continue;
                    std::string value = val.get<std::string>();
                    auto rows = st->get_all<I18nOverrideRow>(
                        orm::where(orm::c(&I18nOverrideRow::locale) == lang and orm::c(&I18nOverrideRow::key) == key));
                    if (rows.empty()) { I18nOverrideRow r; r.locale = lang; r.key = key; r.value = value; st->insert(r); }
                    else { auto r = rows.front(); r.value = value; st->update(r); }
                    i18n.setOverride(localeFromString(lang), key, value);
                    ++n;
                }
            }
            jsonReply(ok(J{{"imported", n}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 已加载语言列表（内置 + i18n/ 目录的自定义翻译文件），供
    // 群语言下拉 / 指令列表语言切换等动态展示。[{code, name}]
    app.registerHandler("/api/i18n/locales", [&i18n](Req, CB&& cb) {
        J arr = J::array();
        for (auto& [code, name] : i18n.listLocales())
            arr.push_back(J{{"code", code}, {"name", name}});
        jsonReply(ok(arr), std::move(cb));
    }, {drogon::Get});

    // ALL editable text: every i18n string key for a locale (default + override),
    // grouped by top-level namespace. Powers the "全部文本" customization view.
    app.registerHandler("/api/i18n/all", [st, &i18n](Req req, CB&& cb) {
        try {
            std::string lang = "zh-Hans";
            if (auto it = req->getParameter("lang"); !it.empty()) lang = it;
            Locale loc = localeFromString(lang);
            auto defaults = i18n.flatten(loc);
            // overrides for this locale from DB
            std::map<std::string, std::string> ov;
            if (st) for (auto& r : st->get_all<I18nOverrideRow>())
                if (r.locale == lang) ov[r.key] = r.value;
            J arr = J::array();
            for (auto& [key, def] : defaults) {
                auto oi = ov.find(key);
                arr.push_back(J{
                    {"key", key},
                    {"group", key.substr(0, key.find('.'))},
                    {"default", def},
                    {"override", oi != ov.end() ? J(oi->second) : J(nullptr)},
                    {"v2key", legacyv2::v2KeyFor(key)}
                });
            }
            // Override-only keys not in the bundle (e.g. imported legacy.* orphans).
            for (auto& [key, val] : ov) {
                if (defaults.count(key)) continue;
                arr.push_back(J{
                    {"key", key},
                    {"group", key.substr(0, key.find('.'))},
                    {"default", ""},
                    {"override", val},
                    {"v2key", legacyv2::v2KeyFor(key)}
                });
            }
            jsonReply(ok(arr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // Import a legacy Dice! V2 [DiceData] folder.  POST {dir, overwrite?}.
    // Enhanced — passes CardDeck/LuaPluginManager for reload, accepts overwrite option,
    // returns structured ImportResult for decks and mods.
    app.registerHandler("/api/legacy/import", [&db, &cfg, &i18n, &replyMgr, &cardDeck, &luaMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string dir = j.value("dir", "");
            if (dir.empty()) { jsonReply(fail("dir required"), std::move(cb)); return; }
            legacyv2::ImportOptions opts;
            opts.overwrite = j.value("overwrite", false);
            J report = legacyv2::runImport(db, cfg, i18n, replyMgr, dir, &cardDeck, &luaMod, opts);
            if (report.value("ok", false)) jsonReply(ok(report), std::move(cb));
            else jsonReply(fail(report.value("error", std::string("import failed"))), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Full local backup. The archive contains the complete split config/
    // directory and runtime data/ after all SQLite WAL stores are checkpointed.
    app.registerHandler("/api/backup/export", [&db, &cfg](Req, CB&& cb) {
        try {
            std::filesystem::path archive; std::string error;
            if (!backup::createArchive(db, cfg.configPath(), archive, error)) { jsonReply(fail(error), std::move(cb)); return; }
            auto resp = drogon::HttpResponse::newFileResponse(archive.string());
            resp->setContentTypeString("application/zip");
            resp->addHeader("Content-Disposition", "attachment; filename=\"" + archive.filename().string() + "\"");
            cb(resp);
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // Create a server-side archive. Downloading is deliberately a separate
    // action so operators can keep a local restore point without a browser.
    app.registerHandler("/api/backup/create", [&db, &cfg](Req req, CB&& cb) {
        try {
            const J body = req->body().empty() ? J::object() : J::parse(req->body());
            const backup::Selection selection = backup::Selection::fromJson(body.value("selection", J::object()));
            std::filesystem::path archive; std::string error;
            if (!backup::createArchive(db, cfg.configPath(), archive, error, selection)) { jsonReply(fail(error), std::move(cb)); return; }
            std::error_code ec;
            jsonReply(ok(J{{"name", archive.filename().string()}, {"size", std::filesystem::file_size(archive, ec)},
                           {"automatic", false}, {"selection", selection.toJson()}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Stored backup management.  Archives remain in data/backups/ after download so
    // the WebUI can list, download again, or delete them later.
    app.registerHandler("/api/backup/list", [](Req, CB&& cb) {
        jsonReply(ok(backup::listArchives()), std::move(cb));
    }, {drogon::Get});
    app.registerHandler("/api/backup/download", [](Req req, CB&& cb) {
        try {
            const std::string name = req->getParameter("name");
            if (!backup::isSafeArchiveName(name)) { jsonReply(fail("无效的备份文件名"), std::move(cb)); return; }
            const bool automatic = req->getParameter("automatic") == "1";
            const auto path = backup::archiveDirectory(automatic) / name;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) { jsonReply(fail("备份文件不存在"), std::move(cb)); return; }
            auto resp = drogon::HttpResponse::newFileResponse(path.string());
            resp->setContentTypeString("application/zip");
            resp->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
            cb(resp);
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});
    app.registerHandler("/api/backup/{1}", [](Req req, CB&& cb, const std::string& name) {
        try {
            std::string error;
            if (!backup::deleteArchive(name, req->getParameter("automatic") == "1", error)) { jsonReply(fail(error), std::move(cb)); return; }
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    app.registerHandler("/api/backup/config", [&cfg](Req, CB&& cb) {
        const backup::Selection selection = backup::Selection::fromJson(
            cfg.get<J>("backup/auto_selection", J::object()));
        jsonReply(ok(J{{"enabled", cfg.get<bool>("backup/auto_enabled", false)},
                       {"schedule", cfg.get<std::string>("backup/auto_schedule", "interval")},
                       {"intervalHours", cfg.get<int>("backup/auto_interval_hours", 24)},
                       {"dailyTime", cfg.get<std::string>("backup/auto_daily_time", "04:00")},
                       {"keepDays", cfg.get<int>("backup/auto_keep_days", 7)},
                       {"selection", selection.toJson()},
                       {"lastAutoAt", cfg.get<long long>("backup/auto_last_at", 0)}}), std::move(cb));
    }, {drogon::Get});
    app.registerHandler("/api/backup/config", [&cfg](Req req, CB&& cb) {
        try {
            const J j = J::parse(req->body());
            const bool enabled = j.value("enabled", false);
            const std::string schedule = j.value("schedule", std::string("interval"));
            const int hours = j.value("intervalHours", 24);
            const std::string daily = j.value("dailyTime", std::string("04:00"));
            const int keepDays = j.value("keepDays", 7);
            const backup::Selection selection = backup::Selection::fromJson(j.value("selection", J::object()));
            const bool validTime = daily.size() == 5 && daily[2] == ':' &&
                std::isdigit(static_cast<unsigned char>(daily[0])) && std::isdigit(static_cast<unsigned char>(daily[1])) &&
                std::isdigit(static_cast<unsigned char>(daily[3])) && std::isdigit(static_cast<unsigned char>(daily[4])) &&
                std::stoi(daily.substr(0, 2)) < 24 && std::stoi(daily.substr(3, 2)) < 60;
            if ((schedule != "interval" && schedule != "daily") || hours < 1 || hours > 720 || keepDays < 1 || keepDays > 3650 || !validTime) {
                jsonReply(fail("自动备份配置无效"), std::move(cb)); return;
            }
            cfg.set<bool>("backup/auto_enabled", enabled);
            cfg.set<std::string>("backup/auto_schedule", schedule);
            cfg.set<int>("backup/auto_interval_hours", hours);
            cfg.set<std::string>("backup/auto_daily_time", daily);
            cfg.set<int>("backup/auto_keep_days", keepDays);
            cfg.set<J>("backup/auto_selection", selection.toJson());
            if (!cfg.save()) { jsonReply(fail("保存自动备份配置失败"), std::move(cb)); return; }
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Restore is deliberately staged instead of touching live stores.  The
    // next process start applies it before opening config/database and keeps a
    // rollback copy of the current data directory.
    app.registerHandler("/api/backup/restore-stored", [](Req req, CB&& cb) {
        try {
            const J body = J::parse(req->body());
            const std::string name = body.value("name", std::string());
            const bool automatic = body.value("automatic", false);
            std::string error;
            if (!backup::stageStoredRestore(name, automatic, error)) { jsonReply(fail(error), std::move(cb)); return; }
            jsonReply(ok(J{{"restartRequired", true}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});
    app.registerHandler("/api/backup/restore", [](Req req, CB&& cb) {
        try {
            constexpr size_t kMaxBackupBytes = 2ull * 1024 * 1024 * 1024;
            if (req->body().size() > kMaxBackupBytes) { jsonReply(fail("备份文件超过 2 GiB 上限"), std::move(cb)); return; }
            std::string error;
            if (!backup::stageRestore(std::string(req->body()), error)) { jsonReply(fail(error), std::move(cb)); return; }
            jsonReply(ok(J{{"restartRequired", true}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Upload a deck file (multipart or JSON body). Writes to data/decks/ and reloads.
    app.registerHandler("/api/decks/upload", [&cardDeck](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string filename = j.value("filename", "");
            std::string content = j.value("content", "");
            if (filename.empty() || content.empty()) {
                jsonReply(fail("filename and content required"), std::move(cb)); return;
            }
            // Validate JSON structure
            std::string validationError = legacyv2::validateDeckJson(content);
            if (!validationError.empty()) {
                jsonReply(fail("invalid deck JSON: " + validationError), std::move(cb)); return;
            }
            namespace fs = std::filesystem;
            fs::path decksDir = "data/decks";
            fs::create_directories(decksDir);
            fs::path target = decksDir / filename;
            std::ofstream out(target, std::ios::binary);
            out << content;
            out.close();
            // Reload decks
            cardDeck.loadDir("data/decks");
            jsonReply(ok(J{{"filename", filename}, {"total_decks", cardDeck.deckCount()}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Upload a mod file (Lua/json/zip). 归位语义统一走 LuaPluginManager::importUpload
    //（旧实现把 zip 原样落盘不解压，产生既加载不了也删不掉的孤儿文件）。
    app.registerHandler("/api/mods/upload", [&luaMod](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string filename = j.value("filename", "");
            std::string content = j.value("content", "");
            if (filename.empty() || content.empty()) {
                jsonReply(fail("filename and content required"), std::move(cb)); return;
            }
            // Content may be base64-encoded (dataURL, for binary files like .zip)
            std::string bytes;
            if (content.rfind("data:", 0) == 0) {
                size_t comma = content.find(',');
                if (comma != std::string::npos) content = content.substr(comma + 1);
                bytes = drogon::utils::base64Decode(content);
            } else bytes = content;
            std::string err; std::vector<std::string> names;
            if (!luaMod.importUpload(filename, bytes, &err, &names)) { jsonReply(fail(err), std::move(cb)); return; }
            luaMod.reload();
            jsonReply(ok(J{{"filename", filename}, {"imported", names}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Development roadmap (rendered as a progress page in the web admin).
    app.registerHandler("/api/roadmap", [](Req, CB&& cb) {
        std::string content;
        for (const char* p : {"docs/roadmap.md", "../docs/roadmap.md", "roadmap.md"}) {
            std::ifstream f(p);
            if (f) { std::stringstream ss; ss << f.rdbuf(); content = ss.str(); break; }
        }
        if (content.empty()) { jsonReply(fail("roadmap not found"), std::move(cb)); return; }
        jsonReply(ok(J{{"content", content}}), std::move(cb));
    }, {drogon::Get});

    app.registerHandler("/api/system/settings", [&cfg](Req, CB&& cb) {
        jsonReply(ok(J{
            {"host", cfg.get<std::string>("server/host","0.0.0.0")},
            {"port", cfg.get<int>("server/port",18088)},
            {"log_level", cfg.get<std::string>("server/log_level","info")},
            {"hot_reload", cfg.get<bool>("hot_reload/enabled",true)}
        }), std::move(cb));
    }, {drogon::Get});

    // 统一时区：server/timezone_minutes（相对 UTC 的分钟偏移，东为正；
    // null = 跟随系统本地时区）。日志上传/展示、审计、备份名、定时任务等
    // 面向用户的时间统一用它。
    app.registerHandler("/api/system/timezone", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body(), nullptr, false);
                if (!j.is_object() || !j.contains("offset_minutes")) {
                    jsonReply(fail("offset_minutes required"), std::move(cb)); return;
                }
                int minutes = (std::numeric_limits<int>::min)();
                if (j["offset_minutes"].is_number()) {
                    minutes = j["offset_minutes"].get<int>();
                    if (minutes < -720 || minutes > 840) {
                        jsonReply(fail("offset must be between -720 and 840 minutes"), std::move(cb)); return;
                    }
                } else if (!j["offset_minutes"].is_null()) {
                    jsonReply(fail("offset_minutes must be a number or null"), std::move(cb)); return;
                }
                cfg.set<int>("server/timezone_minutes", minutes);
                cfg.save();
                utils::setTimezoneOffset(minutes);
            }
            int minutes = cfg.get<int>("server/timezone_minutes", (std::numeric_limits<int>::min)());
            jsonReply(ok(J{
                {"offset_minutes", minutes == (std::numeric_limits<int>::min)() ? J(nullptr) : J(minutes)},
                {"effective_offset_minutes", utils::effectiveTimezoneOffsetMinutes()},
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── Dashboard ─────────────────────────────────────────────
    // Live host metrics for the dashboard server-info curve (polled frequently).
    app.registerHandler("/api/system/sysinfo", [](Req, CB&& cb) {
        jsonReply(ok(sysInfoJson()), std::move(cb));
    }, {drogon::Get});

    // 平台能力位：{platform: {kick,ban,poke,friends,...}}。前端按能力显示/隐藏
    // 群管按钮等，禁止按 platform 字符串硬编码猜功能。
    app.registerHandler("/api/platform-caps", [&adapterMgr](Req, CB&& cb) {
        J caps = J::object();
        for (auto& a : adapterMgr.allAdapters())
            if (a && !caps.contains(a->platform())) caps[a->platform()] = a->capabilities();
        jsonReply(ok(caps), std::move(cb));
    }, {drogon::Get});

    app.registerHandler("/api/dashboard/stats", [st, lst, &adapterMgr](Req, CB&& cb) {
        try {
            int adapters = (int)st->count<AdapterRow>();
            int rules    = (int)st->count<ReplyRuleRow>();

            int sessions = (int)lst->count<GameLogRow>();
            int connectedCount = 0;
            for (auto& a : adapterMgr.allAdapters())
                if (a->isConnected()) ++connectedCount;
            // Read last 20 lines from log file for dashboard display
            J logs = J::array();
            auto readRecentLogs = []() -> J {
                J arr = J::array();
                // 运行日志已迁到 data/logs/app/dice-<启动时间戳>_<日期>.log，且每次启动+每日
                // 切割成新文件（#31）。首页要读「最新的那个」，否则会卡在旧文件/旧路径。
                namespace fs = std::filesystem; std::error_code ec;
                fs::path logFile; fs::file_time_type newest{};
                for (const char* dir : {"data/logs/app", "../data/logs/app"}) {
                    if (!fs::is_directory(dir, ec)) continue;
                    for (const auto& e : fs::directory_iterator(dir, ec)) {
                        if (!e.is_regular_file(ec)) continue;
                        const std::string name = dnx_u8str(e.path().filename());
                        if (name.rfind("dice-", 0) != 0 || e.path().extension() != ".log") continue;
                        auto t = e.last_write_time(ec);
                        if (logFile.empty() || t > newest) { newest = t; logFile = e.path(); }
                    }
                    if (!logFile.empty()) break;   // 第一个存在的目录里取最新即可
                }
                std::ifstream f;
                if (!logFile.empty()) f.open(logFile);
                if (!f) { f.open("logs/dice.log"); }          // 兼容旧位置
                if (!f) { f.open("../logs/dice.log"); }
                if (!f) return arr;
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(f, line)) {
                    // Strip invalid UTF-8 bytes to avoid JSON serialization errors
                    std::string clean;
                    for (size_t j = 0; j < line.size(); ) {
                        unsigned char c = line[j];
                        if (c < 0x80) { clean += c; ++j; }
                        else if (c < 0xC0) { ++j; }  // orphan continuation — skip
                        else if (c < 0xE0) { if (j+1<line.size()) { clean += line.substr(j,2); } j+=2; }
                        else if (c < 0xF0) { if (j+2<line.size()) { clean += line.substr(j,3); } j+=3; }
                        else { if (j+3<line.size()) { clean += line.substr(j,4); } j+=4; }
                    }
                    // A new entry begins with "[YYYY-MM-DD HH:MM:SS.mmm]". Anything
                    // else is a continuation of a multi-line message (换行消息) —
                    // fold it back into the previous entry, joined by a newline, so
                    // it shows as ONE log line in the WebUI instead of splitting.
                    bool isNewEntry = clean.size() > 24 && clean[0] == '[' && clean[24] == ']'
                                      && clean[5] == '-' && clean[8] == '-' && clean[11] == ' ';
                    if (!isNewEntry && !lines.empty()) {
                        lines.back() += "\n" + clean;
                    } else if (!clean.empty()) {
                        lines.push_back(clean);
                    }
                }
                int start = (int)lines.size() - 100;
                if (start < 0) start = 0;
                for (int i = start; i < (int)lines.size(); ++i) {
                    // Parse "[2026-06-14 13:52:54.340] [info] [dice] message"
                    std::string ts, lvl, msg = lines[i];
                    if (msg.size() > 26 && msg[0] == '[') {
                        ts = msg.substr(1, 23);  // 2026-06-14 13:52:54.340
                        auto lvlEnd = msg.find(']', 25);
                        if (lvlEnd != std::string::npos) {
                            lvl = msg.substr(27, lvlEnd - 27); // info/warn/error
                            auto diceEnd = msg.find(']', lvlEnd + 1);
                            if (diceEnd != std::string::npos) {
                                msg = msg.substr(diceEnd + 2);
                            }
                        }
                    }
                    arr.push_back({
                        {"id", std::to_string(i)},
                        {"timestamp", ts},
                        {"level", lvl.empty() ? "info" : lvl},
                        {"message", msg}
                    });
                }
                return arr;
            };
            jsonReply(ok(J{
                {"uptime_seconds", static_cast<int>(std::time(nullptr) - utils::getStartupEpoch())},
                {"active_connections", connectedCount},
                {"total_adapters", adapters},
                {"total_commands", static_cast<int>(CommandRouter::commandCount())},
                {"total_rules", rules},
                {"active_sessions", sessions},
                {"system", sysInfoJson()},
                {"recent_logs", readRecentLogs()}
            }), std::move(cb));
        } catch (const std::exception&) {
            // Fallback: return stats without logs if log parsing fails
            jsonReply(ok(J{
                {"uptime_seconds", static_cast<int>(std::time(nullptr) - utils::getStartupEpoch())},
                {"active_connections", 0},
                {"total_adapters", (int)st->count<AdapterRow>()},
                {"total_commands", static_cast<int>(CommandRouter::commandCount())},
                {"total_rules", (int)st->count<ReplyRuleRow>()},
                {"active_sessions", (int)lst->count<GameLogRow>()},
                {"system", sysInfoJson()},
                {"recent_logs", J::array()}
            }), std::move(cb));
        }
    }, {drogon::Get});

    // ── QQ 官方机器人 2.0：扫码绑定（二维码密钥仅保留在服务端） ──
    app.registerHandler("/api/adapters/qq-official/qr/start", [](Req, CB&& cb) {
        QQOfficialAdapter::beginQrLogin([cb=std::move(cb)](QQOfficialAdapter::QrResult r) mutable {
            jsonReply(r.ok ? ok(r.data) : fail(r.error), std::move(cb));
        });
    }, {drogon::Post});
    app.registerHandler("/api/adapters/qq-official/qr/{1}", [](Req, CB&& cb, const std::string& sessionId) {
        QQOfficialAdapter::pollQrLogin(sessionId, [cb=std::move(cb)](QQOfficialAdapter::QrResult r) mutable {
            jsonReply(r.ok ? ok(r.data) : fail(r.error), std::move(cb));
        });
    }, {drogon::Get});

    // ── Adapters (DB-backed, status from AdapterManager) ──────
    app.registerHandler("/api/adapters", [st, &adapterMgr, &cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Get) {
                auto rows = st->get_all<AdapterRow>();
                J arr = J::array();
                for (auto& r : rows) {
                    auto j = adapterToJson(r, adapterMgr.lastActiveAt(std::to_string(r.id)));
                    // Inject live connection status from adapter manager
                    auto adapter = adapterMgr.getAdapter(std::to_string(r.id));
                    j["status"] = adapter ? adapter->connectionStatus() : std::string("disconnected");
                    if (adapter) {
                        j["loginId"] = adapter->getLoginId();
                        j["loginName"] = adapter->getLoginName();
                        if (auto official = std::dynamic_pointer_cast<QQOfficialAdapter>(adapter)) {
                            j["qqNumber"] = official->displayQQ();
                            j["shareUrl"] = official->shareUrl();
                        }
                    }
                    arr.push_back(j);
                }
                jsonReply(ok(arr), std::move(cb));
            } else if (req->method() == drogon::Post) {
                auto j = J::parse(req->body());
                AdapterRow a;
                a.name = j.value("name", "");
                a.enabled = j.value("enabled", true);
                const std::string type = j.value("type", std::string("onebot_v11"));
                a.type = static_cast<int>(adapterTypeFromString(type));
                if (a.type == static_cast<int>(AdapterType::kUnknown)) throw std::runtime_error("不支持的适配器类型");
                const std::string heartApiKey = j.value("heartApiKey", std::string());
                if (a.type == static_cast<int>(AdapterType::kQQOfficial)) {
                    const std::string appId = j.value("appId", std::string());
                    const std::string appSecret = j.value("appSecret", std::string());
                    if (appId.empty() || appSecret.empty()) throw std::runtime_error("QQ 官方机器人需要 AppID 和 AppSecret");
                    a.connectionMode = 0; a.endpoint.clear(); a.accessToken.clear();
                    a.config = J{{"appId", appId}, {"appSecret", appSecret},
                                 {"qqNumber", j.value("qqNumber", std::string())},
                                 {"forceVerifyImageResource", j.value("forceVerifyImageResource", false)},
                                 {"heartApiKey", heartApiKey}}.dump();
                } else if (a.type == static_cast<int>(AdapterType::kDiscord)
                           || a.type == static_cast<int>(AdapterType::kKook)) {
                    // Token 存 accessToken 列；无 endpoint / 连接模式概念。
                    a.accessToken = j.value("accessToken", std::string());
                    if (a.accessToken.empty()) throw std::runtime_error("需要 Bot Token");
                    a.connectionMode = 0; a.endpoint.clear();
                    a.config = J{{"heartApiKey", heartApiKey}}.dump();
                } else {
                    a.endpoint = j.value("endpoint", ""); a.accessToken = j.value("accessToken", "");
                    std::string mode = j.value("connectionMode", "forward_ws");
                    a.connectionMode = (mode == "reverse_ws") ? 1 : (mode == "http") ? 2 : 0;
                    a.config = J{{"heartApiKey", heartApiKey}}.dump();
                }
                a.id = st->insert(a);
                persistAdaptersToConfig(st, cfg);

                // Create real adapter instance and auto-start if enabled
                if (a.enabled) {
                    auto adapter = makeRuntimeAdapter(a);
                    adapterMgr.registerAdapter(adapter);
                    adapterMgr.startAdapter(std::to_string(a.id));
                }

                jsonReply(ok(adapterToJson(a, adapterMgr.lastActiveAt(std::to_string(a.id)))), std::move(cb));
            } else { jsonReply(fail("Method not allowed"), std::move(cb)); }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    // PUT/DELETE /api/adapters/{id}
    app.registerHandler("/api/adapters/{1}", [st, &adapterMgr, &cfg](Req req, CB&& cb, const std::string& id) {
        try {
            int aid = std::stoi(id);
            if (req->method() == drogon::Put || req->method() == drogon::Patch) {
                auto j = J::parse(req->body());
                auto a = st->get<AdapterRow>(aid);
                J adapterCfg = J::parse(a.config, nullptr, false);
                if (!adapterCfg.is_object()) adapterCfg = J::object();
                if (j.contains("name")) a.name = j["name"];
                bool credentialsChanged = false;
                if (a.type == static_cast<int>(AdapterType::kQQOfficial)) {
                    if (j.contains("appId")) { credentialsChanged = credentialsChanged || adapterCfg.value("appId", std::string()) != j["appId"].get<std::string>(); adapterCfg["appId"] = j["appId"]; }
                    if (j.contains("appSecret") && j["appSecret"].is_string() && !j["appSecret"].get<std::string>().empty()) { credentialsChanged = credentialsChanged || adapterCfg.value("appSecret", std::string()) != j["appSecret"].get<std::string>(); adapterCfg["appSecret"] = j["appSecret"]; }
                    if (j.contains("qqNumber") && j["qqNumber"].is_string()) adapterCfg["qqNumber"] = j["qqNumber"];
                    if (j.contains("forceVerifyImageResource") && j["forceVerifyImageResource"].is_boolean()) {
                        credentialsChanged = credentialsChanged || adapterCfg.value("forceVerifyImageResource", false) != j["forceVerifyImageResource"].get<bool>();
                        adapterCfg["forceVerifyImageResource"] = j["forceVerifyImageResource"];
                    }
                    if (adapterCfg.value("appId", std::string()).empty() || adapterCfg.value("appSecret", std::string()).empty()) throw std::runtime_error("QQ 官方机器人需要 AppID 和 AppSecret");
                } else { if (j.contains("endpoint")) a.endpoint = j["endpoint"]; if (j.contains("accessToken")) a.accessToken = j["accessToken"]; }
                if (j.contains("heartApiKey") && j["heartApiKey"].is_string())
                    adapterCfg["heartApiKey"] = j["heartApiKey"].get<std::string>();
                a.config = adapterCfg.dump();
                bool wasEnabled = a.enabled;
                if (j.contains("enabled")) a.enabled = j["enabled"];
                if (j.contains("connectionMode")) {
                    std::string m = j["connectionMode"];
                    a.connectionMode = (m == "reverse_ws") ? 1 : (m == "http") ? 2 : 0;
                }
                st->update(a);
                persistAdaptersToConfig(st, cfg);

                // Handle toggle in AdapterManager
                if (a.enabled && !wasEnabled) {
                    auto adapter = makeRuntimeAdapter(a);
                    adapterMgr.registerAdapter(adapter);
                    adapterMgr.startAdapter(std::to_string(a.id));
                } else if (a.enabled && wasEnabled && credentialsChanged) {
                    adapterMgr.stopAdapter(std::to_string(a.id));
                    adapterMgr.unregisterAdapter(std::to_string(a.id));
                    auto adapter = makeRuntimeAdapter(a);
                    adapterMgr.registerAdapter(adapter);
                    adapterMgr.startAdapter(std::to_string(a.id));
                } else if (!a.enabled && wasEnabled) {
                    adapterMgr.stopAdapter(std::to_string(a.id));
                    adapterMgr.unregisterAdapter(std::to_string(a.id));
                }

                // Inject live connection status into response
                auto jj = adapterToJson(a, adapterMgr.lastActiveAt(std::to_string(a.id)));
                auto adapter = adapterMgr.getAdapter(std::to_string(a.id));
                jj["status"] = adapter ? adapter->connectionStatus() : std::string("disconnected");
                if (adapter) {
                    jj["loginId"] = adapter->getLoginId();
                    jj["loginName"] = adapter->getLoginName();
                }
                jsonReply(ok(jj), std::move(cb));
            } else if (req->method() == drogon::Delete) {
                adapterMgr.stopAdapter(std::to_string(aid));
                adapterMgr.unregisterAdapter(std::to_string(aid));
                st->remove<AdapterRow>(aid);
                scoped_settings::eraseTarget(cfg, "account", id);
                persistAdaptersToConfig(st, cfg);
                jsonReply(ok(nullptr), std::move(cb));
            } else { jsonReply(fail("Method not allowed"), std::move(cb)); }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Delete, drogon::Patch});

    // POST /api/adapters/{id}/test — connection check
    app.registerHandler("/api/adapters/{1}/test", [st, &adapterMgr](Req, CB&& cb, const std::string& id) {
        try {
            int aid = std::stoi(id);
            auto a = st->get<AdapterRow>(aid);
            auto adapter = makeRuntimeAdapter(a);
            adapter->start();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            bool connected = adapter->isConnected();
            adapter->stop();
            jsonReply(connected ? ok(J{{"success",true},{"message","连接测试成功"}})
                                : fail(adapter->lastError().empty() ? "连接失败" : adapter->lastError()), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // POST /api/adapters/{id}/reconnect — manually resume a timed-out adapter
    // (resets the reconnect backoff). The adapter stays enabled; this is NOT an un-disable.
    app.registerHandler("/api/adapters/{1}/reconnect", [st, &adapterMgr](Req, CB&& cb, const std::string& id) {
        try {
            int aid = std::stoi(id);
            auto a = st->get<AdapterRow>(aid);
            auto adapter = adapterMgr.getAdapter(std::to_string(aid));
            if (!adapter) {
                // Not registered (e.g. was disabled) → register + start fresh.
                if (!a.enabled) { a.enabled = true; st->update(a); }
                auto na = makeRuntimeAdapter(a);
                adapterMgr.registerAdapter(na);
                adapterMgr.startAdapter(std::to_string(a.id));
            } else {
                adapter->resumeConnection();
            }
            jsonReply(ok(J{{"success", true}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // ── Dice Rules ────────────────────────────────────────────
    app.registerHandler("/api/dice/rules", [st,&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Get) {
                jsonReply(ok(J{
                    {"coc_enabled", cfg.get<bool>("dice/rules/coc_enabled",true)},
                    {"coc_critical_range", cfg.get<int>("dice/rules/coc_critical_range",1)},
                    {"coc_fumble_range", cfg.get<int>("dice/rules/coc_fumble_range",95)},
                    {"dnd_enabled", cfg.get<bool>("dice/rules/dnd_enabled",true)},
                    {"fate_enabled", cfg.get<bool>("dice/rules/fate_enabled",false)},
                    {"l5r_enabled", cfg.get<bool>("dice/rules/l5r_enabled",false)},
                    {"default_dice_sides", cfg.get<int>("dice/rules/default_dice_sides",100)},
                    {"command_prefix", cfg.get<std::string>("dice/rules/command_prefix",".")}
                }), std::move(cb));
            } else if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("coc_enabled")) cfg.set<bool>("dice/rules/coc_enabled", j["coc_enabled"]);
                if (j.contains("dnd_enabled")) cfg.set<bool>("dice/rules/dnd_enabled", j["dnd_enabled"]);
                if (j.contains("fate_enabled")) cfg.set<bool>("dice/rules/fate_enabled", j["fate_enabled"]);
                if (j.contains("default_dice_sides")) cfg.set<int>("dice/rules/default_dice_sides", j["default_dice_sides"]);
                if (j.contains("command_prefix")) cfg.set<std::string>("dice/rules/command_prefix", j["command_prefix"]);
                if (j.contains("coc_critical_range")) cfg.set<int>("dice/rules/coc_critical_range", j["coc_critical_range"]);
                if (j.contains("coc_fumble_range")) cfg.set<int>("dice/rules/coc_fumble_range", j["coc_fumble_range"]);
                cfg.save();
                jsonReply(ok(), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── 全局设置 ────────────────────────────────────────────────
    // GET 读取全部（带默认值）；PUT 写入任意提供的键。存于 dice/ 命名空间。
    app.registerHandler("/api/system/global", [&cfg, &adapterMgr](Req req, CB&& cb) {
        // {key, default-bool} and {key, default-int}; keep in sync with GET/PUT.
        static const std::vector<std::pair<std::string,bool>> kBools = {
            {"silent_global", false},
            {"disabled_jrrp", false}, {"disabled_me", false}, {"disabled_deck", false},
            {"disabled_draw", false}, {"disabled_send", false}, {"disabled_help", false},
            {"deck_hide_underscore", true},
            {"listen_group_request", true}, {"listen_group_add", true},
            {"listen_friend_request", true}, {"listen_friend_add", true},
            {"listen_at_when_off", true},
            {"allow_official_direct_bind", false},
            {"private_mode", false}, {"check_group_license", false}, {"leave_discuss", false},
            {"leave_black_qq", false},
            {"cloud_visible", true}, {"cloud_black_share", true},
            {"api_enabled", false},
        };
        static const std::vector<std::pair<std::string,int>> kInts = {
            {"allow_stranger", 1}, {"inactive_user_line", 0}, {"inactive_group_line", 0},
            {"group_clear_limit", 20}, {"group_invalid_size", 500}, {"api_timeout", 5},
        };
        try {
            if (req->method() == drogon::Get) {
                J out = J::object();
                for (auto& [k, d] : kBools) out[k] = cfg.get<bool>("dice/" + k, d);
                for (auto& [k, d] : kInts)  out[k] = cfg.get<int>("dice/" + k, d);
                jsonReply(ok(out), std::move(cb));
            } else {
                auto j = J::parse(req->body());
                // B：关键全局开关变更 → 通知骰主（仅在值真的变化时）。
                bool oldSilent  = cfg.get<bool>("dice/silent_global", false);
                bool oldPrivate = cfg.get<bool>("dice/private_mode", false);
                for (auto& [k, d] : kBools) if (j.contains(k) && j[k].is_boolean()) cfg.set<bool>("dice/" + k, j[k].get<bool>());
                for (auto& [k, d] : kInts)  if (j.contains(k) && j[k].is_number()) cfg.set<int>("dice/" + k, j[k].get<int>());
                cfg.save();
                bool newSilent  = cfg.get<bool>("dice/silent_global", false);
                bool newPrivate = cfg.get<bool>("dice/private_mode", false);
                if (newSilent != oldSilent)
                    dice::notice::notify(cfg, adapterMgr, dice::notice::kImportant,
                        newSilent ? "\xe5\x85\xa8\xe5\xb1\x80\xe9\x9d\x99\xe9\xbb\x98\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf\xef\xbc\x88\xe7\xbd\x91\xe9\xa1\xb5\xef\xbc\x89"
                                  : "\xe5\x85\xa8\xe5\xb1\x80\xe9\x9d\x99\xe9\xbb\x98\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad\xef\xbc\x88\xe7\xbd\x91\xe9\xa1\xb5\xef\xbc\x89", "", "", "global");
                if (newPrivate != oldPrivate)
                    dice::notice::notify(cfg, adapterMgr, dice::notice::kImportant,
                        newPrivate ? "\xe5\xb7\xb2\xe5\x88\x87\xe6\x8d\xa2\xe4\xb8\xba\xe7\xa7\x81\xe7\x94\xa8\xe6\xa8\xa1\xe5\xbc\x8f\xef\xbc\x88\xe7\xbd\x91\xe9\xa1\xb5\xef\xbc\x89"
                                   : "\xe5\xb7\xb2\xe5\x88\x87\xe6\x8d\xa2\xe4\xb8\xba\xe5\x85\xac\xe7\x94\xa8\xe6\xa8\xa1\xe5\xbc\x8f\xef\xbc\x88\xe7\xbd\x91\xe9\xa1\xb5\xef\xbc\x89", "", "", "global");
                jsonReply(ok(), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── 指令前缀 ───────────────────────────────────────────────
    app.registerHandler("/api/system/prefixes", [&cfg](Req req, CB&& cb) {
        static const std::vector<std::string> kDefault = {".", "\xe3\x80\x82", "!", "\xef\xbc\x81"};
        try {
            if (req->method() == drogon::Get) {
                J all = cfg.getAll();
                J arr = J::array();
                if (all.contains("dice") && all["dice"].contains("command_prefixes") &&
                    all["dice"]["command_prefixes"].is_array())
                    for (auto& e : all["dice"]["command_prefixes"])
                        if (e.is_string() && !e.get<std::string>().empty()) arr.push_back(e);
                if (arr.empty()) for (auto& d : kDefault) arr.push_back(d);
                jsonReply(ok(J{{"prefixes", arr}}), std::move(cb));
            } else {
                auto j = J::parse(req->body());
                J arr = J::array();
                if (j.contains("prefixes") && j["prefixes"].is_array())
                    for (auto& e : j["prefixes"])
                        if (e.is_string() && !e.get<std::string>().empty()) arr.push_back(e);
                if (arr.empty()) { jsonReply(fail("至少需要一个前缀"), std::move(cb)); return; }
                cfg.set<J>("dice/command_prefixes", arr);
                cfg.save();
                jsonReply(ok(J{{"prefixes", arr}}), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── Group management helpers (moved above handlers that use them) ──
    static auto gsGet = [](decltype(st) s, const std::string& plat, const std::string& gid,
                           const std::string& key) -> std::string {
        auto rows = s->template get_all<GroupSettingRow>(
            orm::where(orm::c(&GroupSettingRow::platform) == plat
                and orm::c(&GroupSettingRow::groupId) == gid
                and orm::c(&GroupSettingRow::key) == key));
        return rows.empty() ? std::string() : rows.front().value;
    };
    static auto gsSet = [](decltype(st) s, const std::string& plat, const std::string& gid,
                           const std::string& key, const std::string& val) {
        auto rows = s->template get_all<GroupSettingRow>(
            orm::where(orm::c(&GroupSettingRow::platform) == plat
                and orm::c(&GroupSettingRow::groupId) == gid
                and orm::c(&GroupSettingRow::key) == key));
        if (rows.empty()) { GroupSettingRow r; r.platform=plat; r.groupId=gid; r.key=key; r.value=val; s->insert(r); }
        else { auto r = rows.front(); r.value = val; s->update(r); }
    };
    static auto agsGet = [](decltype(st) s, const std::string& adapterId,
                            const std::string& plat, const std::string& gid,
                            const std::string& key) -> std::string {
        return accountGroupSetting(*s, adapterId, plat, gid, key);
    };
    static auto agsSet = [](decltype(st) s, const std::string& adapterId,
                            const std::string& plat, const std::string& gid,
                            const std::string& endpointId, const std::string& key,
                            const std::string& val) {
        setAccountGroupSetting(*s, adapterId, plat, gid, endpointId, key, val);
    };

    // ── 日志站配置：API 地址（可自建）+ 上传协议（seal=海豹V1默认 / legacy=旧多段txt）──
    app.registerHandler("/api/system/logsite", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Get) {
                jsonReply(ok(J{
                    {"url", logsvc::uploadUrl(cfg)},
                    {"format", logsvc::uploadFormat(cfg)},
                    {"official", std::string(logsvc::kOfficialLogsite)},
                }), std::move(cb));
            } else {
                auto j = J::parse(req->body());
                if (j.contains("url")) {
                    std::string u = j["url"].is_string() ? j["url"].get<std::string>() : std::string();
                    cfg.set<std::string>("dice/logsite_url", u);   // 空串=恢复官方默认
                }
                if (j.contains("format")) {
                    std::string f = j["format"].is_string() ? j["format"].get<std::string>() : std::string("dicenext");
                    if (f != "seal" && f != "seal_v105" && f != "dicenext" && f != "legacy") { jsonReply(fail("invalid format"), std::move(cb)); return; }
                    cfg.set<std::string>("dice/logsite_format", f);
                }
                cfg.save();
                jsonReply(ok(J{{"url", logsvc::uploadUrl(cfg)}, {"format", logsvc::uploadFormat(cfg)},
                              {"official", std::string(logsvc::kOfficialLogsite)}}), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── 心跳上报（heart.dice.zone）：全局调度配置 + 各适配器最近状态。
    // API Key 属于适配器，在 /api/adapters 中配置。
    app.registerHandler("/api/system/heartbeat", [&cfg](Req req, CB&& cb) {
        try {
            auto& hs = dice::heart::HeartService::instance();
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("enabled") && j["enabled"].is_boolean())
                    cfg.set<bool>("dice/heart_enabled", j["enabled"].get<bool>());
                if (j.contains("url")) {
                    std::string u = j["url"].is_string() ? j["url"].get<std::string>() : std::string();
                    if (u.empty()) u = dice::heart::kOfficialHeartUrl;   // 空串=恢复官方默认
                    cfg.set<std::string>("dice/heart_url", u);
                }
                if (j.contains("public_show") && j["public_show"].is_boolean())
                    cfg.set<bool>("dice/heart_public_show", j["public_show"].get<bool>());
                auto trimmed = [](std::string value) {
                    const auto first = value.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) return std::string();
                    const auto last = value.find_last_not_of(" \t\r\n");
                    return value.substr(first, last - first + 1);
                };
                if (j.contains("master_qq")) {
                    if (!j["master_qq"].is_string()) throw std::runtime_error("骰主 QQ 必须是文本");
                    std::string qq = trimmed(j["master_qq"].get<std::string>());
                    if (qq.size() > 20 || (!qq.empty() && !std::all_of(qq.begin(), qq.end(), [](char ch) { return ch >= '0' && ch <= '9'; })))
                        throw std::runtime_error("骰主 QQ 只能包含数字且不能超过 20 位");
                    cfg.set<std::string>("dice/heart_master_qq", qq);
                }
                if (j.contains("master_nickname")) {
                    if (!j["master_nickname"].is_string()) throw std::runtime_error("骰主昵称必须是文本");
                    std::string nickname = trimmed(j["master_nickname"].get<std::string>());
                    if (nickname.size() > 128) throw std::runtime_error("骰主昵称不能超过 128 字节");
                    cfg.set<std::string>("dice/heart_master_nickname", nickname);
                }
                if (j.contains("interval") && j["interval"].is_number()) {
                    int v = j["interval"].get<int>();
                    if (v < 180) v = 180;
                    if (v > 480) v = 480;   // 服务端 600s 判离线，留巡检余量避免在线状态抖动
                    cfg.set<int>("dice/heart_interval", v);
                }
                cfg.save();
            }
            J st = hs.lastState();
            J master = hs.masterIdentityState();
            jsonReply(ok(J{
                {"enabled", cfg.get<bool>("dice/heart_enabled", false)},
                {"url", hs.url()},
                {"configured_adapters", hs.configuredAdapterCount()},
                {"public_show", cfg.get<bool>("dice/heart_public_show", true)},
                {"master_qq", cfg.get<std::string>("dice/heart_master_qq", std::string())},
                {"master_nickname", cfg.get<std::string>("dice/heart_master_nickname", std::string())},
                {"effective_master_qq", master.value("master_id", std::string())},
                {"effective_master_nickname", master.value("master_nickname", std::string())},
                {"master_source", master.value("source", std::string("none"))},
                {"interval", hs.interval()},
                {"last_status", st.value("last_status", "unknown")},
                {"last_report_at", st.value("last_report_at", "")},
                {"last_error", st.value("last_error", "")},
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 立即按当前状态发一次心跳（分离线程做完再回调，模式同日志站上传）。
    app.registerHandler("/api/system/heartbeat/test", [](Req, CB&& cb) {
        std::thread([cb = std::move(cb)]() mutable {
            std::pair<int, std::string> r{0, ""};
            try { r = dice::heart::HeartService::instance().testReport(); } catch (...) {}
            drogon::app().getLoop()->queueInLoop([cb = std::move(cb), r]() mutable {
                if (r.first == 200) jsonReply(ok(J{{"http", r.first}, {"body", r.second}}), std::move(cb));
                else jsonReply(fail(r.first == 0
                    ? (r.second.empty() ? std::string("\xe7\xbd\x91\xe7\xbb\x9c\xe8\xaf\xb7\xe6\xb1\x82\xe5\xa4\xb1\xe8\xb4\xa5") : r.second)
                    : ("HTTP " + std::to_string(r.first) + ": " + r.second)), std::move(cb));
            });
        }).detach();
    }, {drogon::Post});

    // ── 云黑名单（cloudban.dice.zone）：配置 + 最近同步状态 ────
    app.registerHandler("/api/system/cloudban", [&cfg](Req req, CB&& cb) {
        try {
            auto& cs = dice::cloudban::CloudbanService::instance();
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("enabled") && j["enabled"].is_boolean())
                    cfg.set<bool>("dice/cloudban_enabled", j["enabled"].get<bool>());
                // 换服务器或调低危险门槛都会让旧的增量游标失效（新站/低危记录 updated_at 早于游标而漏拉）
                // → 这两种情况清空游标，下轮触发一次全量重拉。
                bool resetCursor = false;
                if (j.contains("url")) {
                    std::string u = j["url"].is_string() ? j["url"].get<std::string>() : std::string();
                    if (u.empty()) u = dice::cloudban::kOfficialCloudbanUrl;   // 空串=恢复官方默认
                    if (u != cfg.get<std::string>("dice/cloudban_url", std::string(dice::cloudban::kOfficialCloudbanUrl)))
                        resetCursor = true;
                    cfg.set<std::string>("dice/cloudban_url", u);
                }
                if (j.contains("token") && j["token"].is_string()) {
                    std::string t = j["token"].get<std::string>();
                    if (!t.empty()) cfg.set<std::string>("dice/cloudban_token", t);   // 空=不改（回落 heart_token）
                }
                if (j.contains("share") && j["share"].is_boolean())
                    cfg.set<bool>("dice/cloudban_share", j["share"].get<bool>());
                if (j.contains("min_danger") && j["min_danger"].is_number()) {
                    int v = j["min_danger"].get<int>();
                    if (v < 1) v = 1;
                    if (v > 3) v = 3;
                    if (v < cfg.get<int>("dice/cloudban_min_danger", 2)) resetCursor = true;   // 降门槛需重拉
                    cfg.set<int>("dice/cloudban_min_danger", v);
                }
                if (j.contains("sync_interval") && j["sync_interval"].is_number()) {
                    int v = j["sync_interval"].get<int>();
                    if (v < 600) v = 600;
                    cfg.set<int>("dice/cloudban_sync_interval", v);
                }
                if (resetCursor) cfg.set<std::string>("dice/cloudban_cursor", std::string());
                cfg.save();
            }
            std::string tok = cs.token();   // 实际生效 token（可能回落 heart_token），尾号据此取
            J st = cs.lastState();
            jsonReply(ok(J{
                {"enabled", cfg.get<bool>("dice/cloudban_enabled", false)},
                {"url", cs.url()},
                {"token_set", !tok.empty()},
                {"token_tail", tok.size() > 4 ? tok.substr(tok.size() - 4) : std::string()},
                {"share", cfg.get<bool>("dice/cloudban_share", true)},
                {"min_danger", cs.minDanger()},
                {"sync_interval", cs.syncInterval()},
                {"cursor", cfg.get<std::string>("dice/cloudban_cursor", std::string())},
                {"last_sync_at", st.value("last_sync_at", "")},
                {"last_sync_added", st.value("last_sync_added", 0)},
                {"last_sync_removed", st.value("last_sync_removed", 0)},
                {"last_error", st.value("last_error", "")},
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 手动触发一轮云黑同步（分离线程做完再回调 {added, removed}）。
    app.registerHandler("/api/system/cloudban/sync", [](Req, CB&& cb) {
        std::thread([cb = std::move(cb)]() mutable {
            dice::cloudban::SyncResult r;
            try { r = dice::cloudban::CloudbanService::instance().syncNow(true); } catch (...) {}
            drogon::app().getLoop()->queueInLoop([cb = std::move(cb), r]() mutable {
                if (r.ok) jsonReply(ok(J{{"added", r.added}, {"removed", r.removed}}), std::move(cb));
                else jsonReply(fail(r.error.empty() ? std::string("\xe5\x90\x8c\xe6\xad\xa5\xe5\xa4\xb1\xe8\xb4\xa5") : r.error), std::move(cb));
            });
        }).detach();
    }, {drogon::Post});

    // ── 好友/加群邀请 审批策略 ─────────────────────────────────
    app.registerHandler("/api/system/events", [&cfg, st](Req req, CB&& cb) {
        try {
            static const std::set<std::string> kFriend = {"manual", "all", "keyword", "group_used", "reject"};
            static const std::set<std::string> kGroup  = {"manual", "all", "whitelist", "ignore", "reject"};
            static const std::set<std::string> kScopedKeys = {
                "friend_policy", "friend_keyword", "group_invite_policy",
                "group_invite_reject_blacklist", "group_invite_reject_nonfriend",
                "group_name_keyword_leave", "poke", "poke_command", "poke_enabled"
            };
            auto platformForAccount = [&](const std::string& id) -> std::string {
                int aid = 0;
                try { aid = std::stoi(id); } catch (...) { return {}; }
                auto row = st->get_pointer<AdapterRow>(aid);
                if (!row) return {};
                if (row->type == static_cast<int>(AdapterType::kQQOfficial)) return "qq_official";
                if (row->type == static_cast<int>(AdapterType::kDiscord)) return "discord";
                if (row->type == static_cast<int>(AdapterType::kKook)) return "kook";
                return "onebot_v11";
            };
            auto scopeInfo = [&](const J& input) {
                std::string scope = input.value("scope", std::string("global"));
                std::string target = input.value("target", std::string());
                std::string platform = input.value("platform", std::string());
                if (scope != "adapter" && scope != "account") scope = "global";
                if (scope == "adapter") platform = target;
                if (scope == "account") {
                    const std::string actual = platformForAccount(target);
                    if (!actual.empty()) platform = actual;
                }
                return std::tuple<std::string, std::string, std::string>{scope, target, platform};
            };
            auto responseFor = [&](const std::string& scope, const std::string& target,
                                   const std::string& platform) {
                const J all = cfg.getAll();
                J ev;
                if (scope == "adapter")
                    ev = scoped_settings::resolveSection(all, "events", platform, "");
                else if (scope == "account")
                    ev = scoped_settings::resolveSection(all, "events", platform, target);
                else
                    ev = all.value("events", J::object());
                if (!ev.is_object()) ev = J::object();

                std::string fp = ev.value("friend_policy", std::string());
                if (fp.empty()) fp = ev.value("auto_approve_friend", false)
                    ? (ev.value("friend_keyword", std::string()).empty() ? "all" : "keyword") : "manual";
                std::string gp = ev.value("group_invite_policy", std::string());
                if (gp.empty()) gp = ev.value("auto_approve_group", false) ? "all" : "manual";

                J sources = J::object();
                for (const auto& key : kScopedKeys) {
                    if (scope == "global") sources[key] = "global";
                    else sources[key] = scoped_settings::sourceFor(
                        all, "events", key, platform, scope == "account" ? target : "");
                }
                return J{
                    {"friend_policy", fp},
                    {"friend_keyword", ev.value("friend_keyword", std::string())},
                    {"group_invite_policy", gp},
                    {"group_invite_reject_blacklist", ev.value("group_invite_reject_blacklist", true)},
                    {"group_invite_reject_nonfriend", ev.value("group_invite_reject_nonfriend", false)},
                    {"group_name_keyword_leave", ev.value("group_name_keyword_leave", std::string())},
                    {"poke", ev.value("poke", std::string())},
                    {"poke_command", ev.value("poke_command", std::string())},
                    {"poke_enabled", ev.value("poke_enabled", true)},
                    {"welcome_min_delay", all.value("events", J::object()).value("welcome_min_delay", 0)},
                    {"welcome_min_cooldown", all.value("events", J::object()).value("welcome_min_cooldown", 0)},
                    {"scope", scope}, {"target", target}, {"platform", platform},
                    {"overrides", scoped_settings::rawSection(all, scope, target, "events")},
                    {"sources", sources}
                };
            };

            J selector = J::object();
            if (req->method() == drogon::Get) {
                selector["scope"] = req->getParameter("scope");
                selector["target"] = req->getParameter("target");
                selector["platform"] = req->getParameter("platform");
                auto [scope, target, platform] = scopeInfo(selector);
                if (scope != "global" && target.empty()) {
                    jsonReply(fail("请选择适配器或账号"), std::move(cb)); return;
                }
                if (scope == "account" && platform.empty()) {
                    jsonReply(fail("账号不存在"), std::move(cb)); return;
                }
                jsonReply(ok(responseFor(scope, target, platform)), std::move(cb));
            } else {
                auto j = J::parse(req->body());
                auto [scope, target, platform] = scopeInfo(j);
                if (scope != "global" && target.empty()) {
                    jsonReply(fail("请选择适配器或账号"), std::move(cb)); return;
                }
                if (scope == "account" && platform.empty()) {
                    jsonReply(fail("账号不存在"), std::move(cb)); return;
                }
                J values = j.contains("values") && j["values"].is_object() ? j["values"] : j;
                J clear = j.value("clear", J::array());

                if (values.contains("friend_policy")) {
                    std::string v = values["friend_policy"].get<std::string>();
                    if (!kFriend.count(v)) { jsonReply(fail("无效的好友策略"), std::move(cb)); return; }
                }
                if (values.contains("group_invite_policy")) {
                    std::string v = values["group_invite_policy"].get<std::string>();
                    if (!kGroup.count(v)) { jsonReply(fail("无效的群邀请策略"), std::move(cb)); return; }
                }

                if (scope == "global") {
                    for (const auto& key : kScopedKeys) {
                        if (!values.contains(key)) continue;
                        if (values[key].is_boolean()) cfg.set<bool>("events/" + key, values[key].get<bool>());
                        else if (values[key].is_string()) cfg.set<std::string>("events/" + key, values[key].get<std::string>());
                    }
                    if (values.contains("welcome_min_delay") && values["welcome_min_delay"].is_number()) {
                        int newMin = values["welcome_min_delay"].get<int>();
                        int oldMin = cfg.get<int>("events/welcome_min_delay", 0);
                        cfg.set<int>("events/welcome_min_delay", newMin);
                        if (newMin > oldMin) {
                            for (auto& row : st->get_all<GroupSettingRow>(
                                orm::where(orm::c(&GroupSettingRow::key) == std::string("welcome_delay")))) {
                                int cur = 0; try { cur = std::stoi(row.value); } catch (...) {}
                                if (cur < newMin) gsSet(st, row.platform, row.groupId, "welcome_delay", std::to_string(newMin));
                            }
                        }
                    }
                    if (values.contains("welcome_min_cooldown") && values["welcome_min_cooldown"].is_number()) {
                        int newMin = values["welcome_min_cooldown"].get<int>();
                        int oldMin = cfg.get<int>("events/welcome_min_cooldown", 0);
                        cfg.set<int>("events/welcome_min_cooldown", newMin);
                        if (newMin > oldMin) {
                            for (auto& row : st->get_all<GroupSettingRow>(
                                orm::where(orm::c(&GroupSettingRow::key) == std::string("welcome_cooldown")))) {
                                int cur = 0; try { cur = std::stoi(row.value); } catch (...) {}
                                if (cur < newMin) gsSet(st, row.platform, row.groupId, "welcome_cooldown", std::to_string(newMin));
                            }
                        }
                    }
                } else {
                    J filtered = J::object();
                    for (const auto& key : kScopedKeys)
                        if (values.contains(key)) filtered[key] = values[key];
                    J filteredClear = J::array();
                    if (clear.is_array()) {
                        for (const auto& key : clear)
                            if (key.is_string() && kScopedKeys.count(key.get<std::string>()))
                                filteredClear.push_back(key);
                    }
                    scoped_settings::setSection(cfg, scope, target, "events", filtered, filteredClear);
                }
                cfg.save();
                jsonReply(ok(responseFor(scope, target, platform)), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── 规则包管理（P2：导入/查看/删除，热重载）──────────────────
    auto rulePackToJson = [](const CommandRouter::RulePack& p) {
        J keys = J::array(); for (auto& k : p.setKeys) keys.push_back(k);
        // 卡片：自定义指令 / 别名 / 屏蔽 三类指令，供前端像插件一样展示。
        J custom = J::array(); for (auto& [k, v] : p.customCmds) custom.push_back(k);
        J alias = J::array(); for (auto& [k, v] : p.cmdAlias) alias.push_back(J{{"from", k}, {"to", v}});
        J disable = J::array(); for (auto& d : p.disableCmds) disable.push_back(d);
        return J{{"name", p.name}, {"fullName", p.fullName}, {"version", p.version},
                 {"author", p.author}, {"file", p.file}, {"diceSides", p.diceSides}, {"setKeys", keys},
                 {"aliasGroups", p.aliasGroups}, {"computedCount", p.computedCount},
                 {"manualCount", p.manualCount}, {"enabled", p.enabled},
                 {"customCmds", custom}, {"cmdAlias", alias}, {"disableCmds", disable},
                 {"helpCount", (int)p.helpEntries.size()},
                 {"ownerBundle", p.ownerBundle}, {"ownerBundleFolder", p.ownerBundleFolder},
                 {"builtin", CommandRouter::isBuiltinRulePack(p.file)}};   // 内置(coc7/dnd)不可删
    };
    app.registerHandler("/api/rules", [rulePackToJson](Req, CB&& cb) {
        J arr = J::array();
        {
            std::shared_lock<std::shared_mutex> lk(rulesLock());
            for (auto& p : CommandRouter::rulePacks()) arr.push_back(rulePackToJson(p));
        }
        jsonReply(ok(J{{"packs", arr}}), std::move(cb));
    }, {drogon::Get});

    // ── 帮助系统：聚合 内置 / 规则包 / 插件 三源帮助条目 ──────────
    app.registerHandler("/api/help", [&i18n, &jsMod, &luaMod](Req req, CB&& cb) {
        std::string lang = req->getParameter("lang");
        if (lang.empty()) lang = "zh-Hans";
        Locale loc = localeFromString(lang);
        // 服务端分页 + 搜索（Lua helpdoc 可达 2 万+ 条，全量回传会几十 MB/数秒）。
        // 仅小写 ASCII A-Z（CJK 等高位字节保持精确，避免 locale 相关的 std::tolower 误伤多字节）。
        auto aLow = [](unsigned char c) -> char { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; };
        std::string q = req->getParameter("q");
        for (auto& ch : q) ch = aLow((unsigned char)ch);
        std::string srcFilter = req->getParameter("source");   // 分组视图展开时只取某来源
        int page = 1, size = 50;
        try { if (!req->getParameter("page").empty()) { int v = std::stoi(req->getParameter("page")); page = v < 1 ? 1 : v; } } catch (...) {}
        try { if (!req->getParameter("size").empty()) { int v = std::stoi(req->getParameter("size")); size = v < 1 ? 1 : (v > 200 ? 200 : v); } } catch (...) {}

        struct E { std::string key, content, source, i18nKey; bool editable; };
        std::vector<E> all;
        std::unordered_set<std::string> seenKeys;
        auto add = [&](const std::string& k, const std::string& c, const std::string& src,
                       bool ed, const std::string& i18nKey) {
            if (k.empty() || c.empty() || !seenKeys.insert(k).second) return;
            all.push_back({k, c, src, i18nKey, ed});
        };
        for (const auto& t : CommandRouter::helpTopics())
            add(t, i18n.tr(loc, "help.topic." + t), "builtin", true, "help.topic." + t);
        { std::shared_lock<std::shared_mutex> lk(rulesLock());
          for (auto& p : CommandRouter::rulePacks()) { if (!p.enabled) continue;
            for (auto& [k, c] : p.helpEntries) add(k, c, "rule:" + p.name, false, ""); } }
        for (auto& ch : jsMod.commandHelps())
            add(ch.name, ch.help, "plugin:" + ch.plugin, false, "");
        for (auto& h : luaMod.helpEntries())   // 补 Lua mod descriptor.helpdoc
            add(h.topic, h.text, "lua:" + h.mod, false, "");
        { std::shared_lock<std::shared_mutex> lk(CommandRouter::helpLock());
          for (auto& [k, c] : CommandRouter::helpFiles()) add(k, c, "file:" + k, true, ""); }
        { std::shared_lock<std::shared_mutex> lk(CommandRouter::helpDocLock());   // 结构化帮助文档（海豹兼容+随包速查）
          for (auto& h : CommandRouter::helpDocs()) add(h.topic, h.content, "helpdoc:" + h.pack, false, ""); }

        // 大小写无关「包含」（不分配小写副本）。q 已是小写。
        auto ciContains = [&aLow](const std::string& hay, const std::string& nl) {
            if (nl.empty()) return true;
            auto it = std::search(hay.begin(), hay.end(), nl.begin(), nl.end(),
                [&aLow](char a, char b){ return aLow((unsigned char)a) == b; });
            return it != hay.end();
        };
        std::vector<const E*> filtered;
        for (auto& e : all)
            if ((srcFilter.empty() || e.source == srcFilter)
                && (q.empty() || ciContains(e.key, q) || ciContains(e.content, q))) filtered.push_back(&e);

        int total = (int)filtered.size();
        int start = (page - 1) * size;
        J arr = J::array();
        for (int i = start; i < start + size && i < total; ++i) {
            const E& e = *filtered[(size_t)i];
            J o{{"key", e.key}, {"content", e.content}, {"source", e.source}, {"editable", e.editable}};
            if (!e.i18nKey.empty()) o["i18nKey"] = e.i18nKey;
            arr.push_back(std::move(o));
        }
        jsonReply(ok(J{{"entries", arr}, {"total", total}, {"page", page}, {"size", size}}), std::move(cb));
    }, {drogon::Get});

    // 帮助文档「按来源分组」视图：每个规则/插件/文件一组 + 条目数（前端展开看全部条目）。
    app.registerHandler("/api/help/groups", [&i18n, &jsMod, &luaMod](Req req, CB&& cb) {
        std::string lang = req->getParameter("lang"); if (lang.empty()) lang = "zh-Hans";
        Locale loc = localeFromString(lang);
        auto aLow = [](unsigned char c) -> char { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c; };
        std::string q = req->getParameter("q"); for (auto& ch : q) ch = aLow((unsigned char)ch);
        struct E { std::string key, content, source; };
        std::vector<E> all;
        std::unordered_set<std::string> seenKeys;
        auto add = [&](const std::string& k, const std::string& c, const std::string& src) {
            if (k.empty() || c.empty() || !seenKeys.insert(k).second) return;
            all.push_back({k, c, src});
        };
        for (const auto& t : CommandRouter::helpTopics()) add(t, i18n.tr(loc, "help.topic." + t), "builtin");
        { std::shared_lock<std::shared_mutex> lk(rulesLock());
          for (auto& p : CommandRouter::rulePacks()) { if (!p.enabled) continue;
            for (auto& [k, c] : p.helpEntries) add(k, c, "rule:" + p.name); } }
        for (auto& ch : jsMod.commandHelps()) add(ch.name, ch.help, "plugin:" + ch.plugin);
        for (auto& h : luaMod.helpEntries()) add(h.topic, h.text, "lua:" + h.mod);
        { std::shared_lock<std::shared_mutex> lk(CommandRouter::helpLock());
          for (auto& [k, c] : CommandRouter::helpFiles()) add(k, c, "file:" + k); }
        { std::shared_lock<std::shared_mutex> lk(CommandRouter::helpDocLock());
          for (auto& h : CommandRouter::helpDocs()) add(h.topic, h.content, "helpdoc:" + h.pack); }
        auto ciContains = [&aLow](const std::string& hay, const std::string& nl) {
            if (nl.empty()) return true;
            auto it = std::search(hay.begin(), hay.end(), nl.begin(), nl.end(),
                [&aLow](char a, char b){ return aLow((unsigned char)a) == b; });
            return it != hay.end();
        };
        std::vector<std::pair<std::string, int>> groups;   // 保持首见顺序（builtin→rule→plugin→lua→file→helpdoc）
        std::unordered_map<std::string, size_t> idx;
        int total = 0;
        for (auto& e : all) {
            if (!q.empty() && !ciContains(e.key, q) && !ciContains(e.content, q)) continue;
            ++total;
            auto it = idx.find(e.source);
            if (it == idx.end()) { idx[e.source] = groups.size(); groups.push_back({e.source, 1}); }
            else ++groups[it->second].second;
        }
        J arr = J::array();
        for (auto& [src, cnt] : groups) arr.push_back(J{{"source", src}, {"count", cnt}});
        jsonReply(ok(J{{"groups", arr}, {"total", total}, {"groupCount", (int)groups.size()}}), std::move(cb));
    }, {drogon::Get});

    // 帮助文档文件管理（data/help/*.md，第四源；与插件无关的通用文档）。
    app.registerHandler("/api/help/files", [](Req, CB&& cb) {
        J arr = J::array(); namespace fs = std::filesystem; std::error_code ec;
        if (fs::is_directory("data/help", ec))
            for (auto& e : fs::directory_iterator("data/help", ec)) {
                if (ec || !e.is_regular_file()) continue;
                std::string fn = dnx_u8str(e.path().filename());
                if (fn.size() <= 3 || fn.substr(fn.size() - 3) != ".md") continue;
                arr.push_back(J{{"name", dnx_u8str(e.path().stem())},
                               {"size", (long)fs::file_size(e.path(), ec)}});
            }
        jsonReply(ok(J{{"files", arr}}), std::move(cb));
    }, {drogon::Get});
    app.registerHandler("/api/help/file", [](Req req, CB&& cb) {
        std::string name = baseName(req->getParameter("name"));
        if (name.empty()) { jsonReply(fail("name required"), std::move(cb)); return; }
        std::filesystem::path p = std::filesystem::path("data/help") / (name + ".md");
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) { jsonReply(fail("not found"), std::move(cb)); return; }
        std::ifstream f(p, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        jsonReply(ok(J{{"name", name}, {"content", body}}), std::move(cb));
    }, {drogon::Get});
    app.registerHandler("/api/help/file", [](Req req, CB&& cb) {
        try {
            auto j = json::parse(req->getBody());
            std::string name = baseName(j.value("name", ""));
            if (name.empty()) { jsonReply(fail("name required"), std::move(cb)); return; }
            std::filesystem::create_directories("data/help");
            std::ofstream f(std::filesystem::path("data/help") / (name + ".md"), std::ios::binary);
            f << j.value("content", ""); f.close();
            CommandRouter::loadHelpFiles();   // 热生效
            jsonReply(ok(J{{"name", name}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});
    app.registerHandler("/api/help/file/{1}", [](Req, CB&& cb, std::string name) {
        name = baseName(name);
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path("data/help") / (name + ".md"), ec);
        CommandRouter::loadHelpFiles();
        jsonReply(ok(J{{"name", name}}), std::move(cb));
    }, {drogon::Delete});

    // 查看规则包文件原文（data/rules/ 优先，再 rules/）。
    app.registerHandler("/api/rules/file", [](Req req, CB&& cb) {
        std::string file = baseName(req->getParameter("file"));
        if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
        namespace fs = std::filesystem;
        for (const char* d : {"data/rules", "rules"}) {
            fs::path p = fs::path(d) / file; std::error_code ec;
            if (fs::exists(p, ec)) {
                std::ifstream f(p, std::ios::binary);
                std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                jsonReply(ok(J{{"file", file}, {"content", body}}), std::move(cb)); return;
            }
        }
        jsonReply(fail("not found"), std::move(cb));
    }, {drogon::Get});

    // 导入规则包：校验 JSON → 写 data/rules/ → 热重载。POST {filename, content}
    app.registerHandler("/api/rules/upload", [](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string fname = baseName(j.value("filename", std::string()));
            std::string content = j.value("content", std::string());
            if (fname.empty() || content.empty()) { jsonReply(fail("filename/content required"), std::move(cb)); return; }
            if (fname.size() < 5 || fname.substr(fname.size() - 5) != ".json") fname += ".json";
            try { auto pj = J::parse(content); if (!pj.is_object()) { jsonReply(fail("规则包应为 JSON 对象"), std::move(cb)); return; } }
            catch (...) { jsonReply(fail("内容不是合法 JSON"), std::move(cb)); return; }
            namespace fs = std::filesystem; std::error_code ec;
            fs::create_directories("data/rules", ec);
            { std::ofstream f(fs::path("data/rules") / fname, std::ios::binary | std::ios::trunc); f << content; }
            int n = CommandRouter::reloadRulePacks({"rules", "data/rules"});
            jsonReply(ok(J{{"loaded", n}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 删除导入的规则包（仅 data/rules/，内置不可删）→ 热重载。POST {file}
    app.registerHandler("/api/rules/delete", [](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            if (CommandRouter::isBuiltinRulePack(file)) { jsonReply(fail("内置规则包不可删除"), std::move(cb)); return; }
            namespace fs = std::filesystem; std::error_code ec;
            fs::path p = fs::path("data/rules") / file;
            if (!fs::exists(p, ec)) { p = fs::path("rules") / file; }   // 兼容旧根目录位置
            if (!fs::exists(p, ec)) { jsonReply(fail("未找到该规则包"), std::move(cb)); return; }
            fs::remove(p, ec);
            int n = CommandRouter::reloadRulePacks({"rules", "data/rules"});
            jsonReply(ok(J{{"loaded", n}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 启用/停用规则包：重命名 <base>.json ↔ <base>.json.disabled → 热重载。POST {file, enabled}
    app.registerHandler("/api/rules/toggle", [](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            bool enable = j.value("enabled", true);
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            std::string base = CommandRouter::ruleBaseFile(file);   // 去 .disabled
            namespace fs = std::filesystem; std::error_code ec;
            for (const char* d : {"data/rules", "rules"}) {
                fs::path on = fs::path(d) / base, off = fs::path(d) / (base + ".disabled");
                fs::path from = enable ? off : on, to = enable ? on : off;
                if (fs::exists(from, ec)) { fs::rename(from, to, ec); break; }
                if (fs::exists(to, ec)) break;   // 已是目标状态
            }
            int n = CommandRouter::reloadRulePacks({"rules", "data/rules"});
            jsonReply(ok(J{{"loaded", n}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 在线编辑：校验 JSON → 写回该规则包文件 → 热重载。PUT {file, content}
    app.registerHandler("/api/rules/save", [](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string file = baseName(j.value("file", std::string()));
            std::string content = j.value("content", std::string());
            if (file.empty()) { jsonReply(fail("file required"), std::move(cb)); return; }
            try { auto pj = J::parse(content); if (!pj.is_object()) { jsonReply(fail("规则包应为 JSON 对象"), std::move(cb)); return; } }
            catch (...) { jsonReply(fail("内容不是合法 JSON"), std::move(cb)); return; }
            namespace fs = std::filesystem; std::error_code ec;
            fs::path p = fs::path("data/rules") / file;
            if (!fs::exists(p, ec)) { fs::path r = fs::path("rules") / file; if (fs::exists(r, ec)) p = r; }
            fs::create_directories(p.parent_path(), ec);
            { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << content; }
            int n = CommandRouter::reloadRulePacks({"rules", "data/rules"});
            jsonReply(ok(J{{"loaded", n}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put});

    // ── 黑白名单管理（用户/群 × 黑/白）────────────────────────────
    app.registerHandler("/api/banlist", [st, &cfg, &adapterMgr](Req req, CB&& cb) {
        namespace orm = sqlite_orm;
        try {
            if (req->method() == drogon::Get) {
                J arr = J::array();
                if (st) for (auto& r : st->get_all<BanlistRow>())
                    arr.push_back(J{{"id", r.id}, {"targetType", r.targetType}, {"listType", r.listType},
                                    {"targetId", r.targetId}, {"reason", r.reason}, {"createdAt", r.createdAt}});
                jsonReply(ok(J{{"entries", arr}}), std::move(cb));
            } else {   // POST 新增（去重）
                auto j = J::parse(req->body());
                int type = j.value("targetType", 0), list = j.value("listType", 0);
                std::string id = j.value("targetId", std::string()), reason = j.value("reason", std::string());
                if (id.empty()) { jsonReply(fail("targetId required"), std::move(cb)); return; }
                // listType: 0=黑名单 1=白名单 2=骰娘名单
                if (type < 0 || type > 1 || list < 0 || list > 2) { jsonReply(fail("invalid type/list"), std::move(cb)); return; }
                if (!st) { jsonReply(fail("db not open"), std::move(cb)); return; }
                bool exists = st->count<BanlistRow>(orm::where(
                    orm::c(&BanlistRow::targetType) == type and orm::c(&BanlistRow::listType) == list and
                    orm::c(&BanlistRow::targetId) == id)) > 0;
                if (!exists) {
                    BanlistRow r; r.targetType = type; r.listType = list; r.targetId = id;
                    r.reason = reason; r.createdAt = utils::nowIso8601(); st->insert(r);
                    // 新增黑名单条目后通知骰主。
                    if (list == 0) {
                        dice::notice::notify(cfg, adapterMgr, dice::notice::kCritical,
                            std::string("\xe5\xb7\xb2\xe6\x8b\x89\xe9\xbb\x91") + (type == 0 ? "\xe7\x94\xa8\xe6\x88\xb7 " : "\xe7\xbe\xa4 ") + id
                                + (reason.empty() ? std::string() : ("\xef\xbc\x88" + reason + "\xef\xbc\x89")), "", "", "blacklist");
                        // WebUI 主动拉黑 → 云黑上报（分享开启时；"[云黑#..]" 前缀条目内部自动跳过防回声）。
                        dice::cloudban::CloudbanService::instance().reportToCloud(
                            type == 0 ? "user" : "group", id, "other", reason);
                    }
                }
                jsonReply(ok(J{{"saved", true}}), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    app.registerHandler("/api/banlist/{1}", [st](Req, CB&& cb, int rowId) {
        namespace orm = sqlite_orm;
        try { if (st) st->remove_all<BanlistRow>(orm::where(orm::c(&BanlistRow::id) == rowId));
              jsonReply(ok(J{{"removed", true}}), std::move(cb)); }
        catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // 仅白名单模式开关（dice.whitelist_only）。
    app.registerHandler("/api/banlist/whitelist-only", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                cfg.set<bool>("dice/whitelist_only", j.value("enabled", false));
                cfg.save();
            }
            bool on = cfg.getAll().value("dice", J::object()).value("whitelist_only", false);
            jsonReply(ok(J{{"enabled", on}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ── 定时任务 CRUD (#48) ────────────────────────────────────
    auto schedToJson = [](const ScheduledTaskRow& r) {
        return J{{"id", r.id}, {"name", r.name}, {"adapterId", r.adapterId}, {"platform", r.platform},
                 {"targetType", r.targetType}, {"targetId", r.targetId}, {"cronTime", r.cronTime},
                 {"days", r.days}, {"content", r.content}, {"enabled", r.enabled != 0},
                 {"lastRun", r.lastRun},
                 {"action", r.action.empty() ? "send" : r.action}, {"condition", r.condition},
                 {"triggerType", r.triggerType.empty() ? "daily" : r.triggerType},
                 {"intervalMin", r.intervalMin}, {"onceDate", r.onceDate}};
    };
    // 保存前校验（拼错的条件/畸形时刻以前会静默存库，跑起来才出怪行为）。
    // 返回空串=通过，否则为错误消息。校验规则与 CommandRouter::evalScheduledCondition 对齐。
    static auto schedValidate = [](const ScheduledTaskRow& r) -> std::string {
        auto validHM = [](const std::string& s) {
            if (s.size() != 5 || s[2] != ':') return false;
            for (int i : {0, 1, 3, 4}) if (s[i] < '0' || s[i] > '9') return false;
            int h = (s[0]-'0')*10 + (s[1]-'0'), m = (s[3]-'0')*10 + (s[4]-'0');
            return h < 24 && m < 60;
        };
        auto validYMD = [](const std::string& s) {
            if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
            for (int i : {0,1,2,3,5,6,8,9}) if (s[i] < '0' || s[i] > '9') return false;
            int mo = (s[5]-'0')*10 + (s[6]-'0'), dd = (s[8]-'0')*10 + (s[9]-'0');
            return mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31;
        };
        const std::string ttype = r.triggerType.empty() ? "daily" : r.triggerType;
        if (ttype != "daily" && ttype != "interval" && ttype != "once")
            return "triggerType must be daily|interval|once";
        if (r.targetId.empty()) return "targetId required";
        if (ttype == "interval") {
            if (r.intervalMin < 1 || r.intervalMin > 10080) return "intervalMin must be 1-10080";
        } else {
            if (!validHM(r.cronTime)) return "cronTime must be HH:MM";
        }
        if (ttype == "once" && !validYMD(r.onceDate)) return "onceDate must be YYYY-MM-DD";
        if (r.targetType != "group" && r.targetType != "private") return "targetType must be group|private";
        if (r.targetId == "*" && r.targetType != "group") return "targetId=* (all groups) requires targetType=group";
        if (!r.action.empty() && r.action != "send" && r.action != "leave") return "action must be send|leave";
        if (r.action == "leave" && r.targetType != "group") return "action=leave requires a group target";
        if (r.action != "leave" && r.content.empty()) return "content required";
        if (!CommandRouter::isValidScheduledCondition(r.condition))
            return "condition must be empty or inactive>=N (ops: >= > <= < ==)";
        std::stringstream ss(r.days); std::string d;
        while (std::getline(ss, d, ',')) {
            if (d.empty()) continue;
            if (d.size() != 1 || d[0] < '0' || d[0] > '6') return "days must be comma-separated 0-6";
        }
        return {};
    };
    // 当前配置时区 "HH:MM" / "YYYY-MM-DD"（lastRun 播种用）。
    static auto schedNow = [](std::string& hm, std::string& ymd) {
        const std::time_t now = std::time(nullptr);
        hm = utils::formatTimeInTimezone(now, "%H:%M");
        ymd = utils::formatTimeInTimezone(now, "%Y-%m-%d");
    };
    app.registerHandler("/api/schedules", [st, schedToJson](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Get) {
                J arr = J::array();
                for (auto& r : st->get_all<ScheduledTaskRow>()) arr.push_back(schedToJson(r));
                jsonReply(ok(arr), std::move(cb));
            } else {
                auto j = J::parse(req->body());
                ScheduledTaskRow r;
                r.name = j.value("name", std::string("任务"));
                r.adapterId = j.value("adapterId", std::string());
                r.platform = j.value("platform", std::string("onebot_v11"));
                r.targetType = j.value("targetType", std::string("group"));
                r.targetId = j.value("targetId", std::string());
                r.cronTime = j.value("cronTime", std::string("09:00"));
                r.days = j.value("days", std::string());
                r.content = j.value("content", std::string());
                r.enabled = j.value("enabled", true) ? 1 : 0;
                r.action = j.value("action", std::string("send"));
                r.condition = j.value("condition", std::string());
                r.triggerType = j.value("triggerType", std::string("daily"));
                if (r.triggerType == "daily") r.triggerType = "";   // 默认类型存空，兼容旧行
                r.intervalMin = j.value("intervalMin", 0);
                r.onceDate = j.value("onceDate", std::string());
                r.lastRun = ""; r.createdAt = "";
                if (auto err = schedValidate(r); !err.empty()) { jsonReply(fail(err), std::move(cb)); return; }
                // lastRun 播种：daily 时刻已过 → 从明天开始（补发窗口会把「刚建的
                // 09:00 任务」在 09:30 立刻触发，吓人）；interval → 从现在起算一个
                // 间隔后首发；once → 留空等 onceDate 到点。
                std::string hm, ymd; schedNow(hm, ymd);
                if (r.enabled && r.triggerType.empty() && r.cronTime <= hm) r.lastRun = ymd;
                if (r.triggerType == "interval") r.lastRun = ymd + " " + hm;
                int id = st->insert(r);
                jsonReply(ok(J{{"id", id}}), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});
    app.registerHandler("/api/schedules/{1}", [st](Req req, CB&& cb, const std::string& sid) {
        try {
            int id = 0;
            try { id = std::stoi(sid); } catch (...) { jsonReply(fail("bad id"), std::move(cb)); return; }
            if (req->method() == drogon::Delete) {
                st->remove<ScheduledTaskRow>(id);
                jsonReply(ok(), std::move(cb));
            } else {
                auto j = J::parse(req->body());
                auto rp = st->get_pointer<ScheduledTaskRow>(id);
                if (!rp) { jsonReply(fail("task not found"), std::move(cb)); return; }
                auto r = *rp;
                const std::string oldCron = r.cronTime; const int oldEnabled = r.enabled;
                const std::string oldType = r.triggerType; const int oldInterval = r.intervalMin;
                const std::string oldOnce = r.onceDate;
                if (j.contains("name")) r.name = j["name"].get<std::string>();
                if (j.contains("adapterId")) r.adapterId = j["adapterId"].get<std::string>();
                if (j.contains("platform")) r.platform = j["platform"].get<std::string>();
                if (j.contains("targetType")) r.targetType = j["targetType"].get<std::string>();
                if (j.contains("targetId")) r.targetId = j["targetId"].get<std::string>();
                if (j.contains("cronTime")) r.cronTime = j["cronTime"].get<std::string>();
                if (j.contains("days")) r.days = j["days"].get<std::string>();
                if (j.contains("content")) r.content = j["content"].get<std::string>();
                if (j.contains("enabled")) r.enabled = j["enabled"].get<bool>() ? 1 : 0;
                if (j.contains("action")) r.action = j["action"].get<std::string>();
                if (j.contains("condition")) r.condition = j["condition"].get<std::string>();
                if (j.contains("triggerType")) {
                    r.triggerType = j["triggerType"].get<std::string>();
                    if (r.triggerType == "daily") r.triggerType = "";
                }
                if (j.contains("intervalMin")) r.intervalMin = j["intervalMin"].get<int>();
                if (j.contains("onceDate")) r.onceDate = j["onceDate"].get<std::string>();
                if (auto err = schedValidate(r); !err.empty()) { jsonReply(fail(err), std::move(cb)); return; }
                // 触发定义变了（类型/时刻/间隔/日期）或重新启用 → 重新播种 lastRun：
                //   daily    时刻已过今天 → 明天开始；没过 → 今天照常触发
                //   interval 从现在起算一个间隔
                //   once     清空重新武装（改到过去的日期会在下个 tick 标记完成）
                const bool trigChanged = r.cronTime != oldCron || r.triggerType != oldType
                    || r.intervalMin != oldInterval || r.onceDate != oldOnce;
                if (trigChanged || (r.enabled && !oldEnabled)) {
                    std::string hm, ymd; schedNow(hm, ymd);
                    if (r.triggerType == "interval")   r.lastRun = ymd + " " + hm;
                    else if (r.triggerType == "once")  r.lastRun = "";
                    else                               r.lastRun = (r.cronTime <= hm) ? ymd : std::string();
                }
                st->update(r);
                jsonReply(ok(), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Delete});

    // ── Dice Roll ─────────────────────────────────────────────
    app.registerHandler("/api/dice/roll", [&engine](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            auto expr = j.value("expression", "1d100");
            auto result = engine.roll(expr);
            if (result.ok()) {
                jsonReply(ok(J{{"expression", result.expression},{"result", result.formattedOutput},{"detail", result.detail}}), std::move(cb));
            } else {
                jsonReply(fail(result.error, result.errorCode), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // ── Replies ───────────────────────────────────────────────
    app.registerHandler("/api/replies", [st, &replyMgr](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Get) {
                auto rows = st->get_all<ReplyRuleRow>(orm::order_by(&ReplyRuleRow::priority));
                J arr = J::array();
                for (auto& r : rows) arr.push_back(replyToJson(r));
                jsonReply(ok(arr), std::move(cb));
            } else if (req->method() == drogon::Post) {
                auto j = J::parse(req->body());
                ReplyRule rule = replyRuleFromJson(j);
                if (auto err = replyRuleValidate(rule); !err.empty()) { jsonReply(fail(err), std::move(cb)); return; }
                int id = replyMgr.addRule(rule);
                if (id < 0) { jsonReply(fail("add failed"), std::move(cb)); return; }
                auto row = st->get<ReplyRuleRow>(id);
                jsonReply(ok(replyToJson(row)), std::move(cb));
            } else { jsonReply(fail("Method not allowed"), std::move(cb)); }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    // PUT/DELETE /api/replies/{id}
    app.registerHandler("/api/replies/{1}", [st, &replyMgr](Req req, CB&& cb, const std::string& id) {
        try {
            int rid = std::stoi(id);
            if (req->method() == drogon::Put || req->method() == drogon::Patch) {
                auto j = J::parse(req->body());
                auto existing = st->get<ReplyRuleRow>(rid);   // for fields not in the body
                // 以现值（含 conditions/results/触发限制）打底，body 覆盖其上再重建。
                // 以前只回填 matchContent 等五个旧标量：网页开关一下（PUT {enabled}）
                // 就把多条件/多回复规则静默塌成单条件，触发限制也被重置。
                J base = replyToJson(existing);
                for (auto& [k, v] : j.items()) base[k] = v;
                ReplyRule rule = replyRuleFromJson(base);
                if (auto err = replyRuleValidate(rule); !err.empty()) { jsonReply(fail(err), std::move(cb)); return; }
                if (!replyMgr.updateRule(rid, rule)) { jsonReply(fail("not found"), std::move(cb)); return; }
                auto row = st->get<ReplyRuleRow>(rid);
                jsonReply(ok(replyToJson(row)), std::move(cb));
            } else if (req->method() == drogon::Delete) {
                st->remove<ReplyRuleRow>(rid);
                replyMgr.loadRules();
                jsonReply(ok(nullptr), std::move(cb));
            } else { jsonReply(fail("Method not allowed"), std::move(cb)); }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Delete, drogon::Patch});

    // ── Decks ─────────────────────────────────────────────────
    app.registerHandler("/api/decks", [st, &cardDeck](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Get) {
                auto rows = st->get_all<DeckRow>();
                J arr = J::array();
                for (auto& r : rows) arr.push_back(J{{"id",r.id},{"name",r.name},{"cards",J::parse(r.cards.empty()?"[]":r.cards)}});
                // Also include file-loaded decks from CardDeck, grouped by FILE
                int nextId = (int)rows.size() + 1000;
                std::map<std::string, J> fileGroups; // filename → meta
                for (auto& name : cardDeck.deckNames()) {
                    auto fname = cardDeck.getSourceFile(name);
                    if (fname.empty()) {
                        // Built-in deck: show without file
                        J g;
                        g["filename"] = "";
                        g["title"] = name;
                        g["author"] = nullptr;
                        g["version"] = nullptr;
                        g["date"] = nullptr;
                        g["entries"] = J::array({name});
                        g["id"] = nextId++;
                        arr.push_back(g);
                        continue;
                    }
                    if (!fileGroups.count(fname)) {
                        J g;
                        g["filename"] = fname;
                        g["title"] = fname;
                        g["author"] = nullptr;
                        g["version"] = nullptr;
                        g["date"] = nullptr;
                        g["entries"] = J::array();
                        fileGroups[fname] = g;
                    }
                    fileGroups[fname]["entries"].push_back(name);
                }
                // Read each file for metadata
                for (auto& pair : fileGroups) {
                    auto& g = pair.second;
                    auto& fname = pair.first;
                    // 用户牌堆在 data/decks/，内置牌堆在 decks/——都要找（之前只找 decks/，
                    // 导致 data/decks 里的牌堆元数据 _title/_author 不显示）。
                    std::ifstream f(u8p("data/decks/" + fname));
                    if (!f) f.open(u8p("decks/" + fname));
                    if (!f) f.open(u8p("../data/decks/" + fname));
                    if (!f) f.open(u8p("../decks/" + fname));
                    if (f) {
                        try {
                            auto j = nlohmann::json::parse(f);
                            if (j.contains("_title") && j["_title"].is_array() && !j["_title"].empty())
                                g["title"] = j["_title"][0];
                            if (j.contains("_author") && j["_author"].is_array() && !j["_author"].empty())
                                g["author"] = j["_author"][0];
                            if (j.contains("_version") && j["_version"].is_array() && !j["_version"].empty())
                                g["version"] = j["_version"][0];
                            if (j.contains("_date") && j["_date"].is_array() && !j["_date"].empty())
                                g["date"] = j["_date"][0];
                            if (j.contains("_brief") && j["_brief"].is_array() && !j["_brief"].empty())
                                g["description"] = j["_brief"][0];
                        } catch (...) {}
                    }
                    // Separate entries: regular vs hidden (underscore prefix)
                    J regular = J::array(), hidden = J::array();
                    for (auto& e : g["entries"]) {
                        std::string en = e.get<std::string>();
                        if (!en.empty() && en[0] == '_') hidden.push_back(en);
                        else regular.push_back(en);
                    }
                    g["entries"] = regular;
                    g["hidden_entries"] = hidden;
                    g["id"] = nextId++;
                    arr.push_back(g);
                }
                jsonReply(ok(arr), std::move(cb));
            } else if (req->method() == drogon::Post) {
                auto j = J::parse(req->body());
                DeckRow d;
                d.name = j.value("name", "");
                d.cards = j.value("cards", J::array()).dump();
                d.createdAt = utils::nowIso8601();
                d.id = st->insert(d);
                jsonReply(ok(J{{"id",d.id},{"name",d.name}}), std::move(cb));
            } else { jsonReply(fail("Method not allowed"), std::move(cb)); }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    app.registerHandler("/api/decks/{1}", [st](Req req, CB&& cb, const std::string& id) {
        try {
            int did = std::stoi(id);
            if (req->method() == drogon::Delete) {
                st->remove<DeckRow>(did);
                jsonReply(ok(), std::move(cb));
            } else if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                auto d = st->get<DeckRow>(did);
                if (j.contains("name")) d.name = j["name"];
                if (j.contains("cards")) d.cards = j["cards"].dump();
                st->update(d);
                jsonReply(ok(), std::move(cb));
            } else { jsonReply(fail("Method not allowed"), std::move(cb)); }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Delete});

    // ── Sessions ──────────────────────────────────────────────
    app.registerHandler("/api/logs", [lst](Req req, CB&& cb) {
        try {
            std::string gid = req->getParameter("groupId");
            auto rows = lst->get_all<GameLogRow>();
            struct LogUsage {
                int count = 0;
                std::string lastAt;
                std::uint64_t databaseBytes = 0;
                std::uint64_t imageBytes = 0;
            };
            std::unordered_map<int, LogUsage> usage;

            // SQLite 无法精确拆分出单行占用页数；这里按该日志实际保存的字段字节数
            // 统计数据库内容，并与本地缓存图片的真实文件大小相加。这样既不会把
            // 整个 logs.db 的空闲页重复分摊，也能准确反映图片带来的主要磁盘占用。
            for (const auto& message : lst->get_all<GameLogMessageRow>(
                     orm::order_by(&GameLogMessageRow::id).asc())) {
                auto& item = usage[message.logId];
                ++item.count;
                item.lastAt = message.createdAt;
                item.databaseBytes += sizeof(message.id) + sizeof(message.logId)
                    + message.messageId.size() + message.sender.size() + message.userId.size()
                    + message.content.size() + message.createdAt.size() + message.images.size();
            }
            namespace fs = std::filesystem;
            std::error_code imageEc;
            for (const char* dir : {"data/logs/images", "../data/logs/images"}) {
                imageEc.clear();
                if (!fs::is_directory(dir, imageEc)) continue;
                for (const auto& entry : fs::directory_iterator(dir, imageEc)) {
                    if (!entry.is_regular_file(imageEc)) continue;
                    const std::string filename = dnx_u8str(entry.path().filename());
                    if (filename.rfind("log", 0) != 0) continue;
                    const auto underscore = filename.find('_', 3);
                    if (underscore == std::string::npos || underscore == 3) continue;
                    int logId = 0;
                    try { logId = std::stoi(filename.substr(3, underscore - 3)); }
                    catch (...) { continue; }
                    const auto bytes = entry.file_size(imageEc);
                    if (!imageEc) usage[logId].imageBytes += static_cast<std::uint64_t>(bytes);
                    imageEc.clear();
                }
                break;
            }
            J arr = J::array();
            for (auto& r : rows) {
                if (!gid.empty() && r.groupId != gid) continue;
                auto& size = usage[r.id];
                size.databaseBytes += sizeof(r.id) + sizeof(r.status) + r.groupId.size()
                    + r.gmId.size() + r.name.size() + r.players.size()
                    + r.customRules.size() + r.createdAt.size();
                std::string gameCode, gameName;
                try {
                    auto meta = J::parse(r.customRules, nullptr, false);
                    if (meta.is_object()) { gameCode = meta.value("gameCode", ""); gameName = meta.value("gameName", ""); }
                } catch (...) {}
                arr.push_back(J{
                    {"id", r.id}, {"groupId", r.groupId}, {"gmId", r.gmId},
                    {"name", r.name.empty() ? ("log" + std::to_string(r.id)) : r.name},
                    {"status", r.status}, {"createdAt", r.createdAt},
                    {"lastAt", size.lastAt}, {"count", size.count},
                    {"storageBytes", size.databaseBytes + size.imageBytes},
                    {"imageBytes", size.imageBytes},
                    {"gameCode", gameCode}, {"gameName", gameName}
                });
            }
            jsonReply(ok(arr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});
    // 跨群团务会话：进行中的会话从索引读取，已结团的会话从日志元数据恢复。
    app.registerHandler("/api/game-sessions", [&luaMod, lst](Req, CB&& cb) {
        try {
            auto index = J::parse(luaMod.confGet("game:index", "sessions"), nullptr, false);
            J out = J::array();
            if (!index.is_object()) index = J::object();
            auto logs = lst->get_all<GameLogRow>();
            J loggedGames = J::object();
            for (const auto& log : logs) {
                try {
                    auto meta = J::parse(log.customRules, nullptr, false);
                    if (!meta.is_object()) continue;
                    std::string code = meta.value("gameCode", "");
                    if (code.empty()) continue;
                    if (!loggedGames.contains(code)) {
                        loggedGames[code] = J{{"name", meta.value("gameName", "未命名团务")},
                                              {"groups", J::array()}, {"gms", J::array()}, {"logCount", 0},
                                              {"activeLogs", 0}, {"pausedLogs", 0}, {"endedLogs", 0},
                                              {"createdAt", ""}};
                    }
                    auto& game = loggedGames[code];
                    bool knownGroup = false;
                    for (const auto& group : game["groups"]) if (group == log.groupId) { knownGroup = true; break; }
                    if (!knownGroup) game["groups"].push_back(log.groupId);
                    bool knownGm = false;
                    for (const auto& gm : game["gms"]) if (gm == log.gmId) { knownGm = true; break; }
                    if (!log.gmId.empty() && !knownGm) game["gms"].push_back(log.gmId);
                    game["logCount"] = game.value("logCount", 0) + 1;
                    if (game.value("createdAt", std::string()).empty() || log.createdAt < game.value("createdAt", std::string())) game["createdAt"] = log.createdAt;
                    if (log.status == 0) game["activeLogs"] = game.value("activeLogs", 0) + 1;
                    else if (log.status == 1) game["pausedLogs"] = game.value("pausedLogs", 0) + 1;
                    else game["endedLogs"] = game.value("endedLogs", 0) + 1;
                } catch (...) {}
            }
            for (auto it = index.begin(); it != index.end(); ++it) {
                if (!it.value().is_string()) continue;
                std::string code = it.key(), id = it.value().get<std::string>();
                std::string scope = "game:session:" + id;
                std::string name = luaMod.confGet(scope, "__name");
                if (name.empty()) continue;
                J areas = J::parse(luaMod.confGet(scope, "__areas"), nullptr, false);
                if (!areas.is_array()) areas = J::array();
                J gms = J::parse(luaMod.confGet(scope, "__gms"), nullptr, false);
                if (!gms.is_array()) gms = J::array();
                J players = J::parse(luaMod.confGet(scope, "__pls"), nullptr, false);
                if (!players.is_array()) players = J::array();
                int logCount = loggedGames.contains(code) ? loggedGames[code].value("logCount", 0) : 0;
                int activeLogs = loggedGames.contains(code) ? loggedGames[code].value("activeLogs", 0) : 0;
                int pausedLogs = loggedGames.contains(code) ? loggedGames[code].value("pausedLogs", 0) : 0;
                int endedLogs = loggedGames.contains(code) ? loggedGames[code].value("endedLogs", 0) : 0;
                std::string createdAt = loggedGames.contains(code) ? loggedGames[code].value("createdAt", "") : "";
                out.push_back(J{{"code", code}, {"name", name}, {"groups", areas},
                                {"gms", gms}, {"players", players}, {"createdAt", createdAt},
                                {"logCount", logCount}, {"activeLogs", activeLogs}, {"pausedLogs", pausedLogs},
                                {"endedLogs", endedLogs}, {"active", true}});
                loggedGames.erase(code);
            }
            for (auto it = loggedGames.begin(); it != loggedGames.end(); ++it) {
                out.push_back(J{{"code", it.key()}, {"name", it.value().value("name", "未命名团务")},
                                {"groups", it.value()["groups"]}, {"logCount", it.value().value("logCount", 0)},
                                {"gms", it.value()["gms"]}, {"players", J::array()},
                                {"createdAt", it.value().value("createdAt", "")},
                                {"activeLogs", it.value().value("activeLogs", 0)},
                                {"pausedLogs", it.value().value("pausedLogs", 0)},
                                {"endedLogs", it.value().value("endedLogs", 0)}, {"active", false}});
            }
            jsonReply(ok(out), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // POST /api/game-sessions/{code}/export {groupNames:{groupId: displayName}, format:txt|csv|html}
    // 将同一团号的各群日志按时间合并；〔场景：…〕是纯正文标记，不使用 <…> 以免被染色器当作发言标题。
    app.registerHandler("/api/game-sessions/{1}/export", [lst](Req req, CB&& cb, const std::string& code) {
        try {
            J groupNames = J::object();
            std::string fmt = "txt";
            try {
                auto payload = J::parse(req->body(), nullptr, false);
                if (payload.is_object()) {
                    if (payload["groupNames"].is_object()) groupNames = payload["groupNames"];
                    fmt = payload.value("format", "txt");
                }
            } catch (...) {}
            struct Line { GameLogMessageRow msg; std::string groupId; int order = 0; };
            std::vector<Line> lines; std::string gameName;
            for (const auto& log : lst->get_all<GameLogRow>()) {
                try {
                    auto meta = J::parse(log.customRules, nullptr, false);
                    if (!meta.is_object() || meta.value("gameCode", "") != code) continue;
                    if (gameName.empty()) gameName = meta.value("gameName", "");
                } catch (...) { continue; }
                auto msgs = lst->get_all<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == log.id),
                                                            orm::order_by(&GameLogMessageRow::id).asc());
                for (auto& msg : msgs) lines.push_back(Line{std::move(msg), log.groupId, log.id});
            }
            if (lines.empty()) { jsonReply(fail("no logs for game code"), std::move(cb)); return; }
            std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
                if (a.msg.createdAt != b.msg.createdAt) return a.msg.createdAt < b.msg.createdAt;
                if (a.order != b.order) return a.order < b.order;
                return a.msg.id < b.msg.id;
            });
            auto scene = [&](const std::string& groupId) {
                std::string s = (groupNames.contains(groupId) && groupNames[groupId].is_string()) ? groupNames[groupId].get<std::string>() : ("群 " + groupId);
                for (char& c : s) { if (c == '<') c = '('; else if (c == '>') c = ')'; else if (c == '\r' || c == '\n') c = ' '; }
                return s;
            };
            auto escCsv = [](const std::string& value) {
                std::string out = "\"";
                for (char ch : value) { if (ch == '\"') out += "\"\""; else out += ch; }
                return out + "\"";
            };
            auto escHtml = [](const std::string& value) {
                std::string out;
                for (char ch : value) {
                    if (ch == '&') out += "&amp;"; else if (ch == '<') out += "&lt;";
                    else if (ch == '>') out += "&gt;"; else if (ch == '\"') out += "&quot;";
                    else out += ch;
                }
                return out;
            };
            std::string body, mime, ext;
            if (fmt == "csv") {
                body = "\xEF\xBB\xBFscene,time,sender,user_id,content\r\n";
                for (const auto& line : lines)
                    body += escCsv(scene(line.groupId)) + "," + escCsv(line.msg.createdAt) + "," + escCsv(line.msg.sender) + "," + escCsv(line.msg.userId) + "," + escCsv(line.msg.content) + "\r\n";
                mime = "text/csv"; ext = "csv";
            } else if (fmt == "html") {
                body = "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\"><title>" + escHtml(gameName.empty() ? code : gameName) + "</title>"
                    "<style>body{font-family:system-ui,sans-serif;max-width:820px;margin:24px auto;padding:0 16px;color:#222}.msg{margin:8px 0;padding:8px 12px;border-radius:8px;background:#f5f5f7}.scene{font-size:12px;color:#6d28d9;font-weight:600}.meta{font-size:12px;color:#888}.content{white-space:pre-wrap;margin-top:2px}</style></head><body><h2>Dice!Next 跨团日志" + (gameName.empty() ? "" : "：" + escHtml(gameName)) + "</h2><p>团号：" + escHtml(code) + "</p>";
                for (const auto& line : lines) body += "<div class=\"msg\"><div class=\"scene\">〔场景：" + escHtml(scene(line.groupId)) + "〕</div><div class=\"meta\">" + escHtml(line.msg.sender) + "(" + escHtml(line.msg.userId) + ") " + escHtml(line.msg.createdAt) + "</div><div class=\"content\">" + escHtml(line.msg.content) + "</div></div>";
                body += "</body></html>"; mime = "text/html"; ext = "html";
            } else {
                body = "Dice!Next 跨团日志" + (gameName.empty() ? "" : "：" + gameName) + "\n团号：" + code + "\n\n";
                for (const auto& line : lines) {
                    body += "〔场景：" + scene(line.groupId) + "〕\n";
                    body += line.msg.sender + "(" + line.msg.userId + ") " + line.msg.createdAt + "\n";
                    body += line.msg.content + "\n\n";
                }
                mime = "text/plain"; ext = "txt";
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString(mime + "; charset=utf-8");
            resp->addHeader("Content-Disposition", "attachment; filename=\"cross_game_" + code + "." + ext + "\"");
            resp->setBody(std::move(body)); cb(resp);
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // POST /api/game-sessions/{code}/upload {groupNames:{groupId: displayName}}
    // 跨团日志没有实体 logId，直接将按时间合并后的内容编码为当前日志站协议并上传。
    app.registerHandler("/api/game-sessions/{1}/upload", [lst, &adapterMgr, &cfg](Req req, CB&& cb, const std::string& code) {
        try {
            J groupNames = J::object();
            try { auto payload = J::parse(req->body(), nullptr, false); if (payload.is_object() && payload["groupNames"].is_object()) groupNames = payload["groupNames"]; } catch (...) {}
            struct Line { GameLogMessageRow msg; std::string groupId; int order = 0; };
            std::vector<Line> lines; std::string gameName;
            for (const auto& log : lst->get_all<GameLogRow>()) {
                try {
                    auto meta = J::parse(log.customRules, nullptr, false);
                    if (!meta.is_object() || meta.value("gameCode", "") != code) continue;
                    if (gameName.empty()) gameName = meta.value("gameName", "");
                } catch (...) { continue; }
                auto msgs = lst->get_all<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == log.id), orm::order_by(&GameLogMessageRow::id).asc());
                for (auto& msg : msgs) lines.push_back(Line{std::move(msg), log.groupId, log.id});
            }
            if (lines.empty()) { jsonReply(fail("no logs for game code"), std::move(cb)); return; }
            std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
                if (a.msg.createdAt != b.msg.createdAt) return a.msg.createdAt < b.msg.createdAt;
                if (a.order != b.order) return a.order < b.order;
                return a.msg.id < b.msg.id;
            });
            auto scene = [&](const std::string& groupId) {
                std::string s = (groupNames.contains(groupId) && groupNames[groupId].is_string()) ? groupNames[groupId].get<std::string>() : ("群 " + groupId);
                for (char& ch : s) { if (ch == '<') ch = '('; else if (ch == '>') ch = ')'; else if (ch == '\r' || ch == '\n') ch = ' '; }
                return s;
            };
            auto toEpoch = [](const std::string& iso) -> long long {
                if (iso.size() < 19) return 0;
                std::tm tm{};
                try {
                    tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900; tm.tm_mon = std::stoi(iso.substr(5, 2)) - 1;
                    tm.tm_mday = std::stoi(iso.substr(8, 2)); tm.tm_hour = std::stoi(iso.substr(11, 2));
                    tm.tm_min = std::stoi(iso.substr(14, 2)); tm.tm_sec = std::stoi(iso.substr(17, 2)); tm.tm_isdst = -1;
                    return static_cast<long long>(std::mktime(&tm));
                } catch (...) { return 0; }
            };
            std::string selfId;
            for (auto& adapter : adapterMgr.allAdapters()) if (adapter && adapter->isConnected()) { selfId = adapter->getLoginId(); break; }
            std::string text = "Dice!Next 跨团日志" + (gameName.empty() ? "" : "：" + gameName) + "\n团号：" + code + "\n\n";
            J items = J::array(); std::vector<parquetw::LogRow> parquetRows; long long index = 0;
            for (const auto& line : lines) {
                const std::string sceneName = scene(line.groupId);
                const std::string content = "〔场景：" + sceneName + "〕\n" + line.msg.content;
                const std::string uid = line.msg.userId.empty() ? std::string("0") : line.msg.userId;
                text += "〔场景：" + sceneName + "〕\n" + line.msg.sender + "(" + uid + ") " + logsvc::slashTime(line.msg.createdAt) + "\n" + line.msg.content + "\n\n";
                items.push_back(J{{"id", ++index}, {"nickname", line.msg.sender}, {"IMUserId", "QQ:" + uid}, {"time", toEpoch(line.msg.createdAt)}, {"message", content}, {"isDice", !selfId.empty() && uid == selfId}, {"commandId", 0}, {"commandInfo", nullptr}, {"rawMsgId", ""}, {"uniformId", "QQ:" + uid}, {"channel", ""}});
                parquetw::LogRow row; row.id = static_cast<uint64_t>(index); row.nickname = line.msg.sender; row.imUserId = "QQ:" + uid;
                row.time = static_cast<int64_t>(toEpoch(line.msg.createdAt)); row.message = content; row.isDice = !selfId.empty() && uid == selfId;
                row.commandId = 0; row.commandInfo = ""; row.uniformId = "QQ:" + uid; parquetRows.push_back(std::move(row));
            }
            const std::string name = gameName.empty() ? ("跨团日志 " + code) : ("跨团日志：" + gameName);
            const std::string groupId = "cross-" + code;
            const std::string url = logsvc::uploadUrl(cfg);
            auto cbp = std::make_shared<CB>(std::move(cb));
            auto onDone = [cbp](bool success, std::string result) {
                if (success) jsonReply(ok(J{{"url", result}}), std::move(*cbp));
                else jsonReply(fail(result), std::move(*cbp));
            };
            const std::string format = logsvc::uploadFormat(cfg);
            if (format == "legacy") logsvc::upload(url, name, logsvc::makeUniformId(groupId), text, std::move(onDone));
            else if (format == "seal_v105") logsvc::uploadSealV105(url, name, groupId, parquetw::buildLogParquet(parquetRows), std::move(onDone));
            else if (format == "dicenext") logsvc::uploadDiceNext(url, name, groupId, logsvc::packDiceNextItems(std::move(items)), std::move(onDone));
            else logsvc::uploadSeal(url, name, groupId, std::move(items), std::move(onDone));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // ── Sessions POST ─────────────────────────────────────────
    app.registerHandler("/api/logs", [lst](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            GameLogRow s;
            s.groupId = j.value("groupId", "");
            s.gmId = j.value("gmId", "");
            s.status = 0;
            s.createdAt = utils::nowIso8601();
            s.id = lst->insert(s);
            jsonReply(ok(J{{"id", s.id}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // GET /api/logs/{id}/export?format=txt|csv — download the transcript.
    app.registerHandler("/api/logs/{1}/export",
        [lst, &db, &cfg](Req req, CB&& cb, const std::string& idStr0) {
        try {
            int id = std::stoi(idStr0);
            GameLogRow log;
            try { log = lst->get<GameLogRow>(id); } catch (...) { jsonReply(fail("not found"), std::move(cb)); return; }
            auto msgs = lst->get_all<GameLogMessageRow>(
                orm::where(orm::c(&GameLogMessageRow::logId) == id),
                orm::order_by(&GameLogMessageRow::id).asc());
            std::string fmt = req->getParameter("format");
            std::string logName = log.name.empty() ? ("log" + std::to_string(id)) : log.name;
            std::string body, mime, ext;
            if (fmt == "csv") {
                // CSV (opens in Excel). Quote fields, double internal quotes. BOM for Excel.
                auto esc = [](std::string s) {
                    std::string o = "\""; for (char c : s) { if (c == '"') o += "\"\""; else o += c; } o += "\""; return o;
                };
                body = "\xEF\xBB\xBF";   // UTF-8 BOM so Excel reads Chinese
                body += "time,sender,content\r\n";
                for (auto& m : msgs) body += esc(m.createdAt) + "," + esc(m.sender) + "," + esc(m.content) + "\r\n";
                mime = "text/csv"; ext = "csv";
            } else if (fmt == "html") {
                // 自包含网页（带图）。渲染在 logsvc::renderHtml（与 .log type html
                // 的群文件上传共用，）。
                body = logsvc::renderHtml(db, id); mime = "text/html"; ext = "html";
            } else {
                // TXT: identical to what we upload to the log site (renderSealdice),
                // so the downloaded file matches the online transcript byte-for-byte.
                body = logsvc::renderSealdice(db, id, &cfg);   // 图片→稳定图床 URL
                mime = "text/plain"; ext = "txt";
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString(mime + "; charset=utf-8");
            // 按新版命名规则 q_<群号>_<日志名>.<ext>（原来固定 log_<id>.<ext>）。
            // 中文名走 RFC 5987 filename*，ASCII 回退避免旧客户端乱码。
            auto safe = [](std::string s) {
                for (char& c : s) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|'||(unsigned char)c<0x20) c='_';
                return s;
            };
            std::string downloadName = "q_" + safe(log.groupId) + "_" + safe(logName) + "." + ext;
            auto pct = [](const std::string& s) {
                static const char* hex = "0123456789ABCDEF"; std::string o;
                for (unsigned char c : s) {
                    bool un = (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='.'||c=='-'||c=='_';
                    if (un) o += (char)c; else { o += '%'; o += hex[c>>4]; o += hex[c&15]; }
                }
                return o;
            };
            std::string ascii;
            for (char c : downloadName) ascii += ((unsigned char)c>=0x20 && (unsigned char)c<0x80 && c!='"' && c!='\\') ? c : '_';
            resp->addHeader("Content-Disposition",
                "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + pct(downloadName));
            resp->setBody(body);
            cb(resp);
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // GET /api/logs/images/{file} — 服务落地的日志图片（local 图床模式 + 预览）。
    app.registerHandler("/api/logs/images/{1}",
        [](Req, CB&& cb, std::string file) {
            // 仅取 basename，防路径穿越。
            if (auto p = file.find_last_of("/\\"); p != std::string::npos) file = file.substr(p + 1);
            std::filesystem::path path = std::filesystem::path("data/logs/images") / file;
            std::error_code ec;
            if (file.empty() || file.find("..") != std::string::npos || !std::filesystem::exists(path, ec)) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k404NotFound); r->setBody("not found"); cb(r); return;
            }
            std::ifstream f(path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string mime = "image/jpeg";
            if (file.size() > 4 && file.substr(file.size() - 4) == ".png") mime = "image/png";
            else if (file.size() > 4 && file.substr(file.size() - 4) == ".gif") mime = "image/gif";
            else if (file.size() > 4 && file.substr(file.size() - 4) == ".svg") mime = "image/svg+xml";
            else if (file.size() > 5 && file.substr(file.size() - 5) == ".webp") mime = "image/webp";
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString(mime);
            resp->setBody(std::move(bytes));
            cb(resp);
        }, {drogon::Get});

    // GET /api/chat/images/{file} — 服务模拟聊天缓存的入站图片（NTQQ 图收到时
    // 已下到本地，避免网页直连 QQ 图床 rkey 失效 400）。与 logs/images、assets 分开目录。
    app.registerHandler("/api/chat/images/{1}",
        [](Req, CB&& cb, std::string file) {
            if (auto p = file.find_last_of("/\\"); p != std::string::npos) file = file.substr(p + 1);
            std::filesystem::path path = std::filesystem::path("data/chat/images") / file;
            std::error_code ec;
            if (file.empty() || file.find("..") != std::string::npos || !std::filesystem::exists(path, ec)) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setStatusCode(drogon::k404NotFound); r->setBody("not found"); cb(r); return;
            }
            std::ifstream f(path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string mime = "image/jpeg";
            if (file.size() > 4 && file.substr(file.size() - 4) == ".png") mime = "image/png";
            else if (file.size() > 4 && file.substr(file.size() - 4) == ".gif") mime = "image/gif";
            else if (file.size() > 5 && file.substr(file.size() - 5) == ".jpeg") mime = "image/jpeg";
            else if (file.size() > 5 && file.substr(file.size() - 5) == ".webp") mime = "image/webp";
            else if (file.size() > 4 && file.substr(file.size() - 4) == ".bmp") mime = "image/bmp";
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeString(mime);
            resp->setBody(std::move(bytes));
            cb(resp);
        }, {drogon::Get});

    // POST /api/logs/{id}/upload — render + upload to the log site, return {url}.
    app.registerHandler("/api/logs/{1}/upload",
        [&adapterMgr, &db, &cfg](Req, CB&& cb, const std::string& idStr0) {
        try {
            int id = std::stoi(idStr0);
            GameLogRow log;
            try { log = db.getLogStorage()->get<GameLogRow>(id); }
            catch (...) { jsonReply(fail("not found"), std::move(cb)); return; }
            std::string name = log.name.empty() ? ("log" + std::to_string(id)) : log.name;
            std::string url = logsvc::uploadUrl(cfg);
            auto cbp = std::make_shared<CB>(std::move(cb));
            auto onDone = [cbp](bool success, std::string res) {
                if (success) jsonReply(ok(J{{"url", res}}), std::move(*cbp));
                else jsonReply(fail(res), std::move(*cbp));
            };
            // 与 .log 指令一致，按 logsite_format 选协议。原来群管理上传固定走旧
            // POST 仅用于旧端点；默认端点使用 PUT。
            std::string fmt = logsvc::uploadFormat(cfg);
            std::string selfId;   // isDice 染色：取任一已连适配器
            for (auto& a : adapterMgr.allAdapters()) if (a && a->isConnected()) { selfId = a->getLoginId(); break; }
            if (fmt == "legacy") {
                std::string txt = logsvc::renderSealdice(db, id, &cfg);   // 图片→稳定图床 URL
                if (txt.empty()) { jsonReply(fail("empty log"), std::move(*cbp)); return; }
                logsvc::upload(url, name, logsvc::makeUniformId(log.groupId), txt, std::move(onDone));
            } else if (fmt == "seal_v105") {
                // PUT Parquet(zstd) 列式文件。
                logsvc::uploadSealV105(url, name, log.groupId,
                    logsvc::renderSealParquet(db, id, &cfg, selfId), std::move(onDone));
            } else if (fmt == "dicenext") {
                // DiceNext 专属：PUT zstd 压缩的 items JSON。
                logsvc::uploadDiceNext(url, name, log.groupId,
                    logsvc::renderDiceNextData(db, id, &cfg, selfId), std::move(onDone));
            } else {
                // 默认使用 PUT zlib 压缩的 items JSON。
                logsvc::uploadSeal(url, name, log.groupId,
                    logsvc::renderSealItems(db, id, &cfg, selfId), std::move(onDone));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // DELETE /api/logs/{id} — remove a log, its messages, AND its cached local images
    // （落地的图片命名为 log<id>_<时间>_<序>.<ext>，按前缀清理；不污染其它日志的图片）。
    app.registerHandler("/api/logs/{1}",
        [st, lst](Req, CB&& cb, const std::string& idStr0) {
        try {
            int id = std::stoi(idStr0);
            try { (void)lst->get<GameLogRow>(id); }
            catch (...) { jsonReply(fail("not found"), std::move(cb)); return; }

            // WebUI 可以删除正在记录的日志；同步清掉各帐号及旧共享层中的激活引用，
            // 避免后续消息继续写入已经不存在的日志。
            const std::string idText = std::to_string(id);
            for (auto row : st->get_all<GroupAccountSettingRow>(orm::where(
                     orm::c(&GroupAccountSettingRow::key) == std::string("activeLog") and
                     orm::c(&GroupAccountSettingRow::value) == idText))) {
                auto names = st->get_all<GroupAccountSettingRow>(orm::where(
                    orm::c(&GroupAccountSettingRow::adapterId) == row.adapterId and
                    orm::c(&GroupAccountSettingRow::groupId) == row.groupId and
                    orm::c(&GroupAccountSettingRow::key) == std::string("activeLogName")));
                for (auto name : names) { name.value.clear(); st->update(name); }
                row.value.clear(); st->update(row);
            }
            for (auto row : st->get_all<GroupSettingRow>(orm::where(
                     orm::c(&GroupSettingRow::key) == std::string("activeLog") and
                     orm::c(&GroupSettingRow::value) == idText))) {
                auto names = st->get_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == row.platform and
                    orm::c(&GroupSettingRow::groupId) == row.groupId and
                    orm::c(&GroupSettingRow::key) == std::string("activeLogName")));
                for (auto name : names) { name.value.clear(); st->update(name); }
                row.value.clear(); st->update(row);
            }
            lst->remove_all<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == id));
            lst->remove_all<GameLogRow>(orm::where(orm::c(&GameLogRow::id) == id));
            // 同步删掉本群保存到本地的日志图片缓存（开启「保存日志图片」时才有）。
            namespace fs = std::filesystem; std::error_code ec;
            int imgRemoved = 0;
            const std::string prefix = "log" + std::to_string(id) + "_";   // 末尾下划线避免 log1_ 误伤 log11_
            for (const char* dir : {"data/logs/images", "../data/logs/images"}) {
                if (!fs::is_directory(dir, ec)) continue;
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    if (!e.is_regular_file(ec)) continue;
                    if (dnx_u8str(e.path().filename()).rfind(prefix, 0) == 0) {
                        std::error_code de; if (fs::remove(e.path(), de) && !de) ++imgRemoved;
                    }
                }
                break;   // 第一个存在的目录处理完即可
            }
            jsonReply(ok(J{{"imagesRemoved", imgRemoved}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // ── Local aggregate statistics ─────────────────────────────
    app.registerHandler("/api/statistics/overview", [st, &adapterMgr](Req, CB&& cb) {
        try {
            long long totalCommands = 0;
            std::map<std::string, PlayerProfileRow> players;
            for (const auto& row : st->get_all<PlayerProfileRow>()) {
                totalCommands += row.cmdCount;
                auto it = players.find(row.userId);
                if (it == players.end()) players.emplace(row.userId, row);
                else {
                    it->second.cmdCount += row.cmdCount;
                    if (it->second.nickname.empty() && !row.nickname.empty()) it->second.nickname = row.nickname;
                    if (row.lastCmdAt > it->second.lastCmdAt) it->second.lastCmdAt = row.lastCmdAt;
                }
            }

            std::array<long long, 24> commandHours{};
            std::array<long long, 24> rollHours{};
            long long totalRolls = 0;
            for (const auto& row : st->get_all<UsageHourRow>()) {
                if (row.hour < 0 || row.hour > 23) continue;
                commandHours[static_cast<std::size_t>(row.hour)] += row.commandCount;
                rollHours[static_cast<std::size_t>(row.hour)] += row.rollCount;
                totalRolls += row.rollCount;
            }

            std::map<int, std::map<int, long long>> faceCounts;
            for (const auto& row : st->get_all<DiceFaceStatRow>())
                if (row.sides >= 2 && row.face >= 1 && row.face <= row.sides)
                    faceCounts[row.sides][row.face] += row.count;

            J diceFaces = J::array();
            for (const auto& [sides, faces] : faceCounts) {
                J faceArray = J::array();
                long long subtotal = 0;
                for (const auto& [face, count] : faces) {
                    faceArray.push_back(J{{"face", face}, {"count", count}});
                    subtotal += count;
                }
                diceFaces.push_back(J{{"sides", sides}, {"total", subtotal}, {"faces", faceArray}});
            }

            J checkResults{{"crit", 0LL}, {"extreme", 0LL}, {"hard", 0LL},
                           {"regular", 0LL}, {"fail", 0LL}, {"fumble", 0LL}};
            for (const auto& row : st->get_all<RollStatRow>()) {
                checkResults["crit"] = checkResults["crit"].get<long long>() + row.crit;
                checkResults["extreme"] = checkResults["extreme"].get<long long>() + row.extreme;
                checkResults["hard"] = checkResults["hard"].get<long long>() + row.hard;
                checkResults["regular"] = checkResults["regular"].get<long long>() + row.regular;
                checkResults["fail"] = checkResults["fail"].get<long long>() + row.fail;
                checkResults["fumble"] = checkResults["fumble"].get<long long>() + row.fumble;
            }

            std::vector<PlayerProfileRow> ranked;
            for (const auto& [_, player] : players) if (player.cmdCount > 0) ranked.push_back(player);
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                return a.cmdCount > b.cmdCount;
            });
            J topUsers = J::array();
            for (std::size_t i = 0; i < ranked.size() && i < 10; ++i)
                topUsers.push_back(J{{"nickname", ranked[i].nickname}, {"user_id", ranked[i].userId},
                                     {"command_count", ranked[i].cmdCount}, {"last_command_at", ranked[i].lastCmdAt}});

            auto samples = st->get_all<OnlineSampleRow>(orm::order_by(&OnlineSampleRow::sampledAt).desc(),
                                                        orm::limit(2016));
            std::reverse(samples.begin(), samples.end());
            J onlineHistory = J::array();
            for (const auto& row : samples)
                onlineHistory.push_back(J{{"sampled_at", row.sampledAt},
                                          {"online_count", row.onlineCount},
                                          {"total_count", row.totalCount}});

            int adapterTotal = 0, adapterOnline = 0;
            for (const auto& adapter : adapterMgr.allAdapters()) {
                ++adapterTotal;
                if (adapter->isConnected()) ++adapterOnline;
            }
            J hours = J::array();
            for (int hour = 0; hour < 24; ++hour)
                hours.push_back(J{{"hour", hour}, {"commands", commandHours[hour]}, {"rolls", rollHours[hour]}});

            jsonReply(ok(J{
                {"summary", J{{"total_commands", totalCommands}, {"total_rolls", totalRolls},
                               {"total_players", ranked.size()}, {"adapter_online", adapterOnline},
                               {"adapter_total", adapterTotal},
                               {"uptime_seconds", static_cast<long long>(std::time(nullptr) - utils::getStartupEpoch())}}},
                {"usage_by_hour", hours},
                {"dice_faces", diceFaces},
                {"check_results", checkResults},
                {"top_users", topUsers},
                {"online_history", onlineHistory},
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // ── Players (auto-built profiles) ─────────────────────────
    // GET /api/players — list profiles (信任等级/QQ/昵称/上次指令/总指令数).
    app.registerHandler("/api/players", [st](Req, CB&& cb) {
        try {
            // Only surface players who have actually issued ≥1 command. Profiles
            // are auto-created for everyone who talks (so we cache nickname/etc.),
            // but lurkers with 0 commands are hidden from the management UI.
            auto rows = st->get_all<PlayerProfileRow>(
                orm::order_by(&PlayerProfileRow::lastCmdAt).desc());
            std::map<std::string, PlayerProfileRow> merged;
            for (auto& r : rows) {
                if (r.cmdCount == 0 && r.favor == 0 && r.trustLevel == 0 && r.nickname.empty()) continue;  // skip empty placeholders
                auto it = merged.find(r.userId);
                if (it == merged.end()) merged.emplace(r.userId, r);
                else {
                    it->second.cmdCount += r.cmdCount;
                    it->second.groupCmdCount += r.groupCmdCount;
                    it->second.favor += r.favor;
                    it->second.trustLevel = (std::max)(it->second.trustLevel, r.trustLevel);
                    if (it->second.nickname.empty() && !r.nickname.empty()) it->second.nickname = r.nickname;
                }
            }
            J arr = J::array();
            for (auto& [uid, r] : merged) {
                J bindings = J::array(); bool virtualId = identity::BindingStore::isVirtual(uid);
                try {
                    auto ids = st->get_all<IdentityRow>(orm::where(
                        orm::c(&IdentityRow::kind) == "user" and orm::c(&IdentityRow::publicId) == uid), orm::limit(1));
                    if (!ids.empty()) for (const auto& ep : st->get_all<IdentityEndpointRow>(orm::where(
                        orm::c(&IdentityEndpointRow::identityId) == ids.front().id)))
                        bindings.push_back(J{{"adapterType", ep.adapterType}, {"adapterAccount", ep.adapterAccount}, {"endpointId", ep.endpointId}});
                } catch (...) {}
                arr.push_back(J{
                    {"platform", r.platform}, {"userId", r.userId},
                    {"nickname", r.nickname}, {"trustLevel", r.trustLevel},
                    {"cmdCount", r.cmdCount}, {"groupCmdCount", r.groupCmdCount},
                    {"favor", r.favor}, {"lastCmdAt", r.lastCmdAt},
                    {"createdAt", r.createdAt}, {"virtualId", virtualId}, {"bindings", bindings}
                });
            }
            jsonReply(ok(arr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // PUT /api/players/{platform}/{userId}  {trustLevel}  — edit trust.
    // DELETE /api/players/{platform}/{userId}             — remove profile.
    app.registerHandler("/api/players/{1}/{2}",
        [st](Req req, CB&& cb, const std::string& plat, const std::string& uid) {
        try {
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == plat
                    and orm::c(&PlayerProfileRow::userId) == uid));
            if (rows.empty()) { jsonReply(fail("not found"), std::move(cb)); return; }
            if (req->method() == drogon::Delete) {
                st->remove_all<PlayerProfileRow>(
                    orm::where(orm::c(&PlayerProfileRow::platform) == plat
                        and orm::c(&PlayerProfileRow::userId) == uid));
                jsonReply(ok(), std::move(cb)); return;
            }
            auto j = J::parse(req->body());
            auto r = rows.front();
            if (j.contains("trustLevel")) r.trustLevel = j.value("trustLevel", 0);
            if (j.contains("favor")) r.favor = j.value("favor", 0);
            st->update(r);
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Delete});

    // ── 各平台好友 uid 列表与删除能力。QQ 官方没有删除好友接口；其已
    // 绑定真实 QQ 的身份仅在 OneBot 好友列表确认后才可从 WebUI 删除。
    app.registerHandler("/api/friends", [&adapterMgr](Req, CB&& cb) {
        try {
            J lists = J::object(), deletePlatforms = J::array(), officialRealFriends = J::array();
            std::set<std::string> oneBotFriends;
            for (auto& a : adapterMgr.allAdapters()) {
                if (!a->isConnected()) continue;
                J arr = J::array();
                const auto friends = a->getFriendList();
                for (auto& f : friends) arr.push_back(f);
                // 同平台多适配器：合并
                if (lists.contains(a->platform()))
                    for (auto& v : arr) lists[a->platform()].push_back(v);
                else lists[a->platform()] = arr;
                const auto cap = a->capabilities();
                if (cap.value("friend_delete", false)) deletePlatforms.push_back(a->platform());
                if (a->platform() == "onebot_v11")
                    oneBotFriends.insert(friends.begin(), friends.end());
            }
            for (const auto& id : oneBotFriends) officialRealFriends.push_back(id);
            jsonReply(ok(J{{"lists", lists}, {"deletePlatforms", deletePlatforms},
                             {"officialRealFriends", officialRealFriends}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // ── 玩家详情：该用户的全部资料（所在群/人物卡/设置键值/Lua插件数据）──
    app.registerHandler("/api/players/{1}/{2}/detail",
        [&db, &adapterMgr, &luaMod, st](Req, CB&& cb, const std::string& plat, const std::string& uid) {
        try {
            J out = J::object();
            // 所在群：chat.db 里该用户发过言的群（持久） + user_settings 出现过的群。
            // 私聊记录使用 private:<uid> 作为内部会话范围，不应显示为一个群。
            std::set<std::string> gids;
            const std::string privateScope = "private:" + uid;
            int64_t lastMessageAt = 0;
            if (auto* cst = db.getChatStorage()) {
                try {
                    auto v = cst->select(orm::distinct(&ChatMsgRow::groupId),
                        orm::where(orm::c(&ChatMsgRow::platform) == plat
                            and orm::c(&ChatMsgRow::userId) == uid));
                    for (auto& g : v) if (!g.empty() && g != privateScope) gids.insert(g);
                    auto messages = cst->get_all<ChatMsgRow>(
                        orm::where(orm::c(&ChatMsgRow::platform) == plat
                            and orm::c(&ChatMsgRow::userId) == uid),
                        orm::order_by(&ChatMsgRow::time).desc(), orm::limit(1));
                    if (!messages.empty()) lastMessageAt = messages.front().time;
                } catch (...) {}
            }
            out["lastMessageAt"] = lastMessageAt;
            auto settings = st->get_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == uid));
            for (auto& s : settings) if (!s.groupId.empty()) gids.insert(s.groupId);
            // 群名：经该平台已连接适配器的群缓存解析（查不到则回退群号）。
            std::shared_ptr<IAdapter> ad;
            for (auto& a : adapterMgr.allAdapters())
                if (a->platform() == plat && a->isConnected()) { ad = a; break; }
            J groups = J::array();
            for (auto& g : gids)
                groups.push_back(J{{"id", g}, {"name", ad ? ad->getGroupName(g) : g}});
            out["groups"] = groups;
            // 人物卡（cards.db，含各群绑定）。
            J cards = J::array();
            if (auto* crd = db.getCardStorage()) {
                try {
                    for (auto& r : crd->get_all<CharacterCardRow>(
                            orm::where(orm::c(&CharacterCardRow::userId) == uid))) {
                        J attrs = J::parse(r.attrs, nullptr, false);
                        if (!attrs.is_object()) attrs = J::object();
                        J bound = J::array();
                        for (auto& s : settings)
                            if (s.key == "cardBind" && s.value == r.name) bound.push_back(s.groupId);
                        cards.push_back(J{{"id", r.id}, {"name", r.name},
                                          {"attrs", attrs}, {"bound", bound},
                                          {"updatedAt", r.updatedAt}});
                    }
                } catch (...) {}
            }
            out["cards"] = cards;
            // 设置键值（.nn/.set/上限/武器/牌堆背包… user_settings 全量，按行可改删）。
            J sarr = J::array();
            for (auto& s : settings)
                sarr.push_back(J{{"id", s.id}, {"groupId", s.groupId}, {"key", s.key}, {"value", s.value}});
            out["settings"] = sarr;
            // Lua 插件变量（lua_conf u:<uid>）与 Lua 卡片数据（lua_card，如背包）。
            J lv = J::array();
            for (auto& [k, v] : luaMod.confAllOf("u:" + uid))
                lv.push_back(J{{"key", k}, {"value", v}});
            out["luaVars"] = lv;
            J lc = J::array();
            for (auto& [sc, d] : luaMod.cardAllOf(uid))
                lc.push_back(J{{"scope", sc}, {"data", d}});
            out["luaCards"] = lc;
            jsonReply(ok(out), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // 人物卡属性改/删：{card, attr, value?}（value 缺省/null → 删除该属性）。
    app.registerHandler("/api/players/{1}/{2}/card-attr",
        [&db](Req req, CB&& cb, const std::string&, const std::string& uid) {
        try {
            auto j = J::parse(req->body());
            std::string card = j.value("card", std::string());
            std::string attr = j.value("attr", std::string());
            if (attr.empty()) { jsonReply(fail("attr required"), std::move(cb)); return; }
            auto* crd = db.getCardStorage();
            if (!crd) { jsonReply(fail("cards.db unavailable"), std::move(cb)); return; }
            auto rows = crd->get_all<CharacterCardRow>(
                orm::where(orm::c(&CharacterCardRow::userId) == uid
                    and orm::c(&CharacterCardRow::name) == card));
            if (rows.empty()) { jsonReply(fail("card not found"), std::move(cb)); return; }
            auto r = rows.front();
            J attrs = J::parse(r.attrs, nullptr, false);
            if (!attrs.is_object()) attrs = J::object();
            if (!j.contains("value") || j["value"].is_null()) attrs.erase(attr);
            else attrs[attr] = j["value"];   // 前端按内容传 number 或 string，原样保存
            r.attrs = attrs.dump();
            crd->update(r);
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 删除整张人物卡（连带清掉指向它的各群绑定）：{card}。
    app.registerHandler("/api/players/{1}/{2}/card-del",
        [&db, st](Req req, CB&& cb, const std::string&, const std::string& uid) {
        try {
            auto j = J::parse(req->body());
            std::string card = j.value("card", std::string());
            auto* crd = db.getCardStorage();
            if (!crd) { jsonReply(fail("cards.db unavailable"), std::move(cb)); return; }
            crd->remove_all<CharacterCardRow>(
                orm::where(orm::c(&CharacterCardRow::userId) == uid
                    and orm::c(&CharacterCardRow::name) == card));
            st->remove_all<UserSettingRow>(
                orm::where(orm::c(&UserSettingRow::userId) == uid
                    and orm::c(&UserSettingRow::key) == std::string("cardBind")
                    and orm::c(&UserSettingRow::value) == card));
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 设置键值改/删：{id, value?}（value 缺省/null → 删除该行）。
    app.registerHandler("/api/players/{1}/{2}/setting",
        [st](Req req, CB&& cb, const std::string&, const std::string& uid) {
        try {
            auto j = J::parse(req->body());
            int id = j.value("id", 0);
            UserSettingRow row;
            try { row = st->get<UserSettingRow>(id); }
            catch (...) { jsonReply(fail("setting not found"), std::move(cb)); return; }
            if (row.userId != uid) { jsonReply(fail("user mismatch"), std::move(cb)); return; }
            if (!j.contains("value") || j["value"].is_null()) st->remove<UserSettingRow>(id);
            else { row.value = j["value"].is_string() ? j["value"].get<std::string>() : j["value"].dump(); st->update(row); }
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Lua 插件变量改/删：{key, value?}（value 缺省/null/空串 → 删除）。
    app.registerHandler("/api/players/{1}/{2}/luavar",
        [&luaMod](Req req, CB&& cb, const std::string&, const std::string& uid) {
        try {
            auto j = J::parse(req->body());
            std::string key = j.value("key", std::string());
            if (key.empty()) { jsonReply(fail("key required"), std::move(cb)); return; }
            std::string val;
            if (j.contains("value") && !j["value"].is_null())
                val = j["value"].is_string() ? j["value"].get<std::string>() : j["value"].dump();
            luaMod.confSet("u:" + uid, key, val);   // 空串即删除（confSet 语义）
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Lua 卡片数据改/删：{scope, data?}（data 缺省/null → 删除；否则须为 JSON 对象串）。
    app.registerHandler("/api/players/{1}/{2}/luacard",
        [&luaMod](Req req, CB&& cb, const std::string&, const std::string& uid) {
        try {
            auto j = J::parse(req->body());
            std::string scope = j.value("scope", std::string());
            if (!j.contains("data") || j["data"].is_null()) { luaMod.cardDel(uid, scope); jsonReply(ok(), std::move(cb)); return; }
            std::string data = j["data"].is_string() ? j["data"].get<std::string>() : j["data"].dump();
            J parsed = J::parse(data, nullptr, false);
            if (!parsed.is_object()) { jsonReply(fail("data must be a JSON object"), std::move(cb)); return; }
            luaMod.cardSave(uid, scope, parsed.dump());
            jsonReply(ok(), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Pick a connected adapter for a platform.  A binding carries its adapter
    // account too, so a simulation can deliberately choose between multiple
    // official bots instead of accidentally using the first one.
    static auto pickAdapter = [](AdapterManager& mgr, const std::string& plat,
                                 const std::string& account = {}) -> AdapterPtr {
        AdapterPtr any;
        for (auto& a : mgr.allAdapters()) {
            if (!a->isConnected()) continue;
            if (a->platform() == plat) {
                if (!any) any = a;
                if (account.empty() || a->id() == account) return a;
                if (plat == "qq_official") {
                    auto official = std::dynamic_pointer_cast<QQOfficialAdapter>(a);
                    if (official && official->appId() == account) return a;
                }
            }
        }
        return account.empty() ? any : nullptr;
    };

    // WebUI stores and displays conversations under a public identity, while
    // adapters must send to their own native endpoint.  Resolve the selected
    // binding exactly so a virtual public number is never sent to a platform
    // API as though it were a real group/user id.
    static auto selectedTransportEndpoint = [](Database& database, const std::string& publicId,
                                               const std::string& type, const std::string& account,
                                               const std::string& requestedEndpoint,
                                               identity::Kind kind) -> std::string {
        const auto endpoints = identity::BindingStore::instance().endpoints(database, publicId, kind);
        if (endpoints.empty()) {
            return identity::BindingStore::isVirtual(publicId) ? std::string{} : publicId;
        }
        for (const auto& endpoint : endpoints) {
            if (endpoint.adapterType != type) continue;
            if (!account.empty() && endpoint.adapterAccount != account) continue;
            if (!requestedEndpoint.empty() && endpoint.endpointId != requestedEndpoint) continue;
            return endpoint.endpointId;
        }
        return {};
    };

    // Read an id field that may be a number or string (OneBot inconsistency).
    static auto idStr = [](const J& m, const char* key) -> std::string {
        if (!m.contains(key) || m[key].is_null()) return "";
        const auto& v = m[key];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number()) return std::to_string(v.get<int64_t>());
        return "";
    };

    // GET /api/groups — live joined-group list merged with group_settings.
    app.registerHandler("/api/groups", [st, &adapterMgr, &cfg, &db](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Post) {
                auto j = J::parse(req->body());
                std::string plat = j.value("platform", "onebot_v11");
                std::string gid  = j.value("groupId", "");
                if (gid.empty()) { jsonReply(fail("groupId required"), std::move(cb)); return; }
                gsSet(st, plat, gid, "__removed", "0");   // 清除墓碑，重新纳入管理
                gsSet(st, plat, gid, "left", "0");        // 手动重加也清「已退群」状态
                gsSet(st, plat, gid, "leaving", "0");
                if (gsGet(st, plat, gid, "enabled").empty()) gsSet(st, plat, gid, "enabled", "1");
                jsonReply(ok(J{{"platform",plat},{"groupId",gid}}), std::move(cb));
                return;
            }
            // Auto-discover each account independently.  The native group cache is
            // owned by the adapter; only that adapter's rows may be marked left.
            for (auto& a : adapterMgr.allAdapters()) {
                if (!a->isConnected()) continue;
                std::string plat = a->platform();
                const std::string aid = a->id();
                std::map<std::string, std::string> publicGroups; // public id -> native endpoint
                for (auto& [endpoint, gname] : a->getGroupList()) {
                    std::string gid = endpoint;
                    if (plat == "onebot_v11")
                        gid = identity::BindingStore::instance().observeQQ(db, plat, aid, endpoint, identity::Kind::Group);
                    else if (plat == "discord" || plat == "kook")
                        gid = identity::BindingStore::instance().observeVirtual(db, plat, aid, endpoint, identity::Kind::Group);
                    if (gid.empty()) gid = endpoint;
                    publicGroups[gid] = endpoint;
                    agsSet(st, aid, plat, gid, endpoint, "__removed", "0");
                    agsSet(st, aid, plat, gid, endpoint, "left", "0");
                    if (agsGet(st, aid, plat, gid, "enabled").empty())
                        agsSet(st, aid, plat, gid, endpoint, "enabled", "1");
                    if (!gname.empty() && gname != endpoint) gsSet(st, plat, gid, "name", gname);
                    if (a->getSelfRole(endpoint).empty()) a->refreshSelfRole(endpoint);
                }
                a->refreshGroupList();

                // An empty cache can mean the asynchronous list has not arrived
                // yet.  Never archive every group merely because of that race.
                syncAccountGroupPresence(*st, aid, plat, publicGroups);
            }
            auto rows = st->get_all<GroupSettingRow>();
            std::map<std::string, std::map<std::string, std::string>> groups; // "plat\x1fgid" → kv
            for (auto& r : rows) groups[r.platform + "\x1f" + r.groupId][r.key] = r.value;
            auto accountRows = st->get_all<GroupAccountSettingRow>();
            std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> accountGroups;
            std::map<std::string, std::pair<std::string, std::string>> accountMeta; // gid/aid -> platform, endpoint
            for (auto& r : accountRows) {
                accountGroups[r.groupId][r.adapterId][r.key] = r.value;
                accountMeta[r.groupId + "\x1f" + r.adapterId] = {r.platform, r.endpointId};
                // Ensure a group with no shared metadata is still represented.
                groups[r.platform + "\x1f" + r.groupId];
            }
            // 预载各群语言覆盖（locale_settings scope=group，key "<plat>:<gid>"）。
            std::map<std::string, std::string> groupLocales;
            for (auto& lr : st->get_all<LocaleSettingRow>(
                     orm::where(orm::c(&LocaleSettingRow::scope) == std::string("group"))))
                groupLocales[lr.scopeKey] = lr.locale;
            // A real QQ group can be reached through more than one adapter.  The
            // management view represents that group once, and keeps every actual
            // adapter endpoint as metadata instead of rendering duplicate cards.
            std::map<std::string, std::string> selectedByGroup;
            for (auto& [pg, kv] : groups) {
                if (kv.count("__removed") && kv["__removed"] == "1") continue;
                auto sep = pg.find('\x1f');
                std::string plat = pg.substr(0, sep), gid = pg.substr(sep + 1);
                auto it = selectedByGroup.find(gid);
                if (it == selectedByGroup.end()
                    || (it->second.rfind("qq_official\x1f", 0) == 0 && plat != "qq_official")) {
                    selectedByGroup[gid] = pg;
                }
            }

            std::map<int64_t, IdentityRow> identities;
            std::map<std::string, J> groupBindings;
            try {
                for (const auto& row : st->get_all<IdentityRow>()) identities[row.id] = row;
                for (const auto& ep : st->get_all<IdentityEndpointRow>()) {
                    auto it = identities.find(ep.identityId);
                    if (it == identities.end() || it->second.kind != "group") continue;
                    auto& list = groupBindings[it->second.publicId];
                    if (!list.is_array()) list = J::array();
                    list.push_back(J{{"adapterType", ep.adapterType}, {"adapterAccount", ep.adapterAccount},
                                     {"endpointId", ep.endpointId}});
                }
            } catch (...) {}

            J arr = J::array();
            for (auto& [gid, pg] : selectedByGroup) {
                auto& kv = groups[pg];
                if (kv.count("__removed") && kv["__removed"] == "1") continue;   // 隐藏已移除的群
                auto sep = pg.find('\x1f');
                std::string plat = pg.substr(0, sep);
                std::string name = (kv.count("name") && !kv["name"].empty()) ? kv["name"] : gid;
                J accounts = J::array();
                if (accountGroups.count(gid)) {
                    for (auto& [aid, akv] : accountGroups[gid]) {
                        if (akv.count("__removed") && akv["__removed"] == "1") continue;
                        auto meta = accountMeta[gid + "\x1f" + aid];
                        const std::string aplat = meta.first.empty() ? plat : meta.first;
                        const std::string endpoint = meta.second.empty() ? gid : meta.second;
                        auto value = [&](const std::string& key) -> std::string {
                            auto it = akv.find(key); if (it != akv.end()) return it->second;
                            auto old = kv.find(key); return old == kv.end() ? std::string() : old->second;
                        };
                        auto a = adapterMgr.getAdapter(aid);
                        std::string accountName = aid, loginId, loginName, appId, botRole;
                        int memberCount = 0;
                        bool connected = a && a->isConnected();
                        if (a) {
                            accountName = a->name(); loginId = a->getLoginId(); loginName = a->getLoginName();
                            if (auto off = std::dynamic_pointer_cast<QQOfficialAdapter>(a)) appId = off->appId();
                            std::string gn = a->getGroupName(endpoint);
                            if (name == gid && !gn.empty() && gn != endpoint) name = gn;
                            botRole = a->getSelfRole(endpoint);
                            memberCount = a->getGroupMemberCount(endpoint);
                        }
                        int observers = 0;
                        try { auto o = J::parse(value("observers")); if (o.is_array()) observers = (int)o.size(); } catch (...) {}
                        auto glIt = groupLocales.find(aplat + ":" + gid);
                        accounts.push_back(J{
                            {"adapterId", aid}, {"adapterName", accountName}, {"loginId", loginId},
                            {"loginName", loginName}, {"appId", appId},
                            {"platform", aplat}, {"endpointId", endpoint}, {"connected", connected},
                            {"enabled", value("enabled") != "0"}, {"ai_enabled", value("aiEnabled") != "0"},
                            {"locked", value("locked") == "1"}, {"card", value("card")},
                            {"activeLog", !value("activeLog").empty()}, {"activeLogId", value("activeLog")},
                            {"activeLogName", value("activeLogName")}, {"observers", observers},
                            {"botRole", botRole}, {"memberCount", memberCount}, {"inviter", value("inviter")},
                            {"locale", value("locale").empty() ? (glIt != groupLocales.end() ? glIt->second : std::string()) : value("locale")},
                            {"left", value("left") == "1"}, {"welcome", value("welcome")},
                            {"welcome_delay", value("welcome_delay")}, {"welcome_cooldown", value("welcome_cooldown")}
                        });
                    }
                }

                // Compatibility for databases that have not observed an adapter
                // since upgrading: expose the old shared state as one legacy item.
                if (accounts.empty()) {
                    int observers = 0;
                    if (kv.count("observers")) { try { auto o = J::parse(kv["observers"]); if (o.is_array()) observers = (int)o.size(); } catch (...) {} }
                    auto glIt = groupLocales.find(plat + ":" + gid);
                    accounts.push_back(J{{"adapterId", ""}, {"adapterName", ""}, {"loginId", ""},
                        {"platform", plat}, {"endpointId", gid}, {"connected", false},
                        {"enabled", !kv.count("enabled") || kv["enabled"] != "0"},
                        {"ai_enabled", !kv.count("aiEnabled") || kv["aiEnabled"] != "0"},
                        {"locked", kv.count("locked") && kv["locked"] == "1"}, {"card", kv.count("card") ? kv["card"] : ""},
                        {"activeLog", kv.count("activeLog") && !kv["activeLog"].empty()},
                        {"activeLogId", kv.count("activeLog") ? kv["activeLog"] : ""}, {"activeLogName", kv.count("activeLogName") ? kv["activeLogName"] : ""},
                        {"observers", observers}, {"botRole", ""}, {"memberCount", 0}, {"inviter", kv.count("inviter") ? kv["inviter"] : ""},
                        {"locale", glIt != groupLocales.end() ? glIt->second : std::string()},
                        {"left", kv.count("left") && kv["left"] == "1"}, {"welcome", kv.count("welcome") ? kv["welcome"] : ""},
                        {"welcome_delay", kv.count("welcome_delay") ? kv["welcome_delay"] : ""},
                        {"welcome_cooldown", kv.count("welcome_cooldown") ? kv["welcome_cooldown"] : ""}});
                }
                // Prefer a connected, still-joined account for the card summary.
                J selected = accounts.front();
                for (const auto& account : accounts)
                    if (account.value("connected", false) && !account.value("left", false)) { selected = account; break; }
                arr.push_back(J{
                    {"platform", selected.value("platform", plat)}, {"groupId", gid}, {"name", name},
                    {"enabled", selected.value("enabled", true)}, {"ai_enabled", selected.value("ai_enabled", true)},
                    {"locked", selected.value("locked", false)}, {"card", selected.value("card", std::string())},
                    {"remark", kv.count("remark") ? kv["remark"] : ""},
                    {"activeLog", selected.value("activeLog", false)}, {"observers", selected.value("observers", 0)},
                    {"botRole", selected.value("botRole", std::string())}, {"memberCount", selected.value("memberCount", 0)},
                    {"inviter", selected.value("inviter", std::string())}, {"locale", selected.value("locale", std::string())},
                    {"left", std::all_of(accounts.begin(), accounts.end(), [](const J& x){ return x.value("left", false); })},
                    {"welcome", selected.value("welcome", std::string())},
                    {"welcome_delay", selected.value("welcome_delay", std::string())},
                    {"welcome_cooldown", selected.value("welcome_cooldown", std::string())},
                    {"welcome_min_delay", cfg.get<int>("events/welcome_min_delay", 0)},
                    {"welcome_min_cooldown", cfg.get<int>("events/welcome_min_cooldown", 0)},
                    {"bindings", groupBindings.count(gid) ? groupBindings[gid] : J::array()}, {"accounts", accounts}
                });
            }
            jsonReply(ok(arr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    // PUT/DELETE /api/groups/{platform}/{groupId}
    app.registerHandler("/api/groups/{1}/{2}",
        [st, &adapterMgr, &cfg](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            J j = J::object();
            if (!req->body().empty()) j = J::parse(req->body());
            const std::string adapterId = j.value("adapterId", std::string());
            const std::string endpointId = j.value("endpointId", gid);
            if (req->method() == drogon::Delete) {
                if (!adapterId.empty()) {
                    st->remove_all<GroupAccountSettingRow>(orm::where(
                        orm::c(&GroupAccountSettingRow::adapterId) == adapterId and
                        orm::c(&GroupAccountSettingRow::groupId) == gid));
                    agsSet(st, adapterId, plat, gid, endpointId, "__removed", "1");
                } else {
                    st->remove_all<GroupSettingRow>(orm::where(
                        orm::c(&GroupSettingRow::platform) == plat and
                        orm::c(&GroupSettingRow::groupId) == gid));
                    st->remove_all<GroupAccountSettingRow>(orm::where(
                        orm::c(&GroupAccountSettingRow::groupId) == gid));
                    st->remove_all<LocaleSettingRow>(orm::where(
                        orm::c(&LocaleSettingRow::scope) == std::string("group") and
                        orm::c(&LocaleSettingRow::scopeKey) == plat + ":" + gid));
                    gsSet(st, plat, gid, "__removed", "1");
                }
                jsonReply(ok(nullptr), std::move(cb));
                return;
            }
            auto a = !adapterId.empty() ? adapterMgr.getAdapter(adapterId) : pickAdapter(adapterMgr, plat);
            auto setAccount = [&](const std::string& key, const std::string& value) {
                if (!adapterId.empty()) agsSet(st, adapterId, plat, gid, endpointId, key, value);
                else gsSet(st, plat, gid, key, value);
            };
            if (j.contains("enabled")) setAccount("enabled", j["enabled"].get<bool>() ? "1" : "0");
            if (j.contains("ai_enabled")) setAccount("aiEnabled", j["ai_enabled"].get<bool>() ? "1" : "0");
            if (j.contains("locked")) setAccount("locked", j["locked"].get<bool>() ? "1" : "0");
            if (j.contains("remark"))  gsSet(st, plat, gid, "remark", j["remark"].get<std::string>());
            if (j.contains("card")) {
                std::string card = j["card"].get<std::string>();
                setAccount("card", card);
                if (a) a->setGroupCard(endpointId, a->getLoginId(), card);
            }
            if (j.contains("name") && a) a->setGroupName(endpointId, j["name"].get<std::string>());
            if (j.contains("welcome")) setAccount("welcome", j["welcome"].get<std::string>());
            if (j.contains("welcome_delay")) {
                std::string sv = j["welcome_delay"].get<std::string>();
                if (sv.empty()) { setAccount("welcome_delay", ""); }
                else { int val = std::stoi(sv); int mn = cfg.get<int>("events/welcome_min_delay", 0); if (val > 0 && val < mn) val = mn; setAccount("welcome_delay", std::to_string(val)); }
            }
            if (j.contains("welcome_cooldown")) {
                std::string sv = j["welcome_cooldown"].get<std::string>();
                if (sv.empty()) { setAccount("welcome_cooldown", ""); }
                else { int val = std::stoi(sv); int mn = cfg.get<int>("events/welcome_min_cooldown", 0); if (val > 0 && val < mn) val = mn; setAccount("welcome_cooldown", std::to_string(val)); }
            }
            // 网页端设置本群回复语言（写 locale_settings，Resolver 直读 DB 即时生效；空串=清除覆盖）。
            if (j.contains("locale")) {
                std::string lc = j["locale"].get<std::string>();
                if (!adapterId.empty()) { setAccount("locale", lc); }
                std::string key = plat + ":" + gid;
                if (!adapterId.empty()) { jsonReply(ok(nullptr), std::move(cb)); return; }
                st->remove_all<LocaleSettingRow>(
                    orm::where(orm::c(&LocaleSettingRow::scope) == std::string("group")
                        and orm::c(&LocaleSettingRow::scopeKey) == key));
                if (!lc.empty()) {
                    LocaleSettingRow r; r.scope = "group"; r.scopeKey = key; r.locale = lc;
                    st->insert(r);
                }
            }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Delete});

    // POST /api/groups/{platform}/{groupId}/action  {action: leave|message, text?}
    app.registerHandler("/api/groups/{1}/{2}/action",
        [st, &adapterMgr](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            auto j = J::parse(req->body());
            std::string action = j.value("action", "");
            std::string adapterId = j.value("adapterId", std::string());
            std::string endpointId = j.value("endpointId", gid);
            auto a = !adapterId.empty() ? adapterMgr.getAdapter(adapterId) : pickAdapter(adapterMgr, plat);
            if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
            if (action == "leave") {
                a->leaveGroup(endpointId);
                if (!adapterId.empty()) agsSet(st, adapterId, plat, gid, endpointId, "left", "1");
                else gsSet(st, plat, gid, "left", "1");
            }
            else if (action == "message") a->sendGroupMessage(endpointId, j.value("text", ""));
            else { jsonReply(fail("unknown action"), std::move(cb)); return; }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // GET /api/groups/{platform}/{groupId}/members — member list + bot's role.
    app.registerHandler("/api/groups/{1}/{2}/members",
        [&adapterMgr](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            std::string adapterId = req->getParameter("adapterId");
            std::string endpointId = req->getParameter("endpointId");
            if (endpointId.empty()) endpointId = gid;
            auto a = !adapterId.empty() ? adapterMgr.getAdapter(adapterId) : pickAdapter(adapterMgr, plat);
            if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
            a->refreshMembers(endpointId);
            J members = a->getMembers(endpointId), out = J::array();
            for (auto& m : members) {
                std::string title = m.value("title", std::string());
                if (title.empty()) title = m.value("special_title", std::string());
                out.push_back(J{
                    {"userId", idStr(m, "user_id")},
                    {"nickname", m.value("nickname", std::string())},
                    {"card", m.value("card", std::string())},
                    {"role", m.value("role", std::string("member"))},
                    {"title", title}
                });
            }
            jsonReply(ok(J{{"botRole", a->getSelfRole(endpointId)}, {"members", out}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // ── 群文件 ──────────────────────────────────────────
    // GET /api/groups/{p}/{g}/files[?folder=<id>] — 根目录或某文件夹的文件/子文件夹。
    // 经适配器同步调用 get_group_root_files / get_group_files_by_folder（NapCat/go-cqhttp）。
    app.registerHandler("/api/groups/{1}/{2}/files",
        [&adapterMgr](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            auto a = pickAdapter(adapterMgr, plat);
            if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
            std::string folder = req->getParameter("folder");
            long long gidNum = 0; try { gidNum = std::stoll(gid); } catch (...) {}
            J resp = folder.empty()
                ? a->invokeAction("get_group_root_files", J{{"group_id", gidNum}})
                : a->invokeAction("get_group_files_by_folder", J{{"group_id", gidNum}, {"folder_id", folder}});
            if (!resp.is_object() || !resp.contains("data") || !resp["data"].is_object()) {
                jsonReply(fail("group files unavailable (adapter timeout or unsupported)"), std::move(cb)); return;
            }
            const J& d = resp["data"];
            J files = J::array(), folders = J::array();
            if (d.contains("files") && d["files"].is_array())
                for (auto& f : d["files"]) {
                    if (!f.is_object()) continue;
                    files.push_back(J{
                        {"fileId", f.contains("file_id") ? (f["file_id"].is_string() ? f["file_id"].get<std::string>() : f["file_id"].dump()) : ""},
                        {"name", f.value("file_name", std::string())},
                        {"size", f.contains("file_size") ? (f["file_size"].is_number() ? f["file_size"].get<long long>() : 0) : 0},
                        {"busid", f.contains("busid") && f["busid"].is_number() ? f["busid"].get<long long>() : 0},
                        {"uploadTime", f.contains("upload_time") && f["upload_time"].is_number() ? f["upload_time"].get<long long>() : 0},
                        {"uploader", f.contains("uploader") ? (f["uploader"].is_string() ? f["uploader"].get<std::string>() : (f["uploader"].is_number() ? std::to_string(f["uploader"].get<long long>()) : "")) : ""},
                        {"uploaderName", f.value("uploader_name", std::string())},
                        {"downloadTimes", f.contains("download_times") && f["download_times"].is_number() ? f["download_times"].get<long long>() : 0}
                    });
                }
            if (d.contains("folders") && d["folders"].is_array())
                for (auto& f : d["folders"]) {
                    if (!f.is_object()) continue;
                    folders.push_back(J{
                        {"folderId", f.contains("folder_id") ? (f["folder_id"].is_string() ? f["folder_id"].get<std::string>() : f["folder_id"].dump()) : ""},
                        {"name", f.value("folder_name", std::string())},
                        {"count", f.contains("total_file_count") && f["total_file_count"].is_number() ? f["total_file_count"].get<long long>() : 0}
                    });
                }
            jsonReply(ok(J{{"files", files}, {"folders", folders}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // 取群文件直链（get_group_file_url）。@return 空=不可用。
    auto groupFileUrl = [&adapterMgr](const std::string& plat, const std::string& gid,
                                      const std::string& fileId, const std::string& busid) -> std::string {
        auto a = pickAdapter(adapterMgr, plat);
        if (!a || fileId.empty()) return "";
        long long gidNum = 0; try { gidNum = std::stoll(gid); } catch (...) {}
        J p{{"group_id", gidNum}, {"file_id", fileId}};
        if (!busid.empty()) { try { p["busid"] = std::stoll(busid); } catch (...) {} }
        J resp = a->invokeAction("get_group_file_url", p);
        if (resp.is_object() && resp.contains("data") && resp["data"].is_object())
            return resp["data"].value("url", std::string());
        return "";
    };

    // GET /api/groups/{p}/{g}/file-url?file_id=..&busid=.. — 文件下载直链。
    app.registerHandler("/api/groups/{1}/{2}/file-url",
        [groupFileUrl](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            std::string url = groupFileUrl(plat, gid, req->getParameter("file_id"), req->getParameter("busid"));
            if (url.empty()) { jsonReply(fail("file url unavailable"), std::move(cb)); return; }
            jsonReply(ok(J{{"url", url}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // GET /api/groups/{p}/{g}/file-download?file_id=..&busid=..&name=.. — 下载代理。
    // 直链的路径是 file_id（下载得到乱码文件名），经本端中转后用 Content-Disposition
    // 还原真实文件名。文件先 curl 到临时目录再作为附件回给浏览器（残留 1 小时后清理）。
    app.registerHandler("/api/groups/{1}/{2}/file-download",
        [groupFileUrl](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            std::string url = groupFileUrl(plat, gid, req->getParameter("file_id"), req->getParameter("busid"));
            if (url.empty()) { jsonReply(fail("file url unavailable"), std::move(cb)); return; }
            namespace fs = std::filesystem;
            fs::create_directories("data/tmp");
            try {   // 清理 1 小时前的残留临时件
                auto now = fs::file_time_type::clock::now();
                for (auto& e : fs::directory_iterator("data/tmp")) {
                    if (!e.is_regular_file()) continue;
                    if (now - e.last_write_time() > std::chrono::hours(1)) { std::error_code ec; fs::remove(e.path(), ec); }
                }
            } catch (...) {}
            static std::atomic<long> s_dlSeq{0};
            std::string tmp = "data/tmp/gf_" + std::to_string((long long)std::time(nullptr))
                            + "_" + std::to_string(++s_dlSeq) + ".bin";
            {   // curl 配置文件传 URL（防 shell 注入；同 AI 网关手法）
                std::string cfgPath = tmp + ".curl";
                std::ofstream cf(cfgPath, std::ios::binary);
                auto esc = [](const std::string& s) {
                    std::string o;
                    for (char c : s) { if (c == '\n' || c == '\r') continue; if (c == '"' || c == '\\') o += '\\'; o += c; }
                    return o;
                };
                cf << "url = \"" << esc(url) << "\"\noutput = \"" << esc(tmp) << "\"\n"
                   << "location\nsilent\nmax-time = 300\n";
                cf.close();
                logsvc::runCapture("curl -K \"" + cfgPath + "\"");
                std::error_code ec; fs::remove(cfgPath, ec);
            }
            std::error_code ec;
            if (!fs::exists(tmp, ec) || fs::file_size(tmp, ec) == 0) {
                jsonReply(fail("download failed"), std::move(cb)); return;
            }
            std::string name = req->getParameter("name");
            if (name.empty()) name = "groupfile.bin";
            // Content-Disposition：ASCII 回退 + RFC 5987 filename*（UTF-8 百分号编码）。
            auto pct = [](const std::string& s) {
                static const char* hex = "0123456789ABCDEF"; std::string o;
                for (unsigned char c : s) {
                    if (std::isalnum(c) || c == '.' || c == '-' || c == '_') o += (char)c;
                    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
                }
                return o;
            };
            std::string ascii;
            for (char c : name) ascii += ((unsigned char)c >= 0x20 && (unsigned char)c < 0x80 && c != '"' && c != '\\') ? c : '_';
            auto fileResp = drogon::HttpResponse::newFileResponse(tmp);
            fileResp->setContentTypeString("application/octet-stream");
            fileResp->addHeader("Content-Disposition",
                "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + pct(name));
            cb(fileResp);
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get});

    // POST /api/groups/{p}/{g}/file-upload — 网页上传文件到群文件。
    // Body: { name, content(dataURL 或裸 base64) }。经适配器 upload_group_file
    //（本地路径优先，失败自动回退 base64://，见 onebot_v11_adapter）。
    app.registerHandler("/api/groups/{1}/{2}/file-upload",
        [&adapterMgr](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            auto j = J::parse(req->body());
            std::string name = j.value("name", std::string());
            std::string content = j.value("content", std::string());
            if (name.empty() || content.empty()) { jsonReply(fail("name/content required"), std::move(cb)); return; }
            // 防路径穿越：只留 basename
            if (auto p = name.find_last_of("/\\"); p != std::string::npos) name = name.substr(p + 1);
            // dataURL 前缀剥离
            if (auto comma = content.find(";base64,"); comma != std::string::npos) content = content.substr(comma + 8);
            else if (auto c2 = content.find(','); c2 != std::string::npos && content.rfind("data:", 0) == 0) content = content.substr(c2 + 1);
            std::string bytes = drogon::utils::base64Decode(content);
            if (bytes.empty()) { jsonReply(fail("empty/invalid file"), std::move(cb)); return; }
            if (bytes.size() > 30 * 1024 * 1024) { jsonReply(fail("file too large (>30MB)"), std::move(cb)); return; }
            auto a = pickAdapter(adapterMgr, plat);
            if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
            a->uploadGroupFile(gid, name, bytes);
            jsonReply(ok(J{{"queued", true}, {"name", name}, {"size", bytes.size()}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // POST /api/groups/{platform}/{groupId}/member-action  {action,userId,...}
    app.registerHandler("/api/groups/{1}/{2}/member-action",
        [&adapterMgr, &i18n](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            auto j = J::parse(req->body());
            std::string adapterId = j.value("adapterId", std::string());
            std::string endpointId = j.value("endpointId", gid);
            auto a = !adapterId.empty() ? adapterMgr.getAdapter(adapterId) : pickAdapter(adapterMgr, plat);
            if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
            std::string action = j.value("action", ""), uid = j.value("userId", "");
            if (uid.empty()) { jsonReply(fail("userId required"), std::move(cb)); return; }
            // 自身权限不足（原版 strSelfPermissionErr）：骰子非管理员时拒绝管理类操作。
            if (action == "ban" || action == "unban" || action == "kick" ||
                action == "card" || action == "title") {
                std::string selfRole = a->getSelfRole(endpointId);
                if (selfRole == "member") {
                    jsonReply(fail(i18n.tr(localeFromString("zh-Hans"), "gate.self_perm")), std::move(cb));
                    return;
                }
            }
            if (action == "ban")        a->setGroupBan(endpointId, uid, j.value("duration", 600));
            else if (action == "unban") a->setGroupBan(endpointId, uid, 0);
            else if (action == "kick")  a->setGroupKick(endpointId, uid);
            else if (action == "card")  a->setGroupCard(endpointId, uid, j.value("card", std::string()));
            else if (action == "title") a->setGroupSpecialTitle(endpointId, uid, j.value("title", std::string()));
            else { jsonReply(fail("unknown action"), std::move(cb)); return; }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // QQ 官方机器人 2.0 群管理：禁言、用户入群审批与自动审批策略。
    // OpenID 是适配器账号级标识，必须使用当前群详情所选账号的 endpointId。
    app.registerHandler("/api/groups/{1}/{2}/qq-official-admin",
        [&adapterMgr](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            const J posted = req->method() == drogon::Get ? J::object() : J::parse(req->body(), nullptr, false);
            if (req->method() != drogon::Get && !posted.is_object()) {
                jsonReply(fail("invalid JSON request"), std::move(cb)); return;
            }
            const std::string adapterId = req->method() == drogon::Get
                ? req->getParameter("adapterId") : posted.value("adapterId", std::string());
            auto a = !adapterId.empty() ? adapterMgr.getAdapter(adapterId) : pickAdapter(adapterMgr, plat);
            if (!a || a->platform() != "qq_official") {
                jsonReply(fail("QQ Official adapter required"), std::move(cb)); return;
            }
            const std::string endpointId = req->method() == drogon::Get
                ? req->getParameter("endpointId") : posted.value("endpointId", gid);
            J params = posted;
            params["groupId"] = endpointId.empty() ? gid : endpointId;
            std::string action;
            if (req->method() == drogon::Get) {
                const std::string section = req->getParameter("section");
                if (section == "mute") action = "qq_get_mute";
                else if (section == "requests") action = "qq_join_requests";
                else if (section == "strategies") action = "qq_list_join_strategies";
                else { jsonReply(fail("unknown section"), std::move(cb)); return; }
                const std::string cursor = req->getParameter("cursor");
                if (!cursor.empty()) params["cursor"] = cursor;
                const std::string limit = req->getParameter("limit");
                if (!limit.empty()) { try { params["limit"] = (std::min)(100, (std::max)(1, std::stoi(limit))); } catch (...) {} }
            } else {
                const std::string op = posted.value("action", std::string());
                if (op == "setMute") action = "qq_set_mute";
                else if (op == "approveJoin") action = "qq_approve_join";
                else if (op == "createStrategy") action = "qq_create_join_strategy";
                else if (op == "updateStrategy") action = "qq_update_join_strategy";
                else if (op == "deleteStrategy") action = "qq_delete_join_strategy";
                else if (op == "executeStrategy") action = "qq_execute_join_strategy";
                else if (op == "updateWhitelist") action = "qq_update_join_whitelist";
                else { jsonReply(fail("unknown action"), std::move(cb)); return; }
            }
            a->invokeActionAsync(action, params, [cb = std::move(cb)](J result) mutable {
                if (!result.is_object()) { jsonReply(fail("QQ Official action returned no result"), std::move(cb)); return; }
                if (!result.value("ok", false)) {
                    jsonReply(fail(result.value("message", std::string("QQ Official action failed"))), std::move(cb));
                    return;
                }
                jsonReply(ok(result.value("data", J::object())), std::move(cb));
            });
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});
    // GET/POST /api/groups/{platform}/{groupId}/messages — simulated chat window.
    app.registerHandler("/api/groups/{1}/{2}/messages",
        [&adapterMgr, &db](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            std::string key = plat + ":" + gid;
            if (req->method() == drogon::Post) {
                DICE_LOG_INFO("Web simulated group chat: platform={}, group={}, bodySize={}", plat, gid, req->body().size());
                auto j = J::parse(req->body(), nullptr, false);
                if (!j.is_object()) { jsonReply(fail("invalid JSON request"), std::move(cb)); return; }
                std::string text = j.value("text", "");
                std::string adapterAccount = j.value("adapterAccount", std::string());
                std::string endpointId = j.value("endpointId", std::string());
                if (text.empty()) { jsonReply(fail("empty"), std::move(cb)); return; }
                auto a = pickAdapter(adapterMgr, plat, adapterAccount);
                if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
                // QQ Official bindings are keyed by AppID, while the WebUI
                // sends the adapter's internal id.  Always translate to AppID
                // so the identity-endpoint lookup can match.
                if (plat == "qq_official") {
                    if (auto official = std::dynamic_pointer_cast<QQOfficialAdapter>(a)) adapterAccount = official->appId();
                }
                const auto targetId = selectedTransportEndpoint(db, gid, plat, adapterAccount, endpointId,
                                                                identity::Kind::Group);
                if (targetId.empty()) {
                    jsonReply(fail("selected adapter is not bound to this group"), std::move(cb));
                    return;
                }
                a->sendGroupMessage(targetId, text);
                std::string me = a->getLoginName().empty() ? std::string("\xe9\xaa\xb0\xe5\xa8\x98") : a->getLoginName();  // 骰娘
                GroupChatLog::instance().add(key, me, a->getLoginId(), text, true);
                // 网页发送的消息也持久化。
                if (auto* cst = db.getChatStorage()) {
                    try {
                        ChatMsgRow r; r.platform = plat; r.groupId = gid;
                        r.userId = a->getLoginId(); r.sender = me; r.content = text;
                        r.self = 1; r.time = static_cast<int64_t>(std::time(nullptr));
                        cst->insert(r);
                    } catch (...) {}
                }
                jsonReply(ok(nullptr), std::move(cb));
            } else {
                // 读持久化的 chat.db（重启不丢，含撤回标注），取最近 100 条按时间升序。
                auto* cst = db.getChatStorage();
                if (!cst) { jsonReply(ok(GroupChatLog::instance().recent(key, 60)), std::move(cb)); return; }
                J arr = J::array();
                try {
                    auto rows = cst->get_all<ChatMsgRow>(
                        orm::where(orm::c(&ChatMsgRow::platform) == plat
                            and orm::c(&ChatMsgRow::groupId) == gid),
                        orm::order_by(&ChatMsgRow::id).desc(), orm::limit(100));
                    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                        arr.push_back(J{{"id", it->id}, {"sender", it->sender}, {"userId", it->userId},
                                        {"content", it->content}, {"self", it->self != 0},
                                        {"time", it->time}, {"recalled", it->recalled != 0},
                                        {"msgId", it->msgId}});   // 供引用消息(CQ:reply)就地解析
                } catch (...) {}
                jsonReply(ok(arr), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    // 玩家详情里的私聊模拟聊天。复用群聊的 chat.db 行格式，但用
    // private:<用户号> 作用域区分每一位玩家；发送走适配器的私聊接口。
    app.registerHandler("/api/players/{1}/{2}/messages",
        [&adapterMgr, &db](Req req, CB&& cb, const std::string& plat, const std::string& uid) {
        try {
            const std::string scope = "private:" + uid;
            if (req->method() == drogon::Post) {
                DICE_LOG_INFO("Web simulated private chat: platform={}, user={}, bodySize={}", plat, uid, req->body().size());
                auto j = J::parse(req->body(), nullptr, false);
                if (!j.is_object()) { jsonReply(fail("invalid JSON request"), std::move(cb)); return; }
                std::string text = j.value("text", "");
                std::string adapterAccount = j.value("adapterAccount", std::string());
                std::string endpointId = j.value("endpointId", std::string());
                if (text.empty()) { jsonReply(fail("empty"), std::move(cb)); return; }
                auto a = pickAdapter(adapterMgr, plat, adapterAccount);
                if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
                if (plat == "qq_official") {
                    if (auto official = std::dynamic_pointer_cast<QQOfficialAdapter>(a)) adapterAccount = official->appId();
                }
                const auto targetId = selectedTransportEndpoint(db, uid, plat, adapterAccount, endpointId,
                                                                identity::Kind::User);
                if (targetId.empty()) {
                    jsonReply(fail("selected adapter is not bound to this user"), std::move(cb));
                    return;
                }
                a->sendPrivateMessage(targetId, text);
                if (auto* cst = db.getChatStorage()) {
                    try {
                        ChatMsgRow r; r.platform = plat; r.groupId = scope;
                        r.userId = a->getLoginId();
                        r.sender = a->getLoginName().empty() ? std::string("\xe9\xaa\xb0\xe5\xa8\x98") : a->getLoginName();
                        r.content = text; r.self = 1; r.time = static_cast<int64_t>(std::time(nullptr));
                        cst->insert(r);
                    } catch (...) {}
                }
                jsonReply(ok(nullptr), std::move(cb));
            } else {
                J arr = J::array();
                if (auto* cst = db.getChatStorage()) {
                    try {
                        auto rows = cst->get_all<ChatMsgRow>(
                            orm::where(orm::c(&ChatMsgRow::platform) == plat
                                and orm::c(&ChatMsgRow::groupId) == scope),
                            orm::order_by(&ChatMsgRow::id).desc(), orm::limit(100));
                        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                            arr.push_back(J{{"id", it->id}, {"sender", it->sender}, {"userId", it->userId},
                                            {"content", it->content}, {"self", it->self != 0},
                                            {"time", it->time}, {"recalled", it->recalled != 0},
                                            {"msgId", it->msgId}});
                    } catch (...) {}
                }
                jsonReply(ok(arr), std::move(cb));
            }
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Post});

    // 模拟聊天发送戳一戳（NapCat group_poke）。
    app.registerHandler("/api/groups/{1}/{2}/poke",
        [&adapterMgr](Req req, CB&& cb, const std::string& plat, const std::string& gid) {
        try {
            auto j = J::parse(req->body());
            std::string uid = j.value("userId", std::string());
            if (uid.empty()) { jsonReply(fail("userId required"), std::move(cb)); return; }
            auto a = pickAdapter(adapterMgr, plat);
            if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
            a->sendGroupPoke(gid, uid);
            jsonReply(ok(J{{"poked", uid}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // 向平台（NapCat）拉取本群历史消息 → 异步经 kGroupHistory 事件去重入库。
    app.registerHandler("/api/groups/{1}/{2}/fetch-history",
        [&adapterMgr](Req, CB&& cb, const std::string& plat, const std::string& gid) {
        auto a = pickAdapter(adapterMgr, plat);
        if (!a) { jsonReply(fail("no connected adapter"), std::move(cb)); return; }
        a->requestGroupHistory(gid, 50);
        jsonReply(ok(J{{"requested", true}}), std::move(cb));
    }, {drogon::Post});

    // 用户群设置（group=群号，enforce=群内无人在用户群时自动退群，invite=新好友私聊邀请）。
    app.registerHandler("/api/system/user-group", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("group"))   cfg.set<std::string>("dice/user_group", j["group"].get<std::string>());
                if (j.contains("enforce")) cfg.set<bool>("dice/user_group_enforce", j["enforce"].get<bool>());
                if (j.contains("invite"))  cfg.set<bool>("dice/user_group_invite", j["invite"].get<bool>());
                cfg.save();
            }
            jsonReply(ok(J{
                {"group", cfg.get<std::string>("dice/user_group", std::string())},
                {"enforce", cfg.get<bool>("dice/user_group_enforce", false)},
                {"invite", cfg.get<bool>("dice/user_group_invite", true)}
            }), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // 自动清理好友天数（0 = 关闭）。
    app.registerHandler("/api/system/friend-clean", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("days")) {
                    int d = j["days"].get<int>();
                    if (d < 0) d = 0; if (d > 3650) d = 3650;
                    cfg.set<int>("dice/friend_clean_days", d);
                    cfg.save();
                }
            }
            jsonReply(ok(J{{"days", cfg.get<int>("dice/friend_clean_days", 0)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put, drogon::Get});

    // 玩家管理页「删除好友」。官方 QQ 没有该接口；绑定为真实 QQ 后只允许
    // 通过已验证好友关系的 OneBot 适配器操作，避免 OpenID 伪绑定越权。
    app.registerHandler("/api/players/{1}/{2}/delete-friend",
        [&adapterMgr](Req, CB&& cb, const std::string& plat, const std::string& uid) {
        auto canDelete = [&](const AdapterPtr& a) {
            return a && a->isConnected() && a->capabilities().value("friend_delete", false);
        };
        AdapterPtr a;
        if (plat == "qq_official") {
            if (!identity::BindingStore::isRealQQ(uid)) {
                jsonReply(fail("QQ Official user is not bound to a real QQ friend"), std::move(cb)); return;
            }
            for (auto& candidate : adapterMgr.allAdapters()) {
                if (!canDelete(candidate) || candidate->platform() != "onebot_v11") continue;
                const auto list = candidate->getFriendList();
                if (std::find(list.begin(), list.end(), uid) != list.end()) { a = candidate; break; }
            }
            if (!a) { jsonReply(fail("bound real QQ is not a verified OneBot friend"), std::move(cb)); return; }
        } else {
            a = pickAdapter(adapterMgr, plat);
            if (!canDelete(a)) { jsonReply(fail("adapter does not support deleting friends"), std::move(cb)); return; }
            const auto list = a->getFriendList();
            if (!list.empty() && std::find(list.begin(), list.end(), uid) == list.end()) {
                jsonReply(fail("user is not a friend of this adapter"), std::move(cb)); return;
            }
        }
        a->deleteFriend(uid);
        jsonReply(ok(J{{"deleted", uid}}), std::move(cb));
    }, {drogon::Post});

    // 聊天记录保留天数（0 = 永久保留，不自动清理）。
    app.registerHandler("/api/system/chat-config", [&cfg](Req req, CB&& cb) {
        try {
            if (req->method() == drogon::Put) {
                auto j = J::parse(req->body());
                if (j.contains("retentionDays")) {
                    int d = j["retentionDays"].get<int>();
                    if (d < 0) d = 0; if (d > 3650) d = 3650;
                    cfg.set<int>("chat/retention_days", d);
                    cfg.save();
                }
            }
            jsonReply(ok(J{{"retentionDays", cfg.get<int>("chat/retention_days", 7)}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Get, drogon::Put});

    // ═══ Causal Rule Engine API ═══════════════════════════

    // List all causal rules
    app.registerHandler("/api/causal/rules", [&causalMgr](Req, CB&& cb) {
        J arr = J::array();
        for (auto& r : *causalMgr.listRules()) arr.push_back(r.toJSON());
        jsonReply(ok(arr), std::move(cb));
    }, {drogon::Get});

    // Create a causal rule
    app.registerHandler("/api/causal/rules", [&causalMgr](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            auto rule = dice::CausalRule::fromJSON(j);
            int id = causalMgr.addRule(rule);
            if (id < 0) { jsonReply(fail("failed to create rule"), std::move(cb)); return; }
            jsonReply(ok(J{{"id", id}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Update a causal rule
    app.registerHandler("/api/causal/rules/{1}", [&causalMgr](Req req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            auto j = J::parse(req->body());
            auto rule = dice::CausalRule::fromJSON(j);
            rule.id = id;
            if (!causalMgr.updateRule(id, rule)) { jsonReply(fail("rule not found or update failed"), std::move(cb)); return; }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put});

    // Delete a causal rule
    app.registerHandler("/api/causal/rules/{1}", [&causalMgr](Req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            if (!causalMgr.deleteRule(id)) { jsonReply(fail("rule not found"), std::move(cb)); return; }
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Delete});

    // Toggle a causal rule
    app.registerHandler("/api/causal/rules/{1}/toggle", [&causalMgr](Req, CB&& cb, const std::string& idStr) {
        try {
            int id = std::stoi(idStr);
            if (!causalMgr.toggleRule(id)) { jsonReply(fail("rule not found"), std::move(cb)); return; }
            auto r = causalMgr.getRuleById(id);
            jsonReply(ok(J{{"enabled", r ? r->enabled : false}}), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // Test a causal rule match (dry-run, no cooldown/counter side effects)
    app.registerHandler("/api/causal/rules/test", [&causalMgr](Req req, CB&& cb) {
        try {
            auto j = J::parse(req->body());
            std::string msg = j.value("msg", "");
            std::string userId = j.value("userId", "");
            std::string groupId = j.value("groupId", "");
            std::string nick = j.value("nick", std::string("TestUser"));
            auto result = causalMgr.matchAndExecute(msg, userId, groupId, nick, true);
            J data;
            data["matched"] = result.matched;
            data["reply"] = result.reply;
            data["ruleId"] = result.ruleId;
            data["ruleName"] = result.ruleName;
            auto changes = J::array();
            for (auto& cc : result.counterChanges) {
                changes.push_back(J{{"name", cc.name}, {"oldValue", cc.oldValue}, {"newValue", cc.newValue}});
            }
            data["counterChanges"] = changes;
            jsonReply(ok(data), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Post});

    // List all counters
    app.registerHandler("/api/counters", [&counterStore](Req, CB&& cb) {
        auto entries = counterStore.listAll();
        J arr = J::array();
        for (auto& e : entries) {
            arr.push_back(J{
                {"key", e.key}, {"value", e.value}, {"updatedAt", e.updatedAt},
                {"ruleId", e.ruleId}, {"counterName", e.counterName},
                {"scope", e.scope}, {"scopeId", e.scopeId}
            });
        }
        jsonReply(ok(arr), std::move(cb));
    }, {drogon::Get});

    // Update a counter value
    app.registerHandler("/api/counters/{1}", [&counterStore](Req req, CB&& cb, const std::string& key) {
        try {
            auto j = J::parse(req->body());
            int value = j.value("value", 0);
            counterStore.set(key, value);
            jsonReply(ok(nullptr), std::move(cb));
        } catch (const std::exception& e) { jsonReply(fail(e.what()), std::move(cb)); }
    }, {drogon::Put});

    // Reset/delete a counter
    app.registerHandler("/api/counters/{1}", [&counterStore](Req, CB&& cb, const std::string& key) {
        counterStore.reset(key);
        jsonReply(ok(nullptr), std::move(cb));
    }, {drogon::Delete});

    // Clear all cooldowns (admin utility)
    app.registerHandler("/api/causal/cooldowns/clear", [&cooldownMgr](Req, CB&& cb) {
        cooldownMgr.clearAll();
        jsonReply(ok(nullptr), std::move(cb));
    }, {drogon::Post});
}

} // namespace dice::api
