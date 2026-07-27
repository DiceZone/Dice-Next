#pragma once
// ─── Dice!Next — Legacy Dice! V2 [DiceData] importer ─────────
// Reads a real original-Dice! data folder and imports the high-value,
// reliably-mappable data into DiceNext. Binary formats via legacy_dice2.h.
//
//   user/PlayerCards.RDconf → cards.db (character_cards)
//   user/UserConf.dat       → player_profiles (nickname / trust / favor)
//   conf/BlackList.json     → banlist (offenders → user blacklist)
//   conf/CustomMsgReply.json→ reply_rules (multi-condition engine)
//   conf/CustomHelp.json    → i18n overrides (help.topic.*)
//   conf/Console.xml        → dice.masters (config)
//   mod/                    → data/mod/   (Lua 规则/功能 mod — 子系统已兼容)
//   PublicDeck/             → data/decks/ (外置牌堆 — 引擎已兼容)
//   user/ChatConf.dat       → group_settings (群配置: 停用/房规/欢迎词)

#include "database.h"
#include "legacy_dice2.h"
#include "../config/config_manager.h"
#include "../i18n/i18n.h"
#include "../core/reply/reply_manager.h"
#include "../common/utils.h"
#include "../common/logger.h"

#include <nlohmann/json.hpp>
#include <sqlite_orm/sqlite_orm.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <set>

namespace dice {
// Forward declarations (used as pointers in runImport for post-import reload)
class CardDeck;
class LuaPluginManager;
}  // namespace dice

namespace dice::legacyv2 {

namespace fs = std::filesystem;
namespace orm = sqlite_orm;
using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════
// Structured import result types
// ═══════════════════════════════════════════════════════════════

/// Options controlling import behavior.
struct ImportOptions {
    bool overwrite = false;  ///< true = overwrite existing; false = skip existing
};

/// A single file's import outcome.
struct ImportDetail {
    std::string name;    ///< file/dir name
    std::string status;  ///< "success" | "skipped" | "failed"
    std::string reason;  ///< explanation (especially for skipped/failed)
};

/// Structured result returned by importMods/importDecks.
struct ImportResult {
    int success = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<ImportDetail> details;

    json toJSON() const {
        json j;
        j["success"] = success;
        j["skipped"] = skipped;
        j["failed"] = failed;
        auto arr = json::array();
        for (auto& d : details) {
            arr.push_back(json{{"name", d.name}, {"status", d.status}, {"reason", d.reason}});
        }
        j["details"] = arr;
        return j;
    }
};

/// Validate a deck JSON file: must be a JSON object where values are arrays.
inline std::string validateDeckJson(const std::string& content) {
    try {
        json j = json::parse(content);
        if (!j.is_object()) return "not a JSON object";
        for (auto& [key, val] : j.items()) {
            if (!val.is_array()) return "value for '" + key + "' is not an array";
        }
        return "";  // valid
    } catch (const std::exception& e) {
        return std::string("JSON parse error: ") + e.what();
    }
}

inline std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/// Truncate to at most @p maxBytes WITHOUT splitting a multi-byte UTF-8 char.
/// A naive substr(0,N) can cut a 多字节 char in half → invalid UTF-8, which later
/// breaks json.dump (导致黑名单等接口 500)。在续字节(10xxxxxx)处回退到字符边界。
inline std::string utf8Truncate(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

/// Original Dice! wrote raw CR/LF/TAB INSIDE JSON string values (illegal JSON
/// that strict parsers reject). Escape control chars that occur inside strings.
inline std::string sanitizeJsonControls(const std::string& in) {
    std::string out; out.reserve(in.size() + 16);
    bool inStr = false, esc = false;
    for (char c : in) {
        if (inStr) {
            if (esc) { out += c; esc = false; continue; }
            if (c == '\\') { out += c; esc = true; continue; }
            if (c == '"') { inStr = false; out += c; continue; }
            if (c == '\r') { out += "\\r"; continue; }
            if (c == '\n') { out += "\\n"; continue; }
            if (c == '\t') { out += "\\t"; continue; }
            out += c;
        } else {
            if (c == '"') inStr = true;
            out += c;
        }
    }
    return out;
}

// ── PlayerCards.RDconf → cards.db ────────────────────────────
inline int importCards(Database& db, const fs::path& userDir, int& users) {
    auto* cst = db.getCardStorage();
    if (!cst) return 0;
    fs::path file = userDir / "PlayerCards.RDconf";
    if (!fs::exists(file)) return 0;
    int imported = 0;
    legacy2::loadLLMap(file.string(), [&](legacy2::BReader& r, long long uid) {
        legacy2::LegacyPlayer p = legacy2::readPlayer(r);
        if (p.cards.empty()) return;
        ++users;
        std::string user = std::to_string(uid);
        for (auto& c : p.cards) {
            // idx 0 = the default/active card → import as the unnamed default card.
            std::string name = (c.idx == 0) ? std::string() : c.name;
            json attrs = json::object();
            for (auto& [k, v] : c.attrs) {
                if (k.rfind("__", 0) == 0) continue;          // internal meta/stats
                if (k.rfind("&", 0) == 0) continue;           // weapon expressions (skip)
                if (v.kind == legacy2::AttrVal::Kind::Int || v.kind == legacy2::AttrVal::Kind::Num)
                    attrs[k] = (int)v.asInt();
            }
            if (attrs.empty() && name.empty()) continue;
            try {
                auto existing = cst->get_all<CharacterCardRow>(
                    orm::where(orm::c(&CharacterCardRow::userId) == user
                               and orm::c(&CharacterCardRow::name) == name));
                if (!existing.empty()) {
                    auto row = existing.front(); row.attrs = attrs.dump(); row.updatedAt = utils::nowIso8601();
                    cst->update(row);
                } else {
                    CharacterCardRow row;
                    row.userId = user; row.groupId = ""; row.name = name;
                    row.attrs = attrs.dump(); row.updatedAt = utils::nowIso8601();
                    cst->insert(row);
                }
                ++imported;
            } catch (...) {}
        }
    });
    return imported;
}

// ── UserConf.dat → player_profiles (nick / trust / favor) ────
inline int importUsers(Database& db, const fs::path& userDir) {
    auto* st = db.getStorage();
    if (!st) return 0;
    fs::path file = userDir / "UserConf.dat";
    if (!fs::exists(file)) return 0;
    int imported = 0;
    legacy2::loadLLMap(file.string(), [&](legacy2::BReader& r, long long uid) {
        legacy2::LegacyUser u = legacy2::readUser(r);
        int trust = 0, favor = 0;
        if (auto it = u.conf.find("trust"); it != u.conf.end()) trust = (int)it->second.asInt();
        if (auto it = u.conf.find("favor"); it != u.conf.end()) favor = (int)it->second.asInt();
        std::string nick;
        if (!u.nicks.empty()) nick = u.nicks.begin()->second;   // any known nick
        if (trust == 0 && favor == 0 && nick.empty()) return;    // nothing useful
        std::string user = std::to_string(uid);
        try {
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == std::string("onebot_v11")
                           and orm::c(&PlayerProfileRow::userId) == user), orm::limit(1));
            if (rows.empty()) {
                PlayerProfileRow row;
                row.platform = "onebot_v11"; row.userId = user; row.nickname = nick;
                row.trustLevel = trust; row.favor = favor; row.createdAt = utils::nowIso8601();
                st->insert(row);
            } else {
                auto row = rows.front();
                if (!nick.empty()) row.nickname = nick;
                if (trust) row.trustLevel = trust;
                if (favor) row.favor = favor;
                st->update(row);
            }
            ++imported;
        } catch (...) {}
    });
    return imported;
}

// ── BlackList.json → banlist (distinct ban/kick offenders) ───
inline int importBlacklist(Database& db, const fs::path& confDir) {
    auto* st = db.getStorage();
    if (!st) return 0;
    std::string body = readFile(confDir / "BlackList.json");
    if (body.empty()) return 0;
    int imported = 0;
    try {
        json arr = json::parse(body);
        if (!arr.is_array()) return 0;
        std::map<std::string, std::string> offenders;   // uid → note
        for (auto& e : arr) {
            std::string type = e.value("type", "");
            if (type != "ban" && type != "kick") continue;
            std::string uid;
            if (e.contains("fromUID")) {
                if (e["fromUID"].is_number()) uid = std::to_string(e["fromUID"].get<long long>());
                else if (e["fromUID"].is_string()) uid = e["fromUID"].get<std::string>();
            }
            if (uid.empty() || uid == "0") continue;
            offenders[uid] = e.value("note", std::string("legacy import"));
        }
        for (auto& [uid, note] : offenders) {
            auto cnt = st->count<BanlistRow>(orm::where(
                orm::c(&BanlistRow::targetType) == 0 and orm::c(&BanlistRow::listType) == 0
                and orm::c(&BanlistRow::targetId) == uid));
            if (cnt == 0) {
                BanlistRow b; b.targetType = 0; b.listType = 0; b.targetId = uid;
                b.reason = utf8Truncate(note, 200); b.createdAt = utils::nowIso8601();
                st->insert(b); ++imported;
            }
        }
    } catch (...) {}
    return imported;
}

// ── CustomMsgReply.json → reply_rules (via ReplyManager) ─────
// 原版 DiceMsgReply::writeJson 的真实格式（见 Dice-Old DiceMsgReply.cpp）：
//   match[]/prefix[]/search[]/regex[]  四种模式各一个词表（可并存，任一命中）
//   mode+keyword                       更老的单模式写法（keyword 按 | 分行，Regex 除外）
//   echo: Text|Deck|Lua|JS|Py          回复形式；answer: 字符串或数组（Deck）
//   limit: "prob:30;cd:15;grp_id:!123&456;…"  触发限制串
// 以前只认 match[]（完全匹配）：原版的前缀/包含/正则规则、mode 写法、触发限制
// 全部被丢弃。Lua/JS/Py 代码型回复无法转成文本规则，跳过并留日志。
inline int importReplies(ReplyManager& reply, const fs::path& confDir) {
    std::string body = readFile(confDir / "CustomMsgReply.json");
    if (body.empty()) return 0;
    int imported = 0, skippedCode = 0;
    try {
        json obj = json::parse(body);
        if (!obj.is_object()) return 0;
        // Dedup against existing rules by first-condition content (idempotent re-import).
        std::set<std::string> existing;
        for (auto& er : *reply.listRules()) existing.insert(er.matchContent);
        for (auto& [key, r] : obj.items()) {
            if (!r.is_object()) continue;
            // Lua/JS/Py 代码回复导不进文本规则 → 跳过（保留在原文件里，可手工移植为插件）。
            std::string echo = r.value("echo", std::string("Text"));
            if (echo == "Lua" || echo == "JS" || echo == "Py") { ++skippedCode; continue; }
            ReplyRule rule; rule.logic = "or"; rule.priority = 100; rule.enabled = true;
            auto addWords = [&rule](const json& arr, MatchType t) {
                if (!arr.is_array()) return;
                for (auto& m : arr) if (m.is_string() && !m.get<std::string>().empty())
                    rule.conditions.push_back({t, m.get<std::string>()});
            };
            if (r.contains("match"))  addWords(r["match"],  MatchType::kKeyword);
            if (r.contains("prefix")) addWords(r["prefix"], MatchType::kPrefix);
            if (r.contains("search")) addWords(r["search"], MatchType::kSearch);
            if (r.contains("regex"))  addWords(r["regex"],  MatchType::kRegex);
            // 更老的 mode+keyword 单模式写法（非 Regex 模式的 keyword 按 | 分多个词）。
            if (rule.conditions.empty() && r.contains("mode")) {
                std::string mode = r.value("mode", std::string("Match"));
                MatchType t = mode == "Prefix" ? MatchType::kPrefix
                            : mode == "Search" ? MatchType::kSearch
                            : mode == "Regex"  ? MatchType::kRegex : MatchType::kKeyword;
                std::string kw = r.value("keyword", key);
                if (t == MatchType::kRegex) { if (!kw.empty()) rule.conditions.push_back({t, kw}); }
                else {
                    std::stringstream ss(kw); std::string w;
                    while (std::getline(ss, w, '|')) if (!w.empty()) rule.conditions.push_back({t, w});
                }
            }
            if (rule.conditions.empty()) continue;
            if (existing.count(rule.conditions[0].content)) continue;   // already present
            // echo Deck = random-pick among answers (the answers ARE the deck);
            // echo Text = the answer template. Either way the answer strings become
            // our results (multi-result = random pick).
            if (r.contains("answer")) {
                if (r["answer"].is_array()) for (auto& a : r["answer"]) { if (a.is_string() && !a.get<std::string>().empty()) rule.results.push_back(a.get<std::string>()); }
                else if (r["answer"].is_string() && !r["answer"].get<std::string>().empty()) rule.results.push_back(r["answer"].get<std::string>());
            }
            if (rule.results.empty()) continue;
            // 触发限制串 → 概率/冷却(+提示语)/每日上限(+提示语)/群范围/用户范围。
            if (r.contains("limit") && r["limit"].is_string()) {
                std::stringstream ls(r["limit"].get<std::string>()); std::string seg;
                // 拆 "15&@echo=提示" 形式：数字子项 → num，@echo= → notice。
                auto parseCntEcho = [](const std::string& v, int& num, std::string& notice) {
                    std::stringstream ss(v); std::string sub;
                    while (std::getline(ss, sub, '&')) {
                        if (sub.rfind("@echo=", 0) == 0) notice = sub.substr(6);
                        else { try { num = (std::max)(0, std::stoi(sub)); } catch (...) {} }
                    }
                };
                auto parseIdList = [](const std::string& v, std::string& mode, std::string& ids) {
                    bool neg = !v.empty() && v[0] == '!';
                    std::string list = neg ? v.substr(1) : v;
                    for (auto& ch : list) if (ch == '&') ch = ',';   // 原版 & 分隔 → 逗号
                    if (!list.empty()) { mode = neg ? "deny" : "allow"; ids = list; }
                };
                while (std::getline(ls, seg, ';')) {
                    size_t colon = seg.find(':');
                    if (colon == std::string::npos) continue;
                    std::string k = seg.substr(0, colon), v = seg.substr(colon + 1);
                    if (k == "prob") {
                        try { rule.prob = (std::clamp)(std::stoi(v), 0, 100); } catch (...) {}
                    } else if (k == "cd") {
                        int n = 0; parseCntEcho(v, n, rule.cooldownNotice);
                        rule.cooldownSec = n;
                    } else if (k == "today") {
                        int n = 0; parseCntEcho(v, n, rule.dayLimitNotice);
                        rule.dayLimit = n;
                    } else if (k == "grp_id") {
                        parseIdList(v, rule.scopeMode, rule.scopeIds);
                    } else if (k == "user_id") {
                        parseIdList(v, rule.scopeUsersMode, rule.scopeUsers);
                    }
                }
            }
            if (reply.addRule(rule) >= 0) { existing.insert(rule.conditions[0].content); ++imported; }
        }
    } catch (...) {}
    if (skippedCode > 0)
        DICE_LOG_INFO("importReplies: {} 条 Lua/JS/Py 代码型回复无法转为文本规则，已跳过", skippedCode);
    return imported;
}

// ── CustomHelp.json → i18n help.topic.* overrides ────────────
inline int importHelp(Database& db, I18n& i18n, const fs::path& confDir) {
    auto* st = db.getStorage();
    if (!st) return 0;
    std::string body = readFile(confDir / "CustomHelp.json");
    if (body.empty()) return 0;
    int imported = 0;
    try {
        json obj = json::parse(sanitizeJsonControls(body));
        if (!obj.is_object()) return 0;
        for (auto& [topic, val] : obj.items()) {
            if (!val.is_string()) continue;
            std::string key = "help.topic." + topic;
            std::string text = val.get<std::string>();
            // Import to Simplified Chinese only; other locales keep built-in i18n.
            {
                const char* loc = "zh-Hans";
                auto rows = st->get_all<I18nOverrideRow>(
                    orm::where(orm::c(&I18nOverrideRow::locale) == std::string(loc)
                               and orm::c(&I18nOverrideRow::key) == key));
                if (rows.empty()) { I18nOverrideRow ro; ro.locale = loc; ro.key = key; ro.value = text; st->insert(ro); }
                else { auto ro = rows.front(); ro.value = text; st->update(ro); }
                i18n.setOverride(localeFromString(loc), key, text);
            }
            // 清掉繁中/英文对该 help 键的覆盖，回退到自带 i18n（幂等）。
            for (const char* other : {"zh-Hant", "en"}) {
                st->remove_all<I18nOverrideRow>(
                    orm::where(orm::c(&I18nOverrideRow::locale) == std::string(other)
                               and orm::c(&I18nOverrideRow::key) == key));
                i18n.clearOverride(localeFromString(other), key);
            }
            ++imported;
        }
    } catch (...) {}
    return imported;
}

// Original Dice!V2 GlobalMsg key (strXXX) → DiceNext i18n key. Only keys with a
// clean equivalent; the rest have no 1:1 slot (different architecture).
inline const std::map<std::string, std::string>& msgKeyMap() {
    static const std::map<std::string, std::string> M = {
        // greetings / events
        {"strAddFriend", "event.friend_welcome"}, {"strAddGroup", "event.group_joined"},
        // bot on/off
        {"strBotOn", "bot.on"}, {"strBotOff", "bot.off"},
        {"strBotOnAlready", "bot.already_on"}, {"strBotOffAlready", "bot.already_off"},
        // help
        {"strHlpMsg", "help.main"}, {"strHlpNotFound", "help.unknown"},
        // jrrp / roll crit-fumble
        {"strJrrp", "fun.jrrp"},
        {"strRollCriticalSuccess", "dice.crit"}, {"strRollFumble", "dice.fumble"},
        // deck
        {"strDeckEmpty", "deck.empty"}, {"strDeckNotFound", "deck.no_deck"}, {"strDeckNameEmpty", "deck.usage"},
        {"strDeckProSet", "deck.default_set"},
        // sanity / name / set
        {"strSanInvalid", "card.sc.invalid_san"},
        {"strNameTooLongErr", "fun.nn.too_long"}, {"strNameNumTooBig", "fun.name.too_many"},
        {"strSetTooBig", "fun.set.invalid"},
        // welcome
        {"strWelcomeMsgUpdateNotice", "welcome.set"}, {"strWelcomeMsgClearNotice", "welcome.off"},
        {"strWelcomeMsgClearErr", "welcome.none"},
        // dismiss
        {"strDismiss", "dismiss.leaving"},
        // game log (.log new/on/off/end)
        {"strLogNew", "log.new"}, {"strLogOn", "log.on"}, {"strLogOff", "log.off"}, {"strLogEnd", "log.ended"},
        // 全局 / 外置 停用提示 (console)
        {"strGlobalOff", "gate.global_silent"},
        {"strDisabledJrrpGlobal", "gate.jrrp_global"}, {"strDisabledMeGlobal", "gate.me_global"},
        // 私聊守卫 / 云黑群组 / 召唤
        {"strDismissPrivate", "dismiss.private"}, {"strWelcomePrivate", "welcome.private"},
        {"strBlackGroup", "event.blacklist_group"}, {"strSummonEmpty", "fun.summon_empty"},
        // 权限拒绝分级
        {"strNotMaster", "gate.not_master"}, {"strNotAdmin", "gate.not_admin"},
        {"strPermissionDeniedErr", "gate.perm_denied"}, {"strSelfPermissionErr", "gate.self_perm"},
        // 规则速查
        {"strRuleNotFound", "rule.not_found"},
    };
    return M;
}
/// Reverse: DiceNext i18n key → original Dice!V2 key (empty if none). Powers the
/// The gray subtext shown under each key in the web command list.
inline std::string v2KeyFor(const std::string& ourKey) {
    for (auto& [orig, our] : msgKeyMap()) if (our == ourKey) return orig;
    return "";
}

// ── CustomMsg.json → i18n overrides (回执/投掷文本) ───────────
// Mapped keys → their DiceNext i18n key (zh-Hans+zh-Hant). UNmapped keys are
// preserved under "legacy.<strXXX>" so the web 「无效文本」 tab can show them for
// manual porting. The file is UTF-8 (NOT GBK) but has raw CR/LF inside strings.
inline int importCustomMsg(Database& db, I18n& i18n, const fs::path& confDir, int& orphans) {
    auto* st = db.getStorage();
    if (!st) return 0;
    std::string raw = readFile(confDir / "CustomMsg.json");
    if (raw.empty()) return 0;
    int imported = 0;
    static const std::set<std::string> SKIP = {"Number", "str", "strHttpCode"};   // metadata, not text
    auto writeOv = [&](const char* loc, const std::string& key, const std::string& text) {
        auto rows = st->get_all<I18nOverrideRow>(
            orm::where(orm::c(&I18nOverrideRow::locale) == std::string(loc)
                       and orm::c(&I18nOverrideRow::key) == key));
        if (rows.empty()) { I18nOverrideRow ro; ro.locale = loc; ro.key = key; ro.value = text; st->insert(ro); }
        else { auto ro = rows.front(); ro.value = text; st->update(ro); }
        i18n.setOverride(localeFromString(loc), key, text);
    };
    // 仅简体导入：清掉其他语言对该键的覆盖，让繁中/英文回退到自带 i18n
    // （也修复早期版本误把简体写进 zh-Hant/en 的历史数据，使重复导入幂等）。
    auto clearOtherLocales = [&](const std::string& key) {
        for (const char* loc : {"zh-Hant", "en"}) {
            st->remove_all<I18nOverrideRow>(
                orm::where(orm::c(&I18nOverrideRow::locale) == std::string(loc)
                           and orm::c(&I18nOverrideRow::key) == key));
            i18n.clearOverride(localeFromString(loc), key);
        }
    };
    try {
        json obj = json::parse(sanitizeJsonControls(raw));   // UTF-8, raw CR/LF in strings
        if (!obj.is_object()) return 0;
        const auto& M = msgKeyMap();
        for (auto& [origKey, val] : obj.items()) {
            if (!val.is_string() || SKIP.count(origKey)) continue;
            std::string text = val.get<std::string>();
            if (text.empty()) continue;
            auto it = M.find(origKey);
            if (it != M.end()) {                                  // mapped → real i18n key
                writeOv("zh-Hans", it->second, text);             // 仅导入简体中文，其他语言保留自带 i18n
                clearOtherLocales(it->second);
                ++imported;
            } else {                                              // unmapped → 无效文本
                writeOv("zh-Hans", "legacy." + origKey, text);
                ++orphans;
            }
        }
    } catch (...) {}
    return imported;
}

// ── Console.xml → dice.masters (config) ──────────────────────
inline int importMasters(ConfigManager& cfg, const fs::path& confDir) {
    std::string body = readFile(confDir / "Console.xml");
    if (body.empty()) return 0;
    // Extract every <master>digits</master> (simple, avoids an XML dep).
    json arr = cfg.get<json>("dice/masters", json::array());
    if (!arr.is_array()) arr = json::array();
    auto hasMaster = [&](const std::string& id) {
        for (auto& m : arr) if (m.is_object() && m.value("id", "") == id) return true;
        return false;
    };
    int imported = 0;
    size_t pos = 0;
    const std::string open = "<master>", close = "</master>";
    while ((pos = body.find(open, pos)) != std::string::npos) {
        size_t s = pos + open.size();
        size_t e = body.find(close, s);
        if (e == std::string::npos) break;
        std::string id = body.substr(s, e - s);
        // trim whitespace
        while (!id.empty() && (id.front() == ' ' || id.front() == '\t' || id.front() == '\r' || id.front() == '\n')) id.erase(id.begin());
        while (!id.empty() && (id.back() == ' ' || id.back() == '\t' || id.back() == '\r' || id.back() == '\n')) id.pop_back();
        if (!id.empty() && !hasMaster(id)) {
            arr.push_back(json{{"platform", "onebot_v11"}, {"id", id}});
            ++imported;
        }
        pos = e + close.size();
    }
    if (imported) { cfg.set<json>("dice/masters", arr); cfg.save(); }
    return imported;
}
// ── <root>/mod/ → data/mod/（Lua 模块目录）──────────────────────────
// 模块目录包含 descriptor.json、model.xml、rulebook 和 reply.lua 等资源。
// 或 DiceDir/mod/<名>.json。整棵复制到我们的 data/mod/，启动扫描即自动识别。
// Enhanced with ImportResult return + ImportOptions (skip/overwrite).
inline ImportResult importMods(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult result;
    fs::path src = root / "mod";
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return result;
    fs::path dst = fs::path("data") / "mod";
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::directory_iterator(src, ec)) {
        const fs::path& p = entry.path();
        std::string name = p.filename().string();
        fs::path target = dst / name;
        ImportDetail detail;
        detail.name = name;

        // Check if target already exists
        bool exists = fs::exists(target, ec);
        if (exists && !opts.overwrite) {
            detail.status = "skipped";
            detail.reason = "already exists (overwrite=false)";
            ++result.skipped;
            result.details.push_back(detail);
            continue;
        }

        std::error_code ec2;
        if (entry.is_directory())
            fs::copy(p, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec2);
        else
            fs::copy_file(p, target, fs::copy_options::overwrite_existing, ec2);

        if (!ec2) {
            detail.status = "success";
            ++result.success;
        } else {
            detail.status = "failed";
            detail.reason = ec2.message();
            ++result.failed;
        }
        result.details.push_back(detail);
    }
    return result;
}

// ── <root>/PublicDeck/ → data/decks/（外置牌堆）─────────────────────
// Enhanced with ImportResult return + ImportOptions + JSON format validation.
// Non-.json files are marked as skipped in the result.
inline ImportResult importDecks(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult result;
    fs::path src = root / "PublicDeck";
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return result;
    fs::path dst = fs::path("data") / "decks";
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::recursive_directory_iterator(src, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        ImportDetail detail;
        detail.name = name;

        // Non-JSON files are skipped (not an error)
        if (entry.path().extension() != ".json") {
            detail.status = "skipped";
            detail.reason = "not a .json file";
            ++result.skipped;
            result.details.push_back(detail);
            continue;
        }

        // Validate JSON content and deck structure
        std::string content = readFile(entry.path());
        std::string validationError = validateDeckJson(content);
        if (!validationError.empty()) {
            detail.status = "failed";
            detail.reason = validationError;
            ++result.failed;
            result.details.push_back(detail);
            continue;
        }

        fs::path target = dst / name;
        bool exists = fs::exists(target, ec);
        if (exists && !opts.overwrite) {
            detail.status = "skipped";
            detail.reason = "already exists (overwrite=false)";
            ++result.skipped;
            result.details.push_back(detail);
            continue;
        }

        std::error_code ec2;
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec2);
        if (!ec2) {
            detail.status = "success";
            ++result.success;
        } else {
            detail.status = "failed";
            detail.reason = ec2.message();
            ++result.failed;
        }
        result.details.push_back(detail);
    }
    return result;
}

// ── <root>/user/ChatConf.dat → group_settings  (群配置) ──
// Chat::readb（ManagerSystem.cpp）: 记录体 = [ll 内部ID(丢弃)] + tag 流 + short(-1)。
// 现代格式把配置都写进 tag 11 的 AnysTable（停用指令/rc房规/入群欢迎/许可使用…）；
// tag 0/4=群名，1/2/3=旧式 set/map，10=GBK表，21=ChConf(频道子配置，解析丢弃)。
struct LegacyChat { std::string name; std::map<std::string, legacy2::AttrVal> conf; };
inline LegacyChat readChat(legacy2::BReader& r) {
    LegacyChat c;
    r.rLL();   // 内部 ID（与外层 key 重复，丢弃）
    for (int guard = 0; guard < 100000 && r.good(); ++guard) {
        int16_t tag = r.rShort();
        if (tag == -1) break;
        switch (tag) {
            case 0: c.name = legacy2::gbkToUtf8(r.rStr()); break;
            case 4: c.name = r.rStr(); break;
            case 1: { int16_t n = r.rShort(); for (int i = 0; i < n && r.good(); ++i) {
                          std::string k = legacy2::gbkToUtf8(r.rStr());
                          legacy2::AttrVal v; v.kind = legacy2::AttrVal::Kind::Bool; v.b = true; c.conf[k] = v;
                      } } break;
            case 2: { int16_t n = r.rShort(); for (int i = 0; i < n && r.good(); ++i) {
                          std::string k = legacy2::gbkToUtf8(r.rStr());
                          legacy2::AttrVal v; v.kind = legacy2::AttrVal::Kind::Int; v.i = r.rInt(); c.conf[k] = v;
                      } } break;
            case 3: { int16_t n = r.rShort(); for (int i = 0; i < n && r.good(); ++i) {
                          std::string k = legacy2::gbkToUtf8(r.rStr());
                          legacy2::AttrVal v; v.kind = legacy2::AttrVal::Kind::Str; v.s = legacy2::gbkToUtf8(r.rStr()); c.conf[k] = v;
                      } } break;
            case 11: {  // AnysTable::readb —— 现代格式键存为 UTF-8（写入时已转码），不再转
                auto t = legacy2::readAnysTableImpl(r, false, 0);
                for (auto& kv : t) c.conf[kv.first] = kv.second;
            } break;
            case 10: {  // AnysTable::readgb —— GBK 键
                auto t = legacy2::readAnysTableImpl(r, true, 0);
                for (auto& kv : t) c.conf[kv.first] = kv.second;
            } break;
            case 21: { int16_t n = r.rShort();   // ChConf: map<ll, AnysTable> — 解析丢弃
                       for (int i = 0; i < n && r.good(); ++i) { r.rLL(); legacy2::readAnysTableImpl(r, false, 0); } } break;
            default: return c;   // 未知 tag：无法安全续读，止于此条
        }
    }
    return c;
}

inline void upsertGroupSetting(Database& db, const std::string& platform, const std::string& gid,
                               const std::string& key, const std::string& value) {
    auto* st = db.getStorage(); if (!st) return;
    try {
        auto rows = st->get_all<GroupSettingRow>(orm::where(
            orm::c(&GroupSettingRow::platform) == platform
            and orm::c(&GroupSettingRow::groupId) == gid
            and orm::c(&GroupSettingRow::key) == key));
        if (!rows.empty()) { auto row = rows.front(); row.value = value; st->update(row); }
        else { GroupSettingRow row; row.platform = platform; row.groupId = gid; row.key = key; row.value = value; st->insert(row); }
    } catch (...) {}
}

inline int importChatConf(Database& db, const fs::path& userDir, int& groups) {
    fs::path file = userDir / "ChatConf.dat";
    if (!fs::exists(file)) return 0;
    const std::string K_DISABLE = "\xe5\x81\x9c\xe7\x94\xa8\xe6\x8c\x87\xe4\xbb\xa4"; // 停用指令
    const std::string K_COCRULE = "rc\xe6\x88\xbf\xe8\xa7\x84";                       // rc房规
    const std::string K_WELCOME = "\xe5\x85\xa5\xe7\xbe\xa4\xe6\xac\xa2\xe8\xbf\x8e"; // 入群欢迎
    int settings = 0;
    legacy2::loadLLMap(file.string(), [&](legacy2::BReader& r, long long gid) {
        ++groups;
        LegacyChat c = readChat(r);
        const std::string plat = "onebot_v11";
        const std::string g = std::to_string(gid);
        // 停用指令 → enabled=0（机器人在该群关闭）
        if (auto it = c.conf.find(K_DISABLE); it != c.conf.end()) {
            bool off = it->second.kind == legacy2::AttrVal::Kind::Bool ? it->second.b : it->second.asInt() > 0;
            if (off) { upsertGroupSetting(db, plat, g, "enabled", "0"); ++settings; }
        }
        // rc房规 → cocRule（COC 房规 0-7）
        if (auto it = c.conf.find(K_COCRULE); it != c.conf.end()) {
            upsertGroupSetting(db, plat, g, "cocRule", std::to_string(it->second.asInt())); ++settings;
        }
        // 入群欢迎 → welcome（入群欢迎词）
        if (auto it = c.conf.find(K_WELCOME); it != c.conf.end()) {
            std::string w = it->second.asStr();
            if (!w.empty()) { upsertGroupSetting(db, plat, g, "welcome", w); ++settings; }
        }
    });
    return settings;
}

// ── Orchestrator ─────────────────────────────────────────────
// Enhanced runImport accepts CardDeck* and LuaPluginManager* for
// post-import reload, and ImportOptions for skip/overwrite control.
// The deck/mod results are structured ImportResult objects.
inline json runImport(Database& db, ConfigManager& cfg, I18n& i18n, ReplyManager& reply,
                      const std::string& diceDataDir,
                      CardDeck* deck = nullptr, LuaPluginManager* luaMod = nullptr,
                      const ImportOptions& opts = {}) {
    fs::path root(diceDataDir);
    fs::path userDir = root / "user";
    fs::path confDir = root / "conf";
    if (!fs::exists(root)) return json{{"ok", false}, {"error", "目录不存在: " + diceDataDir}};

    int users = 0;
    int cards = importCards(db, userDir, users);
    int profiles = importUsers(db, userDir);
    int black = importBlacklist(db, confDir);
    int replies = importReplies(reply, confDir);
    int help = importHelp(db, i18n, confDir);
    int orphans = 0;
    int msgs = importCustomMsg(db, i18n, confDir, orphans);
    int masters = importMasters(cfg, confDir);

    // Structured deck/mod import results
    auto deckResult = importDecks(root, opts);
    auto modResult = importMods(root, opts);

    // Reload engines if import succeeded and pointers are provided
    if (deckResult.success > 0 && deck) {
        deck->loadDir("data/decks");
        DICE_LOG_INFO("LegacyImport: reloaded card decks after import");
    }
    if (modResult.success > 0 && luaMod) {
        luaMod->reload();
        DICE_LOG_INFO("LegacyImport: reloaded lua mods after import");
    }

    int chatGroups = 0;
    int chatSettings = importChatConf(db, userDir, chatGroups);

    DICE_LOG_INFO("LegacyImport: cards={} (users={}) profiles={} black={} replies={} help={} msgs={} orphans={} masters={} decks(s/f/sk={}/{}/{}) mods(s/f/sk={}/{}/{}) chatGroups={} chatSettings={}",
                  cards, users, profiles, black, replies, help, msgs, orphans, masters,
                  deckResult.success, deckResult.failed, deckResult.skipped,
                  modResult.success, modResult.failed, modResult.skipped,
                  chatGroups, chatSettings);
    return json{
        {"ok", true},
        {"cards", cards}, {"cardUsers", users}, {"profiles", profiles},
        {"blacklist", black}, {"replies", replies}, {"help", help}, {"msgs", msgs},
        {"orphans", orphans}, {"masters", masters},
        {"decks", deckResult.toJSON()}, {"mods", modResult.toJSON()},
        {"chatGroups", chatGroups}, {"chatSettings", chatSettings}
    };
}

}  // namespace dice::legacyv2
