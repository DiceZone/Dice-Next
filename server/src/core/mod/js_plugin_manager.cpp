#include "core/mod/js_plugin_manager.h"
#include "common/logger.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>

namespace fs = std::filesystem;
using nlohmann::json;

namespace dice {

// 路径 → UTF-8 窄串：Windows 上 path::string() 走 ANSI 代码页，文件名含 GBK 无映射
// 字符（emoji 等）会抛 system_error（Server 2012/2016 启动崩溃根因）。u8string 永不抛。
static inline std::string dnx_u8str(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ─── helpers ─────────────────────────────────────────────────
static JsPluginManager* mgrOf(JSContext* ctx) {
    return static_cast<JsPluginManager*>(JS_GetContextOpaque(ctx));
}
static std::string toStr(JSContext* ctx, JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return out;
}
static std::string getStrProp(JSContext* ctx, JSValueConst obj, const char* key) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    std::string s = toStr(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

// Wrap @p value in an already-resolved Promise (takes ownership of value).
// Used by fetch()/Response.json()/.text(): the HTTP call is synchronous, so we
// hand back resolved promises and let drainJobs() run any `await` continuations.
static JSValue makeResolvedPromise(JSContext* ctx, JSValue value) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst*)&value);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, value);
    return promise;
}
static JSValue makeRejectedPromise(JSContext* ctx, const char* msg) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
    JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    JS_FreeValue(ctx, err);
    return promise;
}

// Response.text() → Promise<string> resolving to the stored body.
static JSValue jsRespText(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    std::string body = getStrProp(ctx, this_val, "_body");
    return makeResolvedPromise(ctx, JS_NewString(ctx, body.c_str()));
}
// Response.json() → Promise<any> resolving to JSON.parse(body) (rejects on parse error).
static JSValue jsRespJson(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    std::string body = getStrProp(ctx, this_val, "_body");
    JSValue parsed = JS_ParseJSON(ctx, body.c_str(), body.size(), "<fetch>");
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return makeRejectedPromise(ctx, "fetch: invalid JSON body");
    }
    return makeResolvedPromise(ctx, parsed);
}

// Global fetch(url, opts?) — SealDice/standard Fetch-style. Returns a resolved
// Promise of a Response { ok, status, text(), json() }. The HTTP request is done
// synchronously (blocking) via the injected, gated httpFetch (curl + whitelist).
static JSValue jsFetch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (!m || argc < 1) return makeRejectedPromise(ctx, "fetch: missing url");
    std::string url = toStr(ctx, argv[0]);
    std::string method = "GET", body, headerLines;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(mv)) method = toStr(ctx, mv);
        JS_FreeValue(ctx, mv);
        JSValue bv = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(bv)) body = toStr(ctx, bv);
        else if (JS_IsObject(bv)) {  // tolerate non-string bodies → JSON
            JSValue s = JS_JSONStringify(ctx, bv, JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsString(s)) body = toStr(ctx, s);
            JS_FreeValue(ctx, s);
        }
        JS_FreeValue(ctx, bv);
        JSValue hv = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hv)) {
            JSPropertyEnum* tab = nullptr; uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, hv, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; ++i) {
                    JSValue key = JS_AtomToString(ctx, tab[i].atom);
                    JSValue val = JS_GetProperty(ctx, hv, tab[i].atom);
                    std::string k = toStr(ctx, key), v = toStr(ctx, val);
                    if (!k.empty()) headerLines += k + ": " + v + "\n";
                    JS_FreeValue(ctx, key); JS_FreeValue(ctx, val);
                    JS_FreeAtom(ctx, tab[i].atom);
                }
                js_free(ctx, tab);
            }
        }
        JS_FreeValue(ctx, hv);
    }

    int status = 0;
    std::string resp = m->httpFetch(method, url, headerLines, body, status);
    if (status == 0) return makeRejectedPromise(ctx, "fetch: request blocked or failed (check 外置API开关/白名单)");

    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, r, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, r, "statusText", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, r, "_body", JS_NewString(ctx, resp.c_str()));
    JS_SetPropertyStr(ctx, r, "text", JS_NewCFunction(ctx, jsRespText, "text", 0));
    JS_SetPropertyStr(ctx, r, "json", JS_NewCFunction(ctx, jsRespJson, "json", 0));
    return makeResolvedPromise(ctx, r);
}

// ─── native functions exposed to JS ──────────────────────────
static JSValue jsStorageSet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
static JSValue jsStorageGet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

static JSValue jsConsoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) { if (i) line += ' '; line += toStr(ctx, argv[i]); }
    DICE_LOG_INFO("[js] {}", line);
    return JS_UNDEFINED;
}
// Extract the reply target (platform / private / group / user) from ctx(argv0)+msg(argv1).
static void replyTarget(JSContext* ctx, JSValueConst msg, JSValueConst cctx,
                        std::string& platform, bool& isPrivate, std::string& gid, std::string& uid) {
    platform = getStrProp(ctx, msg, "platform");
    if (platform.empty()) { JSValue ep = JS_GetPropertyStr(ctx, cctx, "endPoint"); platform = getStrProp(ctx, ep, "platform"); JS_FreeValue(ctx, ep); }
    isPrivate = getStrProp(ctx, msg, "messageType") == "private";
    gid = getStrProp(ctx, msg, "groupId");
    JSValue sender = JS_GetPropertyStr(ctx, msg, "sender");
    uid = getStrProp(ctx, sender, "userId");
    JS_FreeValue(ctx, sender);
    // seal.newMessage()/createTempCtx() 构造的临时消息可能没有 sender，
    // 此时以 ctx.player 为目标，符合 replyPerson 的私聊语义。
    if (uid.empty()) {
        JSValue player = JS_GetPropertyStr(ctx, cctx, "player");
        uid = getStrProp(ctx, player, "userId");
        JS_FreeValue(ctx, player);
    }
}
// Shared reply: during a message turn → accumulate (returned by handle); from a
// fired timer / async callback → send immediately via the injected sender.
static JSValue jsReplyCommon(JSContext* ctx, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (!m || argc < 3) return JS_UNDEFINED;
    std::string text = toStr(ctx, argv[2]);
    if (m->capturing()) { m->appendReply(text); return JS_UNDEFINED; }
    std::string platform, gid, uid; bool isPriv = false;
    replyTarget(ctx, argv[1], argv[0], platform, isPriv, gid, uid);
    m->routeReply(platform, isPriv, gid, uid, text);
    return JS_UNDEFINED;
}
static JSValue jsReplyToSender(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsReplyCommon(ctx, argc, argv);
}
// Read ctx.<obj>.<key> (e.g. ctx.endPoint.platform / ctx.group.groupId / ctx.player.userId).
static std::string ctxStr(JSContext* ctx, JSValueConst c, const char* obj, const char* key) {
    JSValue o = JS_GetPropertyStr(ctx, c, obj);
    std::string s = getStrProp(ctx, o, key);
    JS_FreeValue(ctx, o);
    return s;
}
// seal.memberBan(ctx, groupId, userId, duration) / seal.memberKick(ctx, groupId, userId)
static JSValue jsMemberBan(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 3) {
        int64_t dur = 0; if (argc >= 4) JS_ToInt64(ctx, &dur, argv[3]);
        m->groupAdmin(ctxStr(ctx, argv[0], "endPoint", "platform"), "ban",
                      toStr(ctx, argv[1]), toStr(ctx, argv[2]), dur, "");
    }
    return JS_UNDEFINED;
}
static JSValue jsMemberKick(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 3)
        m->groupAdmin(ctxStr(ctx, argv[0], "endPoint", "platform"), "kick",
                      toStr(ctx, argv[1]), toStr(ctx, argv[2]), 0, "");
    return JS_UNDEFINED;
}
// seal.setPlayerGroupCard(ctx, template) — set the SENDER's group card.
static JSValue jsSetPlayerGroupCard(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 2) {
        // 跳过 undefined/null/空：海豹插件常传 ctx.player.autoSetNameTemplate（玩家未设
        // 自动名片时为空），我们若直接写就会把群名片设成字面 "undefined"（实测踩坑）。
        if (JS_IsUndefined(argv[1]) || JS_IsNull(argv[1])) return JS_UNDEFINED;
        std::string card = toStr(ctx, argv[1]);
        if (card.empty() || card == "undefined") return JS_UNDEFINED;
        m->groupAdmin(ctxStr(ctx, argv[0], "endPoint", "platform"), "card",
                      ctxStr(ctx, argv[0], "group", "groupId"),
                      ctxStr(ctx, argv[0], "player", "userId"), 0, card);
    }
    return JS_UNDEFINED;
}
// seal.ban.addBan(ctx,id,place,reason) / addTrust(...) / remove(ctx,id)
static JSValue jsBanAdd(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 2) m->banOp("ban", toStr(ctx, argv[1]), argc >= 4 ? toStr(ctx, argv[3]) : "");
    return JS_UNDEFINED;
}
static JSValue jsBanAddTrust(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 2) m->banOp("trust", toStr(ctx, argv[1]), argc >= 4 ? toStr(ctx, argv[3]) : "");
    return JS_UNDEFINED;
}
static JSValue jsBanRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 2) m->banOp("remove", toStr(ctx, argv[1]), "");
    return JS_UNDEFINED;
}
// seal.ban.getList() → 黑白名单条目数组；接 CommandRouter 的 banlist（经 banQuery_ 注入）。
static JSValue jsBanGetList(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* m = mgrOf(ctx);
    std::string j = m ? m->banQuery("list", "") : "";
    if (j.empty()) j = "[]";
    return JS_ParseJSON(ctx, j.c_str(), j.size(), "<ban.getList>");
}
// seal.ban.getUser(id) → 单条（不存在返回 null）。
static JSValue jsBanGetUser(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    std::string id = argc >= 1 ? toStr(ctx, argv[0]) : "";
    std::string j = m ? m->banQuery("user", id) : "";
    if (j.empty()) j = "null";
    return JS_ParseJSON(ctx, j.c_str(), j.size(), "<ban.getUser>");
}
// Real timers — schedule the callback on the host event loop (see addTimer).
static JSValue jsSetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (!m || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    double ms = 0; if (argc >= 2) JS_ToFloat64(ctx, &ms, argv[1]);
    return JS_NewInt64(ctx, m->addTimer(JS_DupValue(ctx, argv[0]), ms / 1000.0, 0));
}
static JSValue jsSetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (!m || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    double ms = 0; if (argc >= 2) JS_ToFloat64(ctx, &ms, argv[1]);
    return JS_NewInt64(ctx, m->addTimer(JS_DupValue(ctx, argv[0]), ms / 1000.0, ms / 1000.0));
}
static JSValue jsClearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 1) { int64_t id = 0; JS_ToInt64(ctx, &id, argv[0]); m->clearTimer(id); }
    return JS_UNDEFINED;
}
// seal.ext.registerTask(ext, taskType, value, fn, key, desc, group)
// Supports daily "HH:MM" and a daily-equivalent cron "M H * * *". Built on the
// timer mechanism (a daily task = a timer that re-arms every 24h).
static JSValue jsRegisterTask(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (!m || argc < 4 || !JS_IsFunction(ctx, argv[3])) return JS_UNDEFINED;
    std::string taskType = toStr(ctx, argv[1]), value = toStr(ctx, argv[2]);
    int hh = -1, mm = 0;
    if (taskType == "daily") {
        auto c = value.find(':');
        if (c != std::string::npos) { try { hh = std::stoi(value.substr(0, c)); mm = std::stoi(value.substr(c + 1)); } catch (...) {} }
    } else if (taskType == "cron") {
        // best-effort: "M H * * *" → daily at H:M
        std::istringstream iss(value); std::vector<std::string> f; std::string t;
        while (iss >> t) f.push_back(t);
        if (f.size() == 5 && f[2] == "*" && f[3] == "*" && f[4] == "*") {
            try { mm = std::stoi(f[0]); hh = std::stoi(f[1]); } catch (...) { hh = -1; }
        }
    }
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        DICE_LOG_WARN("[js] registerTask: unsupported taskType/value '{}' '{}' (only daily HH:MM / cron 'M H * * *')", taskType, value);
        return JS_UNDEFINED;
    }
    std::time_t now = std::time(nullptr); std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    std::tm tgt = lt; tgt.tm_hour = hh; tgt.tm_min = mm; tgt.tm_sec = 0;
    double delay = difftime(std::mktime(&tgt), now);
    if (delay <= 0) delay += 86400.0;   // already passed today → tomorrow
    m->addTimer(JS_DupValue(ctx, argv[3]), delay, 86400.0);   // re-arm every 24h
    DICE_LOG_INFO("[js] registerTask scheduled daily at {:02d}:{:02d}", hh, mm);
    return JS_UNDEFINED;
}
static JSValue jsExtNew(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue ext = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ext, "name",    JS_NewString(ctx, argc > 0 ? toStr(ctx, argv[0]).c_str() : ""));
    JS_SetPropertyStr(ctx, ext, "author",  JS_NewString(ctx, argc > 1 ? toStr(ctx, argv[1]).c_str() : ""));
    JS_SetPropertyStr(ctx, ext, "version", JS_NewString(ctx, argc > 2 ? toStr(ctx, argv[2]).c_str() : ""));
    JS_SetPropertyStr(ctx, ext, "cmdMap",  JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, ext, "storageSet", JS_NewCFunction(ctx, jsStorageSet, "storageSet", 2));
    JS_SetPropertyStr(ctx, ext, "storageGet", JS_NewCFunction(ctx, jsStorageGet, "storageGet", 1));
    return ext;
}
static JSValue jsExtFind(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (m && argc >= 1) return m->findExt(toStr(ctx, argv[0]));  // 按名返回已注册 ext（跨插件协作）
    return JS_NULL;
}
static JSValue jsNewCmdItemInfo(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue cmd = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cmd, "name", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, cmd, "help", JS_NewString(ctx, ""));
    return cmd;
}
static JSValue jsNewCmdExecuteResult(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "solved",   JS_NewBool(ctx, argc > 0 ? JS_ToBool(ctx, argv[0]) : 1));
    JS_SetPropertyStr(ctx, r, "showHelp", JS_NewBool(ctx, 0));
    return r;
}
static JSValue jsCmdArgsGetArgN(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    int n = 0;
    if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
    JSValue args = JS_GetPropertyStr(ctx, this_val, "_args");
    JSValue v = (n >= 1) ? JS_GetPropertyUint32(ctx, args, (uint32_t)(n - 1)) : JS_UNDEFINED;
    std::string s = JS_IsUndefined(v) ? "" : toStr(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, args);
    return JS_NewString(ctx, s.c_str());
}
static JSValue jsCmdArgsGetRestArgsFrom(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    int n = 1; if (argc > 0) JS_ToInt32(ctx, &n, argv[0]); if (n < 1) n = 1;
    JSValue args = JS_GetPropertyStr(ctx, this_val, "_args");
    uint32_t len = 0; { JSValue l = JS_GetPropertyStr(ctx, args, "length"); JS_ToUint32(ctx, &len, l); JS_FreeValue(ctx, l); }
    std::string out;
    for (uint32_t i = (uint32_t)(n - 1); i < len; ++i) {
        JSValue v = JS_GetPropertyUint32(ctx, args, i);
        if (!out.empty()) out += " "; out += toStr(ctx, v); JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, args);
    return JS_NewString(ctx, out.c_str());
}
static JSValue jsCmdArgsIsArgEqual(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewBool(ctx, 0);
    int n = 0; JS_ToInt32(ctx, &n, argv[0]);
    JSValue args = JS_GetPropertyStr(ctx, this_val, "_args");
    JSValue av = (n >= 1) ? JS_GetPropertyUint32(ctx, args, (uint32_t)(n - 1)) : JS_UNDEFINED;
    std::string a = JS_IsUndefined(av) ? "" : toStr(ctx, av);
    JS_FreeValue(ctx, av); JS_FreeValue(ctx, args);
    bool eq = false;
    for (int i = 1; i < argc; ++i) if (a == toStr(ctx, argv[i])) { eq = true; break; }
    return JS_NewBool(ctx, eq);
}
static JSValue jsNoop(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32(ctx, 0); }
static JSValue jsNewObj(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewObject(ctx); }
// gameSystem.newTemplate(ByYaml) — 记当前插件为「规则类」，并收集模板原文（JSON/YAML）
// 供宿主解析属性别名/衍生（attribute template 集成）。返回空对象（插件后续 .x 等调用无害）。
static JSValue jsRuleTemplate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (auto* m = mgrOf(ctx)) {
        m->markCurrentRulePlugin();
        if (argc >= 1 && JS_IsString(argv[0])) {
            const char* s = JS_ToCString(ctx, argv[0]);
            if (s) { m->addGameSystemTemplate(s); JS_FreeCString(ctx, s); }
        }
    }
    return JS_NewObject(ctx);
}
// C#7：coc.registerRule — 记当前插件为「规则类」，空操作返回 0。
static JSValue jsRuleRegister(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (auto* m = mgrOf(ctx)) m->markCurrentRulePlugin();
    return JS_NewInt32(ctx, 0);
}
// 若第一个参数命中 argv 之一则将其从 _args/args 移除，返回被吃掉的前缀（否则空）。
static std::string chopFirst(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    JSValue args = JS_GetPropertyStr(ctx, self, "_args");
    uint32_t len = 0; { JSValue l = JS_GetPropertyStr(ctx, args, "length"); JS_ToUint32(ctx, &len, l); JS_FreeValue(ctx, l); }
    std::string matched;
    std::vector<std::string> all;
    for (uint32_t i = 0; i < len; ++i) { JSValue v = JS_GetPropertyUint32(ctx, args, i); all.push_back(toStr(ctx, v)); JS_FreeValue(ctx, v); }
    JS_FreeValue(ctx, args);
    if (!all.empty()) for (int i = 0; i < argc; ++i) if (all[0] == toStr(ctx, argv[i])) { matched = all[0]; break; }
    if (!matched.empty()) {
        JSValue na = JS_NewArray(ctx); std::string joined;
        for (size_t i = 1; i < all.size(); ++i) { JS_SetPropertyUint32(ctx, na, (uint32_t)(i - 1), JS_NewString(ctx, all[i].c_str())); if (!joined.empty()) joined += " "; joined += all[i]; }
        JS_SetPropertyStr(ctx, self, "_args", JS_DupValue(ctx, na));
        JS_SetPropertyStr(ctx, self, "args", na);
        JS_SetPropertyStr(ctx, self, "rawArgs", JS_NewString(ctx, joined.c_str()));
        JS_SetPropertyStr(ctx, self, "cleanArgs", JS_NewString(ctx, joined.c_str()));
    }
    return matched;
}
static JSValue jsChopPrefix(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) { return JS_NewBool(ctx, !chopFirst(ctx, self, argc, argv).empty()); }
static JSValue jsEatPrefix(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) { return JS_NewString(ctx, chopFirst(ctx, self, argc, argv).c_str()); }
// cmdArgs.getKwarg(name) — parse "--name" / "--name=value" from rawArgs. Returns
// {name, value, valueExists} like SealDice, or null if the flag isn't present.
static JSValue jsGetKwarg(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    std::string want = toStr(ctx, argv[0]);
    std::string raw = getStrProp(ctx, thisVal, "rawArgs");
    std::istringstream iss(raw);
    std::string tok;
    while (iss >> tok) {
        if (tok.rfind("--", 0) != 0) continue;
        std::string body = tok.substr(2);
        std::string key = body, val; bool hasVal = false;
        if (auto eq = body.find('='); eq != std::string::npos) { key = body.substr(0, eq); val = body.substr(eq + 1); hasVal = true; }
        if (key != want) continue;
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, key.c_str()));
        JS_SetPropertyStr(ctx, o, "value", JS_NewString(ctx, val.c_str()));
        JS_SetPropertyStr(ctx, o, "valueExists", JS_NewBool(ctx, hasVal));
        return o;
    }
    return JS_NULL;
}
// getCtxProxyFirst(ctx,cmdArgs) / getCtxProxyAtPos(ctx,cmdArgs,pos)：v1 无真实 @列表，
// 回退为传入的 ctx 自身（多数插件写 getCtxProxyFirst(...) || ctx，能正常工作）。
// getCtxProxyFirst(ctx,msg) / getCtxProxyAtPos(ctx,msg,pos): return a ctx whose
// player is the @'d user (代骰/代查). The @ list rides on ctx._at (set in buildCtxMsg);
// vars/card scope derive from player.userId, so the proxy really targets that user.
static JSValue jsGetCtxProxy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewObject(ctx);
    JSValueConst orig = argv[0];
    int pos = 0;
    if (argc >= 3) { int32_t p = 0; JS_ToInt32(ctx, &p, argv[2]); pos = p; }
    std::string uid;
    JSValue at = JS_GetPropertyStr(ctx, orig, "_at");
    if (JS_IsArray(at)) {
        uint32_t len = 0; { JSValue l = JS_GetPropertyStr(ctx, at, "length"); JS_ToUint32(ctx, &len, l); JS_FreeValue(ctx, l); }
        if (pos >= 0 && (uint32_t)pos < len) { JSValue v = JS_GetPropertyUint32(ctx, at, pos); uid = toStr(ctx, v); JS_FreeValue(ctx, v); }
    }
    JS_FreeValue(ctx, at);
    if (uid.empty()) return JS_DupValue(ctx, orig);   // no @target → fall back to self
    JSValue proxy = JS_NewObject(ctx);
    for (const char* k : {"group", "isPrivate", "privilegeLevel", "endPoint", "isCurGroupBotOn", "_at"}) {
        JSValue v = JS_GetPropertyStr(ctx, orig, k);
        if (!JS_IsUndefined(v)) JS_SetPropertyStr(ctx, proxy, k, v); else JS_FreeValue(ctx, v);
    }
    JSValue player = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, player, "userId", JS_NewString(ctx, uid.c_str()));
    JS_SetPropertyStr(ctx, player, "name", JS_NewString(ctx, uid.c_str()));
    JS_SetPropertyStr(ctx, proxy, "player", player);
    return proxy;
}
static JSValue jsCreateTempCtx(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue c = JS_NewObject(ctx);
    JSValue p = JS_NewObject(ctx); JS_SetPropertyStr(ctx, p, "userId", JS_NewString(ctx, "")); JS_SetPropertyStr(ctx, p, "name", JS_NewString(ctx, "")); JS_SetPropertyStr(ctx, c, "player", p);
    JSValue gp = JS_NewObject(ctx); JS_SetPropertyStr(ctx, gp, "groupId", JS_NewString(ctx, "")); JS_SetPropertyStr(ctx, c, "group", gp);
    return c;
}
static JSValue jsExtRegister(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (auto* m = mgrOf(ctx); m && argc > 0) m->registerExt(argv[0]);
    return JS_UNDEFINED;
}

// replyGroup 保持普通回复语义；replyPerson 必须直接投递私聊，不能混进群回复缓冲。
static JSValue jsReplyGroup(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) { return jsReplyToSender(ctx, t, argc, argv); }
static JSValue jsReplyPerson(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx);
    if (!m || argc < 3) return JS_UNDEFINED;
    std::string platform, gid, uid; bool ignoredPrivate = false;
    replyTarget(ctx, argv[1], argv[0], platform, ignoredPrivate, gid, uid);
    std::string text = toStr(ctx, argv[2]);
    if (!platform.empty() && !uid.empty() && !text.empty()) {
        m->routeReply(platform, true, "", uid, text);
        m->markSideEffectReply();
    }
    return JS_UNDEFINED;
}

// ─── base64 (atob / btoa) ────────────────────────────────────
static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64encode(const std::string& in) {
    std::string out; int val = 0, bits = -6;
    for (unsigned char c : in) { val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out.push_back(kB64[(val >> bits) & 0x3F]); bits -= 6; } }
    if (bits > -6) out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
static std::string b64decode(const std::string& in) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) T[(unsigned char)kB64[i]] = i;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) { if (T[c] == -1) break; val = (val << 6) + T[c]; bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; } }
    return out;
}
static JSValue jsBtoa(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewString(ctx, b64encode(argc > 0 ? toStr(ctx, argv[0]) : "").c_str());
}
static JSValue jsAtob(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewString(ctx, b64decode(argc > 0 ? toStr(ctx, argv[0]) : "").c_str());
}
static JSValue jsGetVersion(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "version", JS_NewString(ctx, "Dice!Next-3.0"));
    JS_SetPropertyStr(ctx, o, "versionCode", JS_NewInt32(ctx, 30000));
    return o;
}

// ─── seal.vars / setVar* / format（持久化 KV，按 $m/$g/$ 作用域）────
static std::string varKeyOf(JSContext* ctx, JSValueConst ctxObj, const std::string& name) {
    std::string n = name, scope = "G";
    if (n.rfind("$m", 0) == 0)      { scope = "m"; n = n.substr(2); }
    else if (n.rfind("$g", 0) == 0) { scope = "g"; n = n.substr(2); }
    else if (n.rfind("$", 0) == 0)  { scope = "G"; n = n.substr(1); }
    std::string uid, gid;
    JSValue player = JS_GetPropertyStr(ctx, ctxObj, "player"); uid = getStrProp(ctx, player, "userId"); JS_FreeValue(ctx, player);
    JSValue group  = JS_GetPropertyStr(ctx, ctxObj, "group");  gid = getStrProp(ctx, group, "groupId"); JS_FreeValue(ctx, group);
    if (scope == "m") return "u:" + uid + ":" + n;
    if (scope == "g") return "g:" + gid + ":" + n;
    return "G:" + n;
}
static JSValue pairVal(JSContext* ctx, JSValue v, bool ok) {
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, v);
    JS_SetPropertyUint32(ctx, a, 1, JS_NewBool(ctx, ok));
    return a;
}
// 从 ctx 取 platform / userId / groupId（人物卡桥接用）。
static void ctxIds(JSContext* ctx, JSValueConst ctxObj, std::string& plat, std::string& uid, std::string& gid) {
    JSValue player = JS_GetPropertyStr(ctx, ctxObj, "player"); uid = getStrProp(ctx, player, "userId"); JS_FreeValue(ctx, player);
    JSValue group  = JS_GetPropertyStr(ctx, ctxObj, "group");  gid = getStrProp(ctx, group, "groupId"); JS_FreeValue(ctx, group);
    JSValue ep = JS_GetPropertyStr(ctx, ctxObj, "endPoint");   plat = getStrProp(ctx, ep, "platform"); JS_FreeValue(ctx, ep);
}
// 「无 $ 前缀」= 人物卡属性（海豹语义）；有 $ = 个人/群/全局/临时变量（走 KV）。
static bool isCardAttrName(const std::string& name) { return !name.empty() && name[0] != '$'; }

static JSValue jsVarIntGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return pairVal(ctx, JS_NewInt32(ctx, 0), false);
    std::string name = toStr(ctx, argv[1]);
    if (isCardAttrName(name) && m->hasCardBridge()) {   // 桥接到人物卡
        std::string plat, uid, gid; ctxIds(ctx, argv[0], plat, uid, gid);
        long long out = 0;
        bool ok = m->cardGet(plat, uid, gid, name, out);
        return pairVal(ctx, JS_NewInt64(ctx, out), ok);
    }
    std::string raw = m->kvGet(varKeyOf(ctx, argv[0], name));
    try { return pairVal(ctx, JS_NewInt64(ctx, std::stoll(raw)), !raw.empty()); }
    catch (...) { return pairVal(ctx, JS_NewInt32(ctx, 0), false); }
}
static JSValue jsVarStrGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return pairVal(ctx, JS_NewString(ctx, ""), false);
    std::string name = toStr(ctx, argv[1]);
    // 常用临时变量：$t玩家 / $t玩家_RAW 即玩家昵称（海豹内置），让插件回复带上名字。
    if (name == "$t\xe7\x8e\xa9\xe5\xae\xb6" || name == "$t\xe7\x8e\xa9\xe5\xae\xb6_RAW") {  // $t玩家 / $t玩家_RAW
        JSValue player = JS_GetPropertyStr(ctx, argv[0], "player");
        std::string nick = getStrProp(ctx, player, "name"); JS_FreeValue(ctx, player);
        return pairVal(ctx, JS_NewString(ctx, nick.c_str()), !nick.empty());
    }
    // C#37：无 $ 前缀名先看人物卡的「关联/表达式属性」(.st 物防='dex+1')，让海豹规则
    // 插件 strGet 读到原文表达式（如最终物语据此联动算物防）；没有再走 KV。
    if (isCardAttrName(name) && m->hasCardStrBridge()) {
        std::string plat, uid, gid; ctxIds(ctx, argv[0], plat, uid, gid);
        std::string sv;
        if (m->cardGetStr(plat, uid, gid, name, sv))
            return pairVal(ctx, JS_NewString(ctx, sv.c_str()), true);
    }
    std::string key = varKeyOf(ctx, argv[0], name);
    std::string raw = m->kvGet(key, "\x01");
    bool ok = raw != "\x01"; if (!ok) raw = "";
    return pairVal(ctx, JS_NewString(ctx, raw.c_str()), ok);
}
static JSValue jsVarIntSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (m && argc >= 3) {
        int64_t v = 0; JS_ToInt64(ctx, &v, argv[2]);
        std::string name = toStr(ctx, argv[1]);
        if (isCardAttrName(name) && m->hasCardBridge()) {   // 桥接到人物卡
            std::string plat, uid, gid; ctxIds(ctx, argv[0], plat, uid, gid);
            m->cardSet(plat, uid, gid, name, v);
        } else {
            m->kvSet(varKeyOf(ctx, argv[0], name), std::to_string(v));
        }
    }
    return JS_UNDEFINED;
}
static JSValue jsVarStrSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (m && argc >= 3) m->kvSet(varKeyOf(ctx, argv[0], toStr(ctx, argv[1])), toStr(ctx, argv[2]));
    return JS_UNDEFINED;
}
static JSValue jsFormat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_NewString(ctx, "");
    JSValueConst ctxObj = argv[0];
    std::string s = toStr(ctx, argv[1]);
    auto* m = mgrOf(ctx);
    std::string out; size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t end = s.find('}', i);
            if (end != std::string::npos) {
                std::string inner = s.substr(i + 1, end - i - 1);
                if (inner.rfind("$t", 0) == 0) out += s.substr(i, end - i + 1);    // 临时变量：原样保留
                else if (!inner.empty() && inner[0] == '$') out += (m ? m->kvGet(varKeyOf(ctx, ctxObj, inner)) : "");  // 个人/群/全局变量
                else if (m) out += m->evalDice(inner);                              // 表达式：交骰子引擎求值
                else out += inner;
                i = end + 1; continue;
            }
        }
        out += s[i++];
    }
    return JS_NewString(ctx, out.c_str());
}

// ─── ext.storage / 配置项（用持久化 KV，按 ext.name 命名空间）──────
static std::string extStorageKey(JSContext* ctx, JSValueConst extObj, const std::string& key) {
    return "ext:" + getStrProp(ctx, extObj, "name") + ":" + key;
}
static JSValue jsStorageSet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (m && argc >= 2) m->kvSet(extStorageKey(ctx, this_val, toStr(ctx, argv[0])), toStr(ctx, argv[1]));
    return JS_UNDEFINED;
}
static JSValue jsStorageGet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 1) return JS_NewString(ctx, "");
    return JS_NewString(ctx, m->kvGet(extStorageKey(ctx, this_val, toStr(ctx, argv[0]))).c_str());
}

// ─── 配置项 register*Config / get*Config（kv 持久化，cfg:<ext>:<key>）────
static std::string cfgKey(JSContext* ctx, JSValueConst extObj, const std::string& key) {
    return "cfg:" + getStrProp(ctx, extObj, "name") + ":" + key;
}
static std::string jsArrToJson(JSContext* ctx, JSValueConst arr) {
    json j = json::array();
    uint32_t len = 0; JSValue l = JS_GetPropertyStr(ctx, arr, "length"); JS_ToUint32(ctx, &len, l); JS_FreeValue(ctx, l);
    for (uint32_t i = 0; i < len; ++i) { JSValue v = JS_GetPropertyUint32(ctx, arr, i); j.push_back(toStr(ctx, v)); JS_FreeValue(ctx, v); }
    return j.dump();
}
static JSValue jsonToArr(JSContext* ctx, const std::string& s) {
    JSValue a = JS_NewArray(ctx);
    try { json j = json::parse(s); if (j.is_array()) { uint32_t i = 0;
        for (auto& e : j) JS_SetPropertyUint32(ctx, a, i++, JS_NewString(ctx, e.is_string() ? e.get<std::string>().c_str() : e.dump().c_str())); } }
    catch (...) {}
    return a;
}
// 若该 key 尚无值则写入默认值。
static void cfgDefault(JsPluginManager* m, const std::string& k, const std::string& def) {
    if (m->kvGet(k, "\x01") == "\x01") m->kvSet(k, def);
}
// 注册一个配置项：写默认值（若无） + 记录元数据（供 WebUI 渲染输入框）。
// SealDice 调用约定：register*Config(ext, key, default, [description])；
// option 为 register*Config(ext, key, default, optionsArray, [description])。
static void regCfg(JSContext* ctx, int argc, JSValueConst* argv,
                   const char* type, const std::string& def, const std::string& optionsJson = "") {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return;
    std::string ext = getStrProp(ctx, argv[0], "name");
    std::string key = toStr(ctx, argv[1]);
    int descIdx = (std::string(type) == "option") ? 4 : 3;
    std::string desc = (argc > descIdx && JS_IsString(argv[descIdx])) ? toStr(ctx, argv[descIdx]) : "";
    cfgDefault(m, "cfg:" + ext + ":" + key, def);
    m->addConfig(ext, key, type, def, desc, optionsJson);
}
static JSValue jsRegStrCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 3) regCfg(ctx, argc, argv, "string", toStr(ctx, argv[2]));
    return JS_UNDEFINED;
}
static JSValue jsRegIntCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 3) regCfg(ctx, argc, argv, "int", toStr(ctx, argv[2]));
    return JS_UNDEFINED;
}
static JSValue jsRegFloatCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 3) regCfg(ctx, argc, argv, "float", toStr(ctx, argv[2]));
    return JS_UNDEFINED;
}
static JSValue jsRegBoolCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 3) regCfg(ctx, argc, argv, "bool", JS_ToBool(ctx, argv[2]) ? "1" : "0");
    return JS_UNDEFINED;
}
static JSValue jsRegTmplCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 3) regCfg(ctx, argc, argv, "template", jsArrToJson(ctx, argv[2]));
    return JS_UNDEFINED;
}
static JSValue jsRegOptCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 3) regCfg(ctx, argc, argv, "option", toStr(ctx, argv[2]),
                          argc >= 4 ? jsArrToJson(ctx, argv[3]) : std::string());
    return JS_UNDEFINED;
}
static JSValue jsGetStrCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewString(ctx, "");
    return JS_NewString(ctx, m->kvGet(cfgKey(ctx, argv[0], toStr(ctx, argv[1]))).c_str());
}
static JSValue jsGetIntCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewInt32(ctx, 0);
    try { return JS_NewInt64(ctx, std::stoll(m->kvGet(cfgKey(ctx, argv[0], toStr(ctx, argv[1])), "0"))); } catch (...) { return JS_NewInt32(ctx, 0); }
}
static JSValue jsGetFloatCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewFloat64(ctx, 0);
    try { return JS_NewFloat64(ctx, std::stod(m->kvGet(cfgKey(ctx, argv[0], toStr(ctx, argv[1])), "0"))); } catch (...) { return JS_NewFloat64(ctx, 0); }
}
static JSValue jsGetBoolCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, m->kvGet(cfgKey(ctx, argv[0], toStr(ctx, argv[1]))) == "1");
}
static JSValue jsGetTmplCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewArray(ctx);
    return jsonToArr(ctx, m->kvGet(cfgKey(ctx, argv[0], toStr(ctx, argv[1])), "[]"));
}
static JSValue jsGetOptCfg(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return jsGetStrCfg(ctx, JS_UNDEFINED, argc, argv); }
static JSValue jsNewConfigItem(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewObject(ctx); }
static JSValue jsRegisterConfig(JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }
static JSValue jsGetConfig(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewString(ctx, "");
    return JS_NewString(ctx, m->kvGet(cfgKey(ctx, argv[0], toStr(ctx, argv[1]))).c_str());
}

// ─── seal.deck.draw ──────────────────────────────────────────
static JSValue jsDeckDraw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* m = mgrOf(ctx); if (!m || argc < 2) return JS_NewString(ctx, "");
    bool shuffle = argc >= 3 ? JS_ToBool(ctx, argv[2]) : false;
    return JS_NewString(ctx, m->drawDeck(toStr(ctx, argv[1]), shuffle).c_str());
}

// ─── JsPluginManager ─────────────────────────────────────────
JsPluginManager::~JsPluginManager() { freeRuntime(); }

void JsPluginManager::freeRuntime() {
    if (ctx_) {
        for (auto& e : exts_) JS_FreeValue(ctx_, e.first);
        exts_.clear();
        for (auto& [id, t] : timers_) JS_FreeValue(ctx_, t.cb);   // drop pending timers
        timers_.clear();
        JS_FreeContext(ctx_); ctx_ = nullptr;
    }
    if (rt_) { JS_FreeRuntime(rt_); rt_ = nullptr; }
    {
        std::lock_guard<std::mutex> lk(kvMutex_);
        if (kvDb_) { sqlite3_close(kvDb_); kvDb_ = nullptr; }
    }
}

bool JsPluginManager::init() {
    if (ctx_) return true;
    rt_ = JS_NewRuntime();
    if (!rt_) return false;
    ctx_ = JS_NewContext(rt_);
    if (!ctx_) { JS_FreeRuntime(rt_); rt_ = nullptr; return false; }
    JS_SetContextOpaque(ctx_, this);
    installGlobals();
    openKvStore();
    return true;
}

// ─── 持久化 KV：data/plugins.db（表 js_kv）─────────────────────
// 设计：内存 kv_ 作读缓存（命中按完整 key，跨插件共享语义与旧实现一致）；
// 写为单行 upsert（不再整表覆写文件）。ns 列只用于「按插件查看/导出/删除」。
//
// ns 推导：ext:<插件>:<k>→"ext:<插件>"，cfg:<插件>:<k>→"cfg:<插件>"，
//          seal.vars 的 u:/g:/G: → "shared"（故意跨插件共享），其余 → "misc"。
std::string JsPluginManager::kvNamespace(const std::string& key) {
    auto twoColon = [&](const std::string& prefix) -> std::string {
        // 取 prefix + 第二段（插件名），即第二个冒号之前的整段
        size_t p2 = key.find(':', prefix.size());
        return (p2 == std::string::npos) ? key : key.substr(0, p2);
    };
    if (key.rfind("ext:", 0) == 0) return twoColon("ext:");
    if (key.rfind("cfg:", 0) == 0) return twoColon("cfg:");
    if (key.rfind("u:", 0) == 0 || key.rfind("g:", 0) == 0 || key.rfind("G:", 0) == 0) return "shared";
    return "misc";
}

void JsPluginManager::openKvStore() {
    std::lock_guard<std::mutex> lk(kvMutex_);
    kv_.clear();
    std::error_code ec; fs::create_directories("data", ec);
    if (sqlite3_open("data/plugins.db", &kvDb_) != SQLITE_OK) {
        DICE_LOG_ERROR("JsPluginManager: 无法打开 data/plugins.db：{}", kvDb_ ? sqlite3_errmsg(kvDb_) : "open failed");
        if (kvDb_) { sqlite3_close(kvDb_); kvDb_ = nullptr; }
        return;
    }
    char* err = nullptr;
    sqlite3_exec(kvDb_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    if (sqlite3_exec(kvDb_,
            "CREATE TABLE IF NOT EXISTS js_kv (k TEXT PRIMARY KEY, ns TEXT, v TEXT) WITHOUT ROWID;"
            "CREATE INDEX IF NOT EXISTS idx_js_kv_ns ON js_kv(ns);",
            nullptr, nullptr, &err) != SQLITE_OK) {
        DICE_LOG_ERROR("JsPluginManager: 建表失败：{}", err ? err : "?");
        sqlite3_free(err);
    }

    // 一次性迁移旧的 data/js_kv.json（若库为空且文件存在）→ 之后改名备份。
    bool empty = true;
    sqlite3_stmt* cst = nullptr;
    if (sqlite3_prepare_v2(kvDb_, "SELECT COUNT(*) FROM js_kv;", -1, &cst, nullptr) == SQLITE_OK) {
        if (sqlite3_step(cst) == SQLITE_ROW) empty = (sqlite3_column_int(cst, 0) == 0);
        sqlite3_finalize(cst);
    }
    if (empty && fs::exists("data/js_kv.json")) {
        std::ifstream f("data/js_kv.json", std::ios::binary);
        try {
            json j; f >> j; f.close();
            if (j.is_object()) {
                sqlite3_exec(kvDb_, "BEGIN;", nullptr, nullptr, nullptr);
                sqlite3_stmt* ins = nullptr;
                sqlite3_prepare_v2(kvDb_, "INSERT OR REPLACE INTO js_kv(k,ns,v) VALUES(?,?,?);", -1, &ins, nullptr);
                int n = 0;
                for (auto& [k, v] : j.items()) {
                    if (!v.is_string()) continue;
                    std::string val = v.get<std::string>(), ns = kvNamespace(k);
                    sqlite3_bind_text(ins, 1, k.c_str(),   -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ins, 2, ns.c_str(),  -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ins, 3, val.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ins); sqlite3_reset(ins); ++n;
                }
                sqlite3_finalize(ins);
                sqlite3_exec(kvDb_, "COMMIT;", nullptr, nullptr, nullptr);
                DICE_LOG_INFO("JsPluginManager: 已从 js_kv.json 迁移 {} 条数据到 plugins.db", n);
            }
        } catch (...) { DICE_LOG_WARN("JsPluginManager: js_kv.json 迁移失败（已忽略）"); }
        std::error_code mec; fs::rename("data/js_kv.json", "data/js_kv.json.bak", mec);
    }

    // 装载到内存读缓存。
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(kvDb_, "SELECT k, v FROM js_kv;", -1, &sel, nullptr) == SQLITE_OK) {
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const char* k = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
            if (k) kv_[k] = v ? v : "";
        }
        sqlite3_finalize(sel);
    }
}

std::string JsPluginManager::kvGet(const std::string& key, const std::string& def) const {
    std::lock_guard<std::mutex> lk(kvMutex_);
    auto it = kv_.find(key);
    return it == kv_.end() ? def : it->second;
}
void JsPluginManager::kvSet(const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> lk(kvMutex_);
    kv_[key] = val;                       // 读缓存同步
    if (!kvDb_) return;
    std::string ns = kvNamespace(key);
    sqlite3_stmt* up = nullptr;           // 单行 upsert：O(1)，与总数据量无关
    if (sqlite3_prepare_v2(kvDb_, "INSERT INTO js_kv(k,ns,v) VALUES(?,?,?) "
                                  "ON CONFLICT(k) DO UPDATE SET v=excluded.v, ns=excluded.ns;",
                           -1, &up, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(up, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 2, ns.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 3, val.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(up);
        sqlite3_finalize(up);
    }
}
std::vector<std::pair<std::string, std::string>> JsPluginManager::kvByNamespace(const std::string& ns) const {
    std::lock_guard<std::mutex> lk(kvMutex_);
    std::vector<std::pair<std::string, std::string>> out;
    if (!kvDb_) return out;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(kvDb_, "SELECT k, v FROM js_kv WHERE ns=? ORDER BY k;", -1, &sel, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sel, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const char* k = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
            out.emplace_back(k ? k : "", v ? v : "");
        }
        sqlite3_finalize(sel);
    }
    return out;
}
int JsPluginManager::kvClearNamespace(const std::string& ns) {
    std::lock_guard<std::mutex> lk(kvMutex_);
    if (!kvDb_) return 0;
    // 先从内存缓存剔除（命中该 ns 的 key），再删库。
    for (auto it = kv_.begin(); it != kv_.end(); )
        it = (kvNamespace(it->first) == ns) ? kv_.erase(it) : std::next(it);
    int n = 0;
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(kvDb_, "DELETE FROM js_kv WHERE ns=?;", -1, &del, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(del) == SQLITE_DONE) n = sqlite3_changes(kvDb_);
        sqlite3_finalize(del);
    }
    return n;
}

void JsPluginManager::installGlobals() {
    JSValue g = JS_GetGlobalObject(ctx_);

    JSValue console = JS_NewObject(ctx_);
    JSValue logFn = JS_NewCFunction(ctx_, jsConsoleLog, "log", 1);
    for (const char* m : {"log", "info", "warn", "error"})
        JS_SetPropertyStr(ctx_, console, m, JS_DupValue(ctx_, logFn));
    JS_FreeValue(ctx_, logFn);
    JS_SetPropertyStr(ctx_, g, "console", console);

    JS_SetPropertyStr(ctx_, g, "setTimeout",    JS_NewCFunction(ctx_, jsSetTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx_, g, "setInterval",   JS_NewCFunction(ctx_, jsSetInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx_, g, "clearTimeout",  JS_NewCFunction(ctx_, jsClearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx_, g, "clearInterval", JS_NewCFunction(ctx_, jsClearTimer, "clearInterval", 1));
    JS_SetPropertyStr(ctx_, g, "btoa", JS_NewCFunction(ctx_, jsBtoa, "btoa", 1));
    JS_SetPropertyStr(ctx_, g, "atob", JS_NewCFunction(ctx_, jsAtob, "atob", 1));
    JS_SetPropertyStr(ctx_, g, "fetch", JS_NewCFunction(ctx_, jsFetch, "fetch", 2));

    JSValue ext = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, ext, "new",                 JS_NewCFunction(ctx_, jsExtNew, "new", 3));
    JS_SetPropertyStr(ctx_, ext, "find",                JS_NewCFunction(ctx_, jsExtFind, "find", 1));
    JS_SetPropertyStr(ctx_, ext, "newCmdItemInfo",      JS_NewCFunction(ctx_, jsNewCmdItemInfo, "newCmdItemInfo", 0));
    JS_SetPropertyStr(ctx_, ext, "newCmdExecuteResult", JS_NewCFunction(ctx_, jsNewCmdExecuteResult, "newCmdExecuteResult", 1));
    JS_SetPropertyStr(ctx_, ext, "register",            JS_NewCFunction(ctx_, jsExtRegister, "register", 1));
    JS_SetPropertyStr(ctx_, ext, "registerTask",        JS_NewCFunction(ctx_, jsRegisterTask, "registerTask", 7));
    // 配置项
    JS_SetPropertyStr(ctx_, ext, "newConfigItem",          JS_NewCFunction(ctx_, jsNewConfigItem, "newConfigItem", 0));
    JS_SetPropertyStr(ctx_, ext, "registerConfig",         JS_NewCFunction(ctx_, jsRegisterConfig, "registerConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "getConfig",              JS_NewCFunction(ctx_, jsGetConfig, "getConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "registerStringConfig",   JS_NewCFunction(ctx_, jsRegStrCfg, "registerStringConfig", 3));
    JS_SetPropertyStr(ctx_, ext, "registerIntConfig",      JS_NewCFunction(ctx_, jsRegIntCfg, "registerIntConfig", 3));
    JS_SetPropertyStr(ctx_, ext, "registerFloatConfig",    JS_NewCFunction(ctx_, jsRegFloatCfg, "registerFloatConfig", 3));
    JS_SetPropertyStr(ctx_, ext, "registerBoolConfig",     JS_NewCFunction(ctx_, jsRegBoolCfg, "registerBoolConfig", 3));
    JS_SetPropertyStr(ctx_, ext, "registerTemplateConfig", JS_NewCFunction(ctx_, jsRegTmplCfg, "registerTemplateConfig", 3));
    JS_SetPropertyStr(ctx_, ext, "registerOptionConfig",   JS_NewCFunction(ctx_, jsRegOptCfg, "registerOptionConfig", 4));
    JS_SetPropertyStr(ctx_, ext, "getStringConfig",        JS_NewCFunction(ctx_, jsGetStrCfg, "getStringConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "getIntConfig",           JS_NewCFunction(ctx_, jsGetIntCfg, "getIntConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "getFloatConfig",         JS_NewCFunction(ctx_, jsGetFloatCfg, "getFloatConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "getBoolConfig",          JS_NewCFunction(ctx_, jsGetBoolCfg, "getBoolConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "getTemplateConfig",      JS_NewCFunction(ctx_, jsGetTmplCfg, "getTemplateConfig", 2));
    JS_SetPropertyStr(ctx_, ext, "getOptionConfig",        JS_NewCFunction(ctx_, jsGetOptCfg, "getOptionConfig", 2));

    JSValue seal = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, seal, "ext", ext);
    JS_SetPropertyStr(ctx_, seal, "replyToSender", JS_NewCFunction(ctx_, jsReplyToSender, "replyToSender", 3));
    JS_SetPropertyStr(ctx_, seal, "replyGroup",    JS_NewCFunction(ctx_, jsReplyGroup, "replyGroup", 3));
    JS_SetPropertyStr(ctx_, seal, "replyPerson",   JS_NewCFunction(ctx_, jsReplyPerson, "replyPerson", 3));
    JS_SetPropertyStr(ctx_, seal, "format",        JS_NewCFunction(ctx_, jsFormat, "format", 2));
    JS_SetPropertyStr(ctx_, seal, "formatTmpl",    JS_NewCFunction(ctx_, jsFormat, "formatTmpl", 2));
    JS_SetPropertyStr(ctx_, seal, "getVersion",    JS_NewCFunction(ctx_, jsGetVersion, "getVersion", 0));
    JS_SetPropertyStr(ctx_, seal, "setVarInt",     JS_NewCFunction(ctx_, jsVarIntSet, "setVarInt", 3));
    JS_SetPropertyStr(ctx_, seal, "setVarStr",     JS_NewCFunction(ctx_, jsVarStrSet, "setVarStr", 3));
    JSValue vars = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, vars, "intGet", JS_NewCFunction(ctx_, jsVarIntGet, "intGet", 2));
    JS_SetPropertyStr(ctx_, vars, "intSet", JS_NewCFunction(ctx_, jsVarIntSet, "intSet", 3));
    JS_SetPropertyStr(ctx_, vars, "strGet", JS_NewCFunction(ctx_, jsVarStrGet, "strGet", 2));
    JS_SetPropertyStr(ctx_, vars, "strSet", JS_NewCFunction(ctx_, jsVarStrSet, "strSet", 3));
    JS_SetPropertyStr(ctx_, seal, "vars", vars);
    JSValue deck = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, deck, "draw",   JS_NewCFunction(ctx_, jsDeckDraw, "draw", 3));
    JS_SetPropertyStr(ctx_, deck, "reload", JS_NewCFunction(ctx_, jsNoop, "reload", 0));
    JS_SetPropertyStr(ctx_, seal, "deck", deck);

    // 自定义规则 / 名片：v1 占位（返回对象 / 空操作），保证规则类插件能加载。
    JSValue gs = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, gs, "newTemplate",       JS_NewCFunction(ctx_, jsRuleTemplate, "newTemplate", 1));
    JS_SetPropertyStr(ctx_, gs, "newTemplateByYaml", JS_NewCFunction(ctx_, jsRuleTemplate, "newTemplateByYaml", 1));
    JS_SetPropertyStr(ctx_, seal, "gameSystem", gs);
    JSValue coc = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, coc, "newRule",            JS_NewCFunction(ctx_, jsNewObj, "newRule", 0));
    JS_SetPropertyStr(ctx_, coc, "newRuleCheckResult", JS_NewCFunction(ctx_, jsNewObj, "newRuleCheckResult", 0));
    JS_SetPropertyStr(ctx_, coc, "registerRule",       JS_NewCFunction(ctx_, jsRuleRegister, "registerRule", 1));
    JS_SetPropertyStr(ctx_, seal, "coc", coc);
    JS_SetPropertyStr(ctx_, seal, "setPlayerGroupCard",            JS_NewCFunction(ctx_, jsSetPlayerGroupCard, "setPlayerGroupCard", 2));
    JS_SetPropertyStr(ctx_, seal, "memberBan",  JS_NewCFunction(ctx_, jsMemberBan, "memberBan", 4));
    JS_SetPropertyStr(ctx_, seal, "memberKick", JS_NewCFunction(ctx_, jsMemberKick, "memberKick", 3));
    JS_SetPropertyStr(ctx_, seal, "applyPlayerGroupCardByTemplate", JS_NewCFunction(ctx_, jsNoop, "applyPlayerGroupCardByTemplate", 2));
    JS_SetPropertyStr(ctx_, seal, "newMessage", JS_NewCFunction(ctx_, jsNewObj, "newMessage", 0));
    JS_SetPropertyStr(ctx_, seal, "getEndPoints", JS_NewCFunction(ctx_, jsNewObj, "getEndPoints", 0));
    JS_SetPropertyStr(ctx_, seal, "getCtxProxyFirst", JS_NewCFunction(ctx_, jsGetCtxProxy, "getCtxProxyFirst", 2));
    JS_SetPropertyStr(ctx_, seal, "getCtxProxyAtPos", JS_NewCFunction(ctx_, jsGetCtxProxy, "getCtxProxyAtPos", 3));
    JS_SetPropertyStr(ctx_, seal, "createTempCtx", JS_NewCFunction(ctx_, jsCreateTempCtx, "createTempCtx", 2));
    JSValue ban = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, ban, "addBan",   JS_NewCFunction(ctx_, jsBanAdd, "addBan", 4));
    JS_SetPropertyStr(ctx_, ban, "addTrust", JS_NewCFunction(ctx_, jsBanAddTrust, "addTrust", 4));
    JS_SetPropertyStr(ctx_, ban, "remove",   JS_NewCFunction(ctx_, jsBanRemove, "remove", 2));
    JS_SetPropertyStr(ctx_, ban, "getList",  JS_NewCFunction(ctx_, jsBanGetList, "getList", 0));
    JS_SetPropertyStr(ctx_, ban, "getUser",  JS_NewCFunction(ctx_, jsBanGetUser, "getUser", 1));
    JS_SetPropertyStr(ctx_, seal, "ban", ban);

    JS_SetPropertyStr(ctx_, g, "seal", seal);

    JS_FreeValue(ctx_, g);
}

std::vector<std::string> JsPluginManager::extNamesForFile(const std::string& file) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> names;
    for (auto& e : exts_) {
        if (e.second != file) continue;
        std::string nm = getStrProp(ctx_, e.first, "name");
        if (!nm.empty() && std::find(names.begin(), names.end(), nm) == names.end())
            names.push_back(nm);
    }
    return names;
}

void JsPluginManager::registerExt(JSValueConst ext) {
    std::string extName = getStrProp(ctx_, ext, "name");
    // 实时派发：仅保存 ext 引用，派发时再读 cmdMap（与注册顺序无关）。
    exts_.push_back({ JS_DupValue(ctx_, ext), loadingFile_ });
    DICE_LOG_INFO("[js] registered ext '{}' from {}", extName, loadingFile_);
}

int JsPluginManager::loadDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!ctx_ && !init()) return 0;
    return loadDirLocked(dir);
}

int JsPluginManager::loadDirLocked(const std::string& dir) {
    JS_UpdateStackTop(rt_);   // (re)load may run on a worker thread; re-baseline first
    dir_ = dir;   // remember for WebUI management (upload/toggle/delete + reload)
    // C#7：规则类插件目录 data/mod（主目录的祖父目录 / mod），与主目录一并扫描。
    modDir_ = (fs::path(dir).parent_path().parent_path() / "mod").string();
    rulePluginFiles_.clear();
    gameSystemTemplates_.clear();
    int n = 0;

    // Pass 1: read every enabled (.js) file's source + metadata, from BOTH the main
    // plugin dir and data/mod (rule-compat plugins are migrated there).
    struct Entry { std::string src; PluginMeta meta; };
    std::vector<Entry> entries;
    auto scan = [&](const std::string& dstr, bool fromMod) {
        std::error_code e2;
        fs::path d = fs::path(std::u8string(dstr.begin(), dstr.end()));   // UTF-8（规则包中文目录安全）
        if (!fs::is_directory(d, e2)) return;
        for (auto& e : fs::directory_iterator(d, e2)) {
            if (e2 || !e.is_regular_file() || e.path().extension() != ".js") continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            PluginMeta meta = parseMeta(src);
            if (meta.name.empty()) meta.name = dnx_u8str(e.path().stem());
            meta.file = dnx_u8str(e.path().filename());
            meta.inMod = fromMod;
            entries.push_back(Entry{std::move(src), std::move(meta)});
        }
    };
    scan(dir, false);
    if (!modDir_.empty() && modDir_ != dir) scan(modDir_, true);
    for (auto& ed : extraDirs_) scan(ed, false);   // C#27：规则包附加 js 目录
    if (entries.empty()) return 0;

    // Pass 2: de-dupe by @name (NOT filename) — only the highest @version of each
    // name stays active; older same-name files are kept but marked superseded.
    std::map<std::string, size_t> winner;   // name -> index of current best
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& nm = entries[i].meta.name;
        auto it = winner.find(nm);
        if (it == winner.end()) { winner[nm] = i; continue; }
        size_t& best = it->second;
        if (compareVersions(entries[best].meta.version, entries[i].meta.version) < 0) {
            entries[best].meta.superseded = true;
            entries[best].meta.supersededBy = entries[i].meta.version;
            best = i;
        } else {
            entries[i].meta.superseded = true;
            entries[i].meta.supersededBy = entries[best].meta.version;
        }
    }

    // Pass 3: register the winners; record all (superseded ones are listed but not run).
    for (auto& e : entries) {
        plugins_.push_back(e.meta);
        if (e.meta.superseded) {
            DICE_LOG_INFO("[js] '{}' ({}) superseded by v{} — not activated",
                          e.meta.file, e.meta.version, e.meta.supersededBy);
            continue;
        }
        loadingFile_ = e.meta.file;
        // 每个插件包进独立函数作用域，避免多文件顶层 let/const（如 ext）互相冲突；
        // 并提供 module/exports（部分插件是打包过的 CommonJS 模块）。
        std::string wrapped = "(function(){const module={exports:{}};const exports=module.exports;\n"
                              + e.src + "\n})();";
        JSValue r = JS_Eval(ctx_, wrapped.c_str(), wrapped.size(), e.meta.file.c_str(), JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx_);
            DICE_LOG_ERROR("[js] plugin '{}' load error: {}", e.meta.file, toStr(ctx_, exc));
            JS_FreeValue(ctx_, exc);
        } else { ++n; }
        JS_FreeValue(ctx_, r);
        loadingFile_.clear();
    }

    // C#7：eval 后 rulePluginFiles_ 已填好 → 标记规则类，并把仍在主目录的规则类插件
    // 迁移到 data/mod（已加载在内存，移动文件不影响本次运行；下次 reload 从 mod 加载）。
    for (auto& p : plugins_) if (rulePluginFiles_.count(p.file)) p.ruleCompat = true;
    if (!modDir_.empty() && modDir_ != dir) {
        for (auto& p : plugins_) {
            if (!p.ruleCompat || p.inMod || p.superseded) continue;
            std::error_code mec;
            fs::path from = fs::path(dir) / p.file, to = fs::path(modDir_) / p.file;
            if (!fs::exists(from, mec)) continue;
            fs::create_directories(modDir_, mec);
            if (fs::exists(to, mec)) continue;     // 同名已存在 → 不覆盖
            fs::rename(from, to, mec);
            if (!mec) { p.inMod = true; DICE_LOG_INFO("[js] rule plugin '{}' migrated to {}", p.file, modDir_); }
        }
    }
    DICE_LOG_INFO("[js] loaded {} plugin file(s) from {} (+{})", n, dir, modDir_);
    return n;
}

int64_t JsPluginManager::addTimer(JSValue cb, double delaySec, double intervalSec) {
    // Caller holds mutex_. Guard against abuse (runaway timers / too-fast intervals).
    if (timers_.size() >= 256 || !scheduler_) { if (ctx_) JS_FreeValue(ctx_, cb); return 0; }
    if (intervalSec > 0 && intervalSec < 0.2) intervalSec = 0.2;   // min 200ms
    if (delaySec < 0) delaySec = 0;
    int64_t id = ++timerSeq_;
    timers_[id] = Timer{cb, intervalSec};
    scheduler_(delaySec, [this, id] { fireTimer(id); });
    return id;
}

void JsPluginManager::clearTimer(int64_t id) {   // caller holds mutex_
    auto it = timers_.find(id);
    if (it != timers_.end()) { if (ctx_) JS_FreeValue(ctx_, it->second.cb); timers_.erase(it); }
}

void JsPluginManager::fireTimer(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = timers_.find(id);
    if (it == timers_.end() || !ctx_) return;     // cleared, or runtime gone (reload)
    JS_UpdateStackTop(rt_);
    JSValue cb = it->second.cb;
    double interval = it->second.intervalSec;
    bool prev = capturing_; capturing_ = false;   // timer runs outside a message turn → replies route
    JSValue r = JS_Call(ctx_, cb, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(r)) { JSValue e = JS_GetException(ctx_); DICE_LOG_ERROR("[js] timer error: {}", toStr(ctx_, e)); JS_FreeValue(ctx_, e); }
    JS_FreeValue(ctx_, r);
    drainJobs();
    capturing_ = prev;
    it = timers_.find(id);                          // callback may have cleared itself
    if (it == timers_.end()) return;
    if (interval > 0) { scheduler_(interval, [this, id] { fireTimer(id); }); }   // re-arm
    else { JS_FreeValue(ctx_, it->second.cb); timers_.erase(it); }               // one-shot
}

void JsPluginManager::routeReply(const std::string& platform, bool isPrivate,
                                 const std::string& groupId, const std::string& userId, const std::string& text) {
    if (sender_) sender_(platform, isPrivate, groupId, userId, text);
}

JSValue JsPluginManager::findExt(const std::string& name) const {
    for (auto& e : exts_) {
        if (getStrProp(ctx_, e.first, "name") == name) return JS_DupValue(ctx_, e.first);
    }
    return JS_NULL;
}

bool JsPluginManager::hasCommand(const std::string& word) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!ctx_ || word.empty()) return false;
    bool found = false;
    for (auto& e : exts_) {
        JSValue cmdMap = JS_GetPropertyStr(ctx_, e.first, "cmdMap");
        if (JS_IsObject(cmdMap)) {
            JSValue c = JS_GetPropertyStr(ctx_, cmdMap, word.c_str());
            found = JS_IsObject(c);
            JS_FreeValue(ctx_, c);
        }
        JS_FreeValue(ctx_, cmdMap);
        if (found) break;
    }
    return found;
}

int JsPluginManager::compareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<std::string> out; std::string cur;
        for (char c : s) { if (c == '.') { out.push_back(cur); cur.clear(); } else cur += c; }
        out.push_back(cur);
        return out;
    };
    auto va = split(a), vb = split(b);
    size_t n = std::max(va.size(), vb.size());
    for (size_t i = 0; i < n; ++i) {
        std::string sa = i < va.size() ? va[i] : "0";
        std::string sb = i < vb.size() ? vb[i] : "0";
        bool na = !sa.empty() && std::all_of(sa.begin(), sa.end(), ::isdigit);
        bool nb = !sb.empty() && std::all_of(sb.begin(), sb.end(), ::isdigit);
        if (na && nb) {
            long long ia = 0, ib = 0;
            try { ia = std::stoll(sa); ib = std::stoll(sb); } catch (...) {}
            if (ia != ib) return ia < ib ? -1 : 1;
        } else if (sa != sb) {
            return sa < sb ? -1 : 1;
        }
    }
    return 0;
}

int JsPluginManager::reload(const std::string& dir) {
    // Hold the mutex across the WHOLE teardown+reload so a live message running on
    // the adapter thread can't use ctx_/rt_ while we free and rebuild them.
    std::lock_guard<std::mutex> lk(mutex_);
    freeRuntime();
    plugins_.clear();
    configs_.clear();
    if (!init()) return 0;
    // 与 Lua 一致：规则包删除会使附加目录瞬间消失。加载中的异常必须留在
    // 插件层处理，不能把 HTTP 处理线程或骰娘主服务带进半重载状态。
    try {
        return loadDirLocked(dir);
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("[js] reload failed; runtime remains available with no newly loaded plugins: {}", e.what());
    } catch (...) {
        DICE_LOG_ERROR("[js] reload failed with an unknown error; runtime remains available with no newly loaded plugins");
    }
    return 0;
}

std::optional<std::string> JsPluginManager::evalString(const std::string& script) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!ctx_) return std::nullopt;
    JSValue r = JS_Eval(ctx_, script.c_str(), script.size(), "<card-template>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx_);
        DICE_LOG_WARN("[js] evalString error: {}", toStr(ctx_, exc));
        JS_FreeValue(ctx_, exc);
        JS_FreeValue(ctx_, r);
        return std::nullopt;
    }
    std::string out = toStr(ctx_, r);
    JS_FreeValue(ctx_, r);
    return out;
}

void JsPluginManager::addConfig(const std::string& ext, const std::string& key, const std::string& type,
                                const std::string& def, const std::string& description,
                                const std::string& optionsJson) {
    // Stamp the source file from the plugin currently being loaded (loadingFile_).
    // De-dupe on ext+key in case a plugin registers the same item twice.
    for (auto& c : configs_) if (c.ext == ext && c.key == key) return;
    configs_.push_back(ConfigItem{loadingFile_, ext, key, type, def, description, optionsJson});
}

std::vector<JsPluginManager::ConfigItem> JsPluginManager::configs() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return configs_;
}

void JsPluginManager::drainJobs() {
    if (!rt_) return;
    JSContext* cctx = nullptr;
    // Bounded loop: run queued promise jobs until none remain (or error).
    for (int i = 0; i < 100000; ++i) {
        int err = JS_ExecutePendingJob(rt_, &cctx);
        if (err == 0) break;            // no more jobs
        if (err < 0) {                  // a job threw — surface and stop
            JSValue exc = JS_GetException(cctx ? cctx : ctx_);
            DICE_LOG_ERROR("[js] async job error: {}", toStr(cctx ? cctx : ctx_, exc));
            JS_FreeValue(cctx ? cctx : ctx_, exc);
            break;
        }
    }
}

std::vector<JsPluginManager::PluginMeta> JsPluginManager::listAll() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<PluginMeta> out = plugins_;   // loaded (enabled) ones
    // Count commands per source file by enumerating each ext's live cmdMap
    // (some plugins add commands after register, so count now, not at load).
    if (ctx_) {
        std::unordered_map<std::string, std::vector<std::string>> cmdNames;
        for (auto& e : exts_) {
            JSValue cmdMap = JS_GetPropertyStr(ctx_, e.first, "cmdMap");
            if (JS_IsObject(cmdMap)) {
                JSPropertyEnum* tab = nullptr; uint32_t len = 0;
                if (JS_GetOwnPropertyNames(ctx_, &tab, &len, cmdMap,
                                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                    auto& vec = cmdNames[e.second];
                    for (uint32_t i = 0; i < len; ++i) {
                        JSValue nm = JS_AtomToString(ctx_, tab[i].atom);
                        vec.push_back(toStr(ctx_, nm));
                        JS_FreeValue(ctx_, nm);
                        JS_FreeAtom(ctx_, tab[i].atom);
                    }
                    js_free(ctx_, tab);
                }
            }
            JS_FreeValue(ctx_, cmdMap);
        }
        for (auto& p : out) {
            auto it = cmdNames.find(p.file);
            if (it != cmdNames.end()) { p.commandList = it->second; p.commands = (int)it->second.size(); }
        }
    }
    // Append any disabled files (`*.js.disabled`) from BOTH dirs so the WebUI can
    // re-enable them. Disabled files aren't run → detect rule-compat statically.
    auto scanDisabled = [&](const std::string& d, bool fromMod) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) return;
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (ec || !e.is_regular_file()) continue;
            std::string fname = dnx_u8str(e.path().filename());
            if (fname.size() < 12 || fname.substr(fname.size() - 12) != ".js.disabled") continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            PluginMeta m = parseMeta(src);
            if (m.name.empty()) m.name = dnx_u8str(e.path().stem().stem());  // strip .js.disabled
            m.file = fname;
            m.enabled = false;
            m.inMod = fromMod;
            m.ruleCompat = src.find("gameSystem.newTemplate") != std::string::npos
                        || src.find("coc.registerRule") != std::string::npos;
            out.push_back(m);
        }
    };
    scanDisabled(dir_, false);
    if (!modDir_.empty() && modDir_ != dir_) scanDisabled(modDir_, true);
    return out;
}

// C#7：定位某插件文件实际所在目录（主目录或 data/mod），找不到则回退主目录。
std::string JsPluginManager::dirForFile(const std::string& file) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::error_code ec;
    std::string base = file;
    if (base.size() > 9 && base.substr(base.size() - 9) == ".disabled") base = base.substr(0, base.size() - 9);
    for (const std::string& d : { dir_, modDir_ }) {
        if (d.empty()) continue;
        if (fs::exists(fs::path(d) / file, ec) || fs::exists(fs::path(d) / base, ec)
            || fs::exists(fs::path(d) / (base + ".disabled"), ec)) return d;
    }
    return dir_.empty() ? std::string("data/plugins/js") : dir_;
}

std::vector<JsPluginManager::CmdHelp> JsPluginManager::commandHelps() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<CmdHelp> out;
    if (!ctx_) return out;
    std::unordered_map<std::string, std::string> fileName;   // 源文件 → 插件显示名
    for (auto& p : plugins_) fileName[p.file] = p.name.empty() ? p.file : p.name;
    for (auto& e : exts_) {
        std::string pname = fileName.count(e.second) ? fileName[e.second] : e.second;
        JSValue cmdMap = JS_GetPropertyStr(ctx_, e.first, "cmdMap");
        if (JS_IsObject(cmdMap)) {
            JSPropertyEnum* tab = nullptr; uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx_, &tab, &len, cmdMap,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; ++i) {
                    JSValue nm = JS_AtomToString(ctx_, tab[i].atom);
                    std::string cn = toStr(ctx_, nm); JS_FreeValue(ctx_, nm);
                    JSValue c = JS_GetPropertyStr(ctx_, cmdMap, cn.c_str());
                    std::string h; if (JS_IsObject(c)) h = getStrProp(ctx_, c, "help");
                    JS_FreeValue(ctx_, c);
                    if (!cn.empty()) out.push_back({pname, cn, h});
                    JS_FreeAtom(ctx_, tab[i].atom);
                }
                js_free(ctx_, tab);
            }
        }
        JS_FreeValue(ctx_, cmdMap);
    }
    return out;
}

JsPluginManager::UpdateInfo JsPluginManager::checkUpdate(const std::string& file) const {
    UpdateInfo info;
    if (!updateFetch_) { info.error = "update fetch not available"; return info; }
    std::string dir = dir_.empty() ? "data/plugins/js" : dir_;
    std::ifstream f(fs::path(dir) / file, std::ios::binary);
    if (!f) { info.error = "file not found"; return info; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    PluginMeta local = parseMeta(src);
    info.current = local.version;
    info.updateUrl = local.updateUrl;
    if (local.updateUrl.empty()) { info.error = "plugin declares no @updateUrl"; return info; }

    int status = 0;
    std::string remote = updateFetch_(local.updateUrl, status);
    if (status != 200 || remote.empty()) { info.error = "fetch failed (status " + std::to_string(status) + ")"; return info; }
    PluginMeta rmeta = parseMeta(remote);
    info.latest = rmeta.version;
    info.ok = true;
    info.hasUpdate = !info.latest.empty() && compareVersions(info.current, info.latest) < 0;
    return info;
}

bool JsPluginManager::updatePlugin(const std::string& file, std::string& err) {
    if (!updateFetch_) { err = "update fetch not available"; return false; }
    std::string dir = dir_.empty() ? "data/plugins/js" : dir_;
    std::ifstream f(fs::path(dir) / file, std::ios::binary);
    if (!f) { err = "file not found"; return false; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    PluginMeta local = parseMeta(src);
    if (local.updateUrl.empty()) { err = "plugin declares no @updateUrl"; return false; }
    int status = 0;
    std::string remote = updateFetch_(local.updateUrl, status);
    if (status != 200 || remote.empty()) { err = "fetch failed (status " + std::to_string(status) + ")"; return false; }
    // Sanity: the fetched content should itself be a plugin (has a UserScript name).
    PluginMeta rmeta = parseMeta(remote);
    if (rmeta.name.empty()) { err = "downloaded content is not a valid plugin"; return false; }
    std::ofstream out(fs::path(dir) / file, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write file"; return false; }
    out << remote;
    return true;
}

JsPluginManager::PluginMeta JsPluginManager::parseMeta(const std::string& src) {
    PluginMeta m;
    std::istringstream iss(src);
    std::string line; int scanned = 0;
    while (std::getline(iss, line) && scanned < 40) {
        ++scanned;
        auto at = line.find("// @");
        if (at == std::string::npos) continue;
        std::string rest = line.substr(at + 4);
        auto sp = rest.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string key = rest.substr(0, sp);
        std::string val = rest.substr(sp);
        val.erase(0, val.find_first_not_of(" \t"));
        if (auto e = val.find_last_not_of(" \t\r\n"); e != std::string::npos) val.erase(e + 1);
        if (key == "name") m.name = val;
        else if (key == "author") m.author = val;
        else if (key == "version") m.version = val;
        else if (key == "description") m.desc = val;
        else if (key == "homepageURL" || key == "homepage") m.homepage = val;
        else if (key == "updateUrl" || key == "updateURL") m.updateUrl = val;
        else if (key == "license") m.license = val;
    }
    return m;
}

std::vector<JsPluginManager::PluginMeta> JsPluginManager::plugins() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return plugins_;
}

void JsPluginManager::buildCtxMsg(const std::string& platform, const std::string& userId,
                                  const std::string& nickname, const std::string& groupId, const std::string& incomingCard,
                                  bool isPrivate, const std::string& fullMsg, int privilege,
                                  const std::vector<std::string>& atList,
                                  JSValue& outCtx, JSValue& outMsg) {
    JSValue jctx = JS_NewObject(ctx_);
    JSValue player = JS_NewObject(ctx_);
    // D#01：群名片（沿用 SealDice 语义：player.name = 群名片/显示名，非群或无名片时回退 QQ 昵称）。
    // OneBot 入站 sender.card 是当前消息的权威值；成员缓存只作为合成消息的兜底。
    std::string groupCard = incomingCard;
    if (groupCard.empty() && cardNameResolver_ && !groupId.empty())
        groupCard = cardNameResolver_(platform, groupId, userId);
    std::string dispName = groupCard.empty() ? nickname : groupCard;
    JS_SetPropertyStr(ctx_, player, "userId", JS_NewString(ctx_, userId.c_str()));
    JS_SetPropertyStr(ctx_, player, "name",   JS_NewString(ctx_, dispName.c_str()));
    JS_SetPropertyStr(ctx_, player, "card",   JS_NewString(ctx_, groupCard.c_str()));
    // 海豹字段：玩家自动名片模板名。我们暂不跟踪，给定义空串避免插件读到 undefined
    // 再回传给 setPlayerGroupCard 把名片写成 "undefined"。
    JS_SetPropertyStr(ctx_, player, "autoSetNameTemplate", JS_NewString(ctx_, ""));
    JS_SetPropertyStr(ctx_, jctx, "player", player);
    // @ targets (for getCtxProxyFirst/AtPos → 代骰/代查).
    JSValue atArr = JS_NewArray(ctx_);
    for (uint32_t i = 0; i < atList.size(); ++i) JS_SetPropertyUint32(ctx_, atArr, i, JS_NewString(ctx_, atList[i].c_str()));
    JS_SetPropertyStr(ctx_, jctx, "_at", atArr);
    JSValue group = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, group, "groupId", JS_NewString(ctx_, groupId.c_str()));
    bool logOn = false; std::string logCurName;
    if (!groupId.empty() && logStateResolver_) {
        auto logState = logStateResolver_(platform, groupId);
        logOn = logState.first; logCurName = std::move(logState.second);
    }
    JS_SetPropertyStr(ctx_, group, "logOn", JS_NewBool(ctx_, logOn));
    JS_SetPropertyStr(ctx_, group, "logCurName", JS_NewString(ctx_, logCurName.c_str()));
    JS_SetPropertyStr(ctx_, jctx, "group", group);
    JS_SetPropertyStr(ctx_, jctx, "isPrivate", JS_NewBool(ctx_, isPrivate));
    JS_SetPropertyStr(ctx_, jctx, "isCurGroupBotOn", JS_NewBool(ctx_, 1));
    JS_SetPropertyStr(ctx_, jctx, "privilegeLevel", JS_NewInt32(ctx_, privilege));
    JS_SetPropertyStr(ctx_, jctx, "commandHideFlag", JS_NewString(ctx_, ""));  // SealDice 字段，默认空
    JS_SetPropertyStr(ctx_, jctx, "delegateText", JS_NewString(ctx_, ""));
    JSValue endPoint = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, endPoint, "platform", JS_NewString(ctx_, platform.c_str()));
    JS_SetPropertyStr(ctx_, jctx, "endPoint", endPoint);

    JSValue jmsg = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, jmsg, "message", JS_NewString(ctx_, fullMsg.c_str()));
    JS_SetPropertyStr(ctx_, jmsg, "platform", JS_NewString(ctx_, platform.c_str()));
    JS_SetPropertyStr(ctx_, jmsg, "messageType", JS_NewString(ctx_, isPrivate ? "private" : "group"));
    JS_SetPropertyStr(ctx_, jmsg, "groupId", JS_NewString(ctx_, groupId.c_str()));
    JS_SetPropertyStr(ctx_, jmsg, "rawId", JS_NewString(ctx_, ""));
    JS_SetPropertyStr(ctx_, jmsg, "guildId", JS_NewString(ctx_, ""));
    JS_SetPropertyStr(ctx_, jmsg, "channelId", JS_NewString(ctx_, ""));        // SealDice 字段
    JS_SetPropertyStr(ctx_, jmsg, "time", JS_NewInt64(ctx_, (int64_t)std::time(nullptr)));
    JS_SetPropertyStr(ctx_, jmsg, "segment", JS_NewArray(ctx_));               // 防插件读 .segment.forEach 崩
    JSValue sender = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, sender, "userId", JS_NewString(ctx_, userId.c_str()));
    JS_SetPropertyStr(ctx_, sender, "nickname", JS_NewString(ctx_, nickname.c_str()));
    JS_SetPropertyStr(ctx_, sender, "card", JS_NewString(ctx_, groupCard.c_str()));   // D#01：群名片
    JS_SetPropertyStr(ctx_, jmsg, "sender", sender);
    outCtx = jctx; outMsg = jmsg;
}

JsPluginManager::Result JsPluginManager::handleNonCommand(const std::string& platform, const std::string& userId,
                                                           const std::string& nickname, const std::string& groupId,
                                                           const std::string& groupCard, bool isPrivate, const std::string& fullText, int privilege,
                                                          const std::vector<std::string>& atList) {
    Result res;
    if (!ctx_ || exts_.empty()) return res;
    std::lock_guard<std::mutex> lk(mutex_);
    JS_UpdateStackTop(rt_);   // re-baseline stack check for this (adapter) thread
    JSValue jctx, jmsg;
    buildCtxMsg(platform, userId, nickname, groupId, groupCard, isPrivate, fullText, privilege, atList, jctx, jmsg);
    pendingReply_.clear();
    sideEffectReply_ = false;
    capturing_ = true;   // in a message turn → replyToSender accumulates (returned below)
    JSValueConst args[2] = { jctx, jmsg };
    for (auto& e : exts_) {
        for (const char* hook : {"onNotCommandReceived", "onMessageReceived"}) {
            JSValue fn = JS_GetPropertyStr(ctx_, e.first, hook);
            if (JS_IsFunction(ctx_, fn)) {
                JSValue r = JS_Call(ctx_, fn, e.first, 2, args);
                if (JS_IsException(r)) { JSValue exc = JS_GetException(ctx_); DICE_LOG_ERROR("[js] {} error: {}", hook, toStr(ctx_, exc)); JS_FreeValue(ctx_, exc); }
                JS_FreeValue(ctx_, r);
            }
            JS_FreeValue(ctx_, fn);
        }
    }
    drainJobs();   // let async hooks (awaited fetch) finish before reading replies
    capturing_ = false;
    res.reply = pendingReply_;
    res.matched = !pendingReply_.empty() || sideEffectReply_;
    JS_FreeValue(ctx_, jctx); JS_FreeValue(ctx_, jmsg);
    return res;
}

JsPluginManager::Result JsPluginManager::handle(const std::string& platform, const std::string& userId,
                                                 const std::string& nickname, const std::string& groupId,
                                                 const std::string& groupCard, bool isPrivate, const std::string& cmdLine, int privilege,
                                                const std::vector<std::string>& atList) {
    Result res;
    if (!ctx_) return res;
    std::lock_guard<std::mutex> lk(mutex_);
    // Re-baseline the stack-overflow check for THIS thread — live messages run on
    // the adapter thread, not the thread that created the runtime; without this
    // quickjs mis-measures depth and every call throws "Maximum call stack size
    // exceeded", breaking all plugins.
    JS_UpdateStackTop(rt_);

    std::string line = cmdLine;
    line.erase(0, line.find_first_not_of(" \t"));
    size_t sp = line.find_first_of(" \t");
    std::string word = (sp == std::string::npos) ? line : line.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? "" : line.substr(sp + 1);
    if (word.empty()) return res;

    // 实时在所有已注册 ext 的 cmdMap 里找指令名。
    JSValue cmd = JS_UNDEFINED;
    for (auto& e : exts_) {
        // 插件分群启停（C#27 地基）：该源文件在本群被禁用 → 跳过其全部指令。
        if (groupGate_ && !groupId.empty() && !e.second.empty()
            && !groupGate_(platform, groupId, "js:" + e.second)) continue;
        JSValue cmdMap = JS_GetPropertyStr(ctx_, e.first, "cmdMap");
        if (JS_IsObject(cmdMap)) {
            JSValue c = JS_GetPropertyStr(ctx_, cmdMap, word.c_str());
            if (JS_IsObject(c)) { cmd = c; JS_FreeValue(ctx_, cmdMap); break; }
            JS_FreeValue(ctx_, c);
        }
        JS_FreeValue(ctx_, cmdMap);
    }
    if (!JS_IsObject(cmd)) return res;

    JSValue solve = JS_GetPropertyStr(ctx_, cmd, "solve");
    if (!JS_IsFunction(ctx_, solve)) { JS_FreeValue(ctx_, solve); JS_FreeValue(ctx_, cmd); res.matched = true; return res; }

    JSValue jctx, jmsg;
    buildCtxMsg(platform, userId, nickname, groupId, groupCard, isPrivate, cmdLine, privilege, atList, jctx, jmsg);

    JSValue jargs = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, jargs, "command", JS_NewString(ctx_, word.c_str()));
    JS_SetPropertyStr(ctx_, jargs, "rawArgs", JS_NewString(ctx_, rest.c_str()));
    JS_SetPropertyStr(ctx_, jargs, "cleanArgs", JS_NewString(ctx_, rest.c_str()));
    JSValue arr = JS_NewArray(ctx_);
    { std::istringstream iss(rest); std::string t; uint32_t i = 0;
      while (iss >> t) JS_SetPropertyUint32(ctx_, arr, i++, JS_NewString(ctx_, t.c_str())); }
    JS_SetPropertyStr(ctx_, jargs, "_args", JS_DupValue(ctx_, arr));
    JS_SetPropertyStr(ctx_, jargs, "args", arr);
    // at: 数组，每项 {userId}（海豹插件用 cmdArgs.at[i].userId 做 @目标/代骰）。
    JSValue atArr = JS_NewArray(ctx_);
    for (uint32_t i = 0; i < atList.size(); ++i) {
        JSValue o = JS_NewObject(ctx_);
        JS_SetPropertyStr(ctx_, o, "userId", JS_NewString(ctx_, atList[i].c_str()));
        JS_SetPropertyUint32(ctx_, atArr, i, o);
    }
    JS_SetPropertyStr(ctx_, jargs, "at", atArr);
    // kwargs: 数组，解析 rawArgs 里所有 --name / --name=value（海豹 cmdArgs.kwargs）。
    JSValue kwArr = JS_NewArray(ctx_);
    { std::istringstream kss(rest); std::string tok; uint32_t ki = 0;
      while (kss >> tok) {
          if (tok.rfind("--", 0) != 0) continue;
          std::string body = tok.substr(2), k = body, v; bool has = false;
          if (auto eq = body.find('='); eq != std::string::npos) { k = body.substr(0, eq); v = body.substr(eq + 1); has = true; }
          if (k.empty()) continue;
          JSValue o = JS_NewObject(ctx_);
          JS_SetPropertyStr(ctx_, o, "name", JS_NewString(ctx_, k.c_str()));
          JS_SetPropertyStr(ctx_, o, "value", JS_NewString(ctx_, v.c_str()));
          JS_SetPropertyStr(ctx_, o, "valueExists", JS_NewBool(ctx_, has));
          JS_SetPropertyUint32(ctx_, kwArr, ki++, o);
      } }
    JS_SetPropertyStr(ctx_, jargs, "kwargs", kwArr);
    JS_SetPropertyStr(ctx_, jargs, "amIBeMentioned", JS_NewBool(ctx_, 0));       // SealDice 字段（默认否）
    JS_SetPropertyStr(ctx_, jargs, "amIBeMentionedFirst", JS_NewBool(ctx_, 0));
    JS_SetPropertyStr(ctx_, jargs, "getArgN", JS_NewCFunction(ctx_, jsCmdArgsGetArgN, "getArgN", 1));
    JS_SetPropertyStr(ctx_, jargs, "getRestArgsFrom", JS_NewCFunction(ctx_, jsCmdArgsGetRestArgsFrom, "getRestArgsFrom", 1));
    JS_SetPropertyStr(ctx_, jargs, "isArgEqual", JS_NewCFunction(ctx_, jsCmdArgsIsArgEqual, "isArgEqual", 1));
    JS_SetPropertyStr(ctx_, jargs, "chopPrefixToArgsWith", JS_NewCFunction(ctx_, jsChopPrefix, "chopPrefixToArgsWith", 1));
    JS_SetPropertyStr(ctx_, jargs, "eatPrefixWith", JS_NewCFunction(ctx_, jsEatPrefix, "eatPrefixWith", 1));
    JS_SetPropertyStr(ctx_, jargs, "getKwarg", JS_NewCFunction(ctx_, jsGetKwarg, "getKwarg", 1));
    JS_SetPropertyStr(ctx_, jargs, "getKwargs", JS_NewCFunction(ctx_, jsGetKwarg, "getKwargs", 1));

    pendingReply_.clear();
    sideEffectReply_ = false;
    capturing_ = true;   // in a message turn → replyToSender accumulates (returned below)
    JSValueConst callArgs[3] = { jctx, jmsg, jargs };
    JSValue rv = JS_Call(ctx_, solve, cmd, 3, callArgs);
    if (JS_IsException(rv)) {
        JSValue exc = JS_GetException(ctx_);
        DICE_LOG_ERROR("[js] command '{}' error: {}", word, toStr(ctx_, exc));
        JS_FreeValue(ctx_, exc);
    } else if (JS_IsObject(rv)) {
        JSValue sh = JS_GetPropertyStr(ctx_, rv, "showHelp");
        if (JS_ToBool(ctx_, sh)) {
            std::string help = getStrProp(ctx_, cmd, "help");
            if (!help.empty()) appendReply(help);
        }
        JS_FreeValue(ctx_, sh);
    }

    // Run any queued promise jobs so an async solve() (using awaited fetch, which
    // we resolve synchronously) completes and its replyToSender calls land here.
    drainJobs();
    capturing_ = false;

    res.matched = true;
    res.reply = pendingReply_;

    JS_FreeValue(ctx_, rv);
    JS_FreeValue(ctx_, jctx); JS_FreeValue(ctx_, jmsg); JS_FreeValue(ctx_, jargs);
    JS_FreeValue(ctx_, solve); JS_FreeValue(ctx_, cmd);
    return res;
}

} // namespace dice
