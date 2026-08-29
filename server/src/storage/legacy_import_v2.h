#pragma once
// ─── Dice!Next — Legacy Dice! V2 [DiceData] importer ─────────
// Reads a real original-Dice! data folder and imports the high-value,
// reliably-mappable data into DiceNext. Binary formats via legacy_dice2.h.
//
//   user/PlayerCards.RDconf → cards.db (character_cards)
//   user/UserConf.dat       → player_profiles (nickname / trust / favor)
//   conf/BlackList.json     → banlist (offenders → user blacklist)
//   conf/CustomMsgReply.json→ reply_rules (multi-condition engine)
//   conf/CustomCensor.json  → dice.censor (sensitive command rules)
//   conf/CustomHelp.json    → i18n overrides (help.topic.*)
//   conf/Console.xml        → dice.masters (config)
//   mod/                    → data/mod/   (Lua 规则/功能 mod — 子系统已兼容)
//   PublicDeck/             → data/decks/ (外置牌堆 — 引擎已兼容)
//   user/ChatConf.dat       → group_settings (群配置: 停用/房规/欢迎词)

#include "database.h"
#include "legacy_dice2.h"
#include "legacy_message_keys.h"
#include "../config/config_manager.h"
#include "../i18n/i18n.h"
#include "../core/reply/reply_manager.h"
#include "../common/utils.h"
#include "../common/logger.h"
#include "../service/sensitive_word_filter.h"

#include <nlohmann/json.hpp>
#include <sqlite_orm/sqlite_orm.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>

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

/// Select the same primary/backup order used by the old Dice! loader.  A
/// migration must not report "0 imported" merely because the primary file is
/// damaged while its automatic backup is intact.
inline fs::path legacyDataFile(const fs::path& dir, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        fs::path candidate = dir / name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
    }
    return {};
}

inline std::string legacyScalar(const legacy2::AttrVal& value) {
    switch (value.kind) {
        case legacy2::AttrVal::Kind::Bool: return value.b ? "1" : "0";
        case legacy2::AttrVal::Kind::Int:
        case legacy2::AttrVal::Kind::Num:  return std::to_string(value.asInt());
        case legacy2::AttrVal::Kind::Str:  return value.s;
        default: return {};
    }
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

inline void upsertUserSetting(Database& db, const std::string& uid, const std::string& gid,
                              const std::string& key, const std::string& value) {
    auto* st = db.getStorage(); if (!st) return;
    try {
        auto rows = st->get_all<UserSettingRow>(orm::where(
            orm::c(&UserSettingRow::userId) == uid
            and orm::c(&UserSettingRow::groupId) == gid
            and orm::c(&UserSettingRow::key) == key));
        if (rows.empty()) { UserSettingRow row; row.userId = uid; row.groupId = gid; row.key = key; row.value = value; st->insert(row); }
        else { auto row = rows.front(); row.value = value; st->update(row); }
    } catch (...) {}
}

// ── PlayerCards.RDconf → cards.db ────────────────────────────
inline int importCards(Database& db, const fs::path& userDir, int& users) {
    auto* cst = db.getCardStorage();
    if (!cst) return 0;
    auto* mainSt = db.getStorage();
    std::vector<std::tuple<std::string, std::string, std::string>> bindings;
    fs::path file = legacyDataFile(userDir, {"PlayerCards.RDconf", "PlayerCards.RDconf.bak", "PlayerCards.bak"});
    if (file.empty()) return 0;
    int imported = 0;
    try {
        cst->transaction([&] {
            legacy2::loadLLMap(file.string(), [&](legacy2::BReader& r, long long uid) {
        legacy2::LegacyPlayer p = legacy2::readPlayer(r);
        if (p.cards.empty()) return;
        ++users;
        std::string user = std::to_string(uid);
        std::map<unsigned short, std::string> cardNames;
        for (auto& c : p.cards) {
            // idx 0 = the default/active card → import as the unnamed default card.
            std::string name = (c.idx == 0) ? std::string() : c.name;
            json attrs = json::object();
            json meta = json::object();
            json texts = json::object(), expressions = json::object();
            for (auto& [k, v] : c.attrs) {
                if (k == "__Name" || k == "__Type") continue;
                if (k.rfind("&", 0) == 0) {                   // old inline dice expression
                    if (v.kind == legacy2::AttrVal::Kind::Str) expressions[k.substr(1)] = v.s;
                    continue;
                }
                if (v.kind == legacy2::AttrVal::Kind::Int || v.kind == legacy2::AttrVal::Kind::Num)
                    attrs[k] = (int)v.asInt();
                else if (v.kind == legacy2::AttrVal::Kind::Bool)
                    attrs[k] = v.b ? 1 : 0;
                else if (v.kind == legacy2::AttrVal::Kind::Str)
                    texts[k] = v.s;
            }
            for (auto& [k, v] : c.diceExp) expressions[k] = v;
            for (auto& [k, v] : c.info) texts[k] = v;
            if (!c.note.empty()) texts["note"] = c.note;
            if (!texts.empty()) meta["texts"] = texts;
            if (!expressions.empty()) meta["legacyExpressions"] = expressions;
            if (!c.type.empty()) meta["legacyType"] = c.type;
            if (!c.locks.empty()) {
                std::string locks;
                for (auto& lock : c.locks) {
                    if (lock.empty()) continue;
                    if (!locks.empty()) locks += ',';
                    locks += lock;
                }
                if (!locks.empty()) meta["locks"] = locks;
            }
            if (!meta.empty()) attrs["__meta"] = meta;
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
                cardNames[c.idx] = name;
                ++imported;
            } catch (...) {}
        }
        // Old Player stores a gid → card-index map.  Dice!Next stores the same
        // concept as UserSettingRow(cardBind), so this is a direct conversion.
        for (auto& [gid, idx] : p.groupBind) {
            auto it = cardNames.find(idx);
            if (it != cardNames.end()) bindings.emplace_back(user, std::to_string(gid), it->second);
        }
            });
            return true;
        });
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("LegacyImport: card transaction failed: {}", e.what());
    }
    if (mainSt && !bindings.empty()) {
        try {
            mainSt->transaction([&] {
                for (const auto& [user, group, card] : bindings)
                    upsertUserSetting(db, user, group, "cardBind", card);
                return true;
            });
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("LegacyImport: card binding transaction failed: {}", e.what());
        }
    }
    return imported;
}

// ── UserConf.dat → player_profiles + Lua-compatible user config ─
inline int importUsers(Database& db, const fs::path& userDir, LuaPluginManager* luaMod = nullptr) {
    auto* st = db.getStorage();
    if (!st) return 0;
    fs::path file = legacyDataFile(userDir, {"UserConf.dat", "UserConf.dat.bak", "UserConf.bak"});
    if (file.empty()) return 0;
    int imported = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> luaValues;
    try {
        st->transaction([&] {
            legacy2::loadLLMap(file.string(), [&](legacy2::BReader& r, long long uid) {
        legacy2::LegacyUser u = legacy2::readUser(r);
        int trust = 0, favor = 0;
        if (auto it = u.conf.find("trust"); it != u.conf.end()) trust = (int)it->second.asInt();
        if (auto it = u.conf.find("favor"); it != u.conf.end()) favor = (int)it->second.asInt();
        std::string nick;
        if (!u.nicks.empty()) nick = u.nicks.begin()->second;   // any known nick
        bool hasScalarConfig = std::any_of(u.conf.begin(), u.conf.end(), [](const auto& kv) {
            const auto kind = kv.second.kind;
            return !kv.first.empty() && (kind == legacy2::AttrVal::Kind::Bool
                || kind == legacy2::AttrVal::Kind::Int || kind == legacy2::AttrVal::Kind::Num
                || kind == legacy2::AttrVal::Kind::Str);
        });
        // A user with only Lua/plugin configuration is still meaningful.  The
        // previous early return silently dropped all such settings.
        if (trust == 0 && favor == 0 && nick.empty() && !hasScalarConfig) return;
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
            // Keep every scalar old User::AnysTable field in the compatibility
            // store.  Old Lua getUserConf(uid,key) maps to this exact scope.
            // Structured/function values cannot be faithfully represented by
            // the old decoder and are deliberately not fabricated.
            if (luaMod) for (auto& [key, value] : u.conf) {
                std::string scalar = legacyScalar(value);
                if (!key.empty() && (value.kind == legacy2::AttrVal::Kind::Bool
                        || value.kind == legacy2::AttrVal::Kind::Int
                        || value.kind == legacy2::AttrVal::Kind::Num
                        || value.kind == legacy2::AttrVal::Kind::Str))
                    luaValues.emplace_back("u:" + user, key, std::move(scalar));
            }
            // Old NN/Nick is per-group.  Preserve it as the current .nn
            // setting rather than collapsing all group cards into one profile.
            for (auto& [gid, nickForGroup] : u.nicks)
                if (!nickForGroup.empty()) upsertUserSetting(db, user, std::to_string(gid), "nick", nickForGroup);
            ++imported;
        } catch (...) {}
            });
            return true;
        });
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("LegacyImport: user transaction failed: {}", e.what());
    }
    if (luaMod && !luaValues.empty() && !luaMod->confSetBatch(luaValues)) {
        DICE_LOG_ERROR("LegacyImport: failed to batch-import {} Lua user settings", luaValues.size());
    }
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
        st->transaction([&] {
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
            return true;
        });
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
    // 与旧版源码（DiceMod.cpp custom_reply 段）一致：
    // CustomMsgReply.json 非空才读取；不存在或为空时回退迁移 CustomReply.json
    // （完全匹配）与 CustomRegexReply.json（正则），迁移后旧版会写回
    // CustomMsgReply.json。CustomMsg.json 是独立的全局文案，不参与本判断。
    std::string body = readFile(confDir / "CustomMsgReply.json");
    bool hasModernRules = false;
    try { auto modern = json::parse(sanitizeJsonControls(body)); hasModernRules = modern.is_object() && !modern.empty(); } catch (...) {}
    if (!hasModernRules) {
        json legacy = json::object();
        auto addLegacy = [&](const char* filename, const char* matchKey, bool isRegex) {
            try {
                json source = json::parse(sanitizeJsonControls(readFile(confDir / filename)));
                if (!source.is_object()) return;
                for (auto& [key, answers] : source.items()) {
                    if (key.empty() || !answers.is_array()) continue;
                    json rule;
                    rule[matchKey] = json::array({key});
                    rule["echo"] = "Deck";
                    rule["answer"] = answers;
                    // Keep both legacy files even if a keyword happens to be
                    // identical; the suffix makes the JSON object key unique.
                    legacy[key + (isRegex ? "#legacy-regex" : "#legacy")] = std::move(rule);
                }
            } catch (...) {}
        };
        addLegacy("CustomReply.json", "match", false);
        addLegacy("CustomRegexReply.json", "regex", true);
        if (legacy.empty()) return 0;
        body = legacy.dump();
    }
    int imported = 0, skippedCode = 0;
    try {
        json obj = json::parse(body);
        if (!obj.is_object()) return 0;
        // 完整规则级去重（条件集合+回复+触发限制都相同才算重复）：既能幂等重导，
        // 也不会像旧的“只按首条件”那样误杀“同匹配词、不同回复”的规则；
        // 同时剔除历史 Dice 文件里因旧版 bug 产生的完全重复条目。
        auto fingerprint = [](const ReplyRule& r) -> std::string {
            std::string fp;
            std::vector<std::pair<int, std::string>> conds;
            conds.reserve(r.conditions.size());
            for (const auto& c : r.conditions) conds.emplace_back(static_cast<int>(c.type), c.content);
            std::sort(conds.begin(), conds.end());
            for (const auto& [t, c] : conds) { fp += std::to_string(t); fp += ':'; fp += c; fp += '\x1f'; }
            fp += '|';
            std::vector<std::string> rs = r.results; std::sort(rs.begin(), rs.end());
            for (const auto& x : rs) { fp += x; fp += '\x1f'; }
            fp += '|' + std::to_string(r.prob) + '|' + std::to_string(r.cooldownSec)
                + '|' + std::to_string(r.dayLimit) + '|' + r.scopeMode + '|' + r.scopeIds
                + '|' + r.scopeUsersMode + '|' + r.scopeUsers + '|' + r.cooldownNotice
                + '|' + r.dayLimitNotice + '|' + r.logic;
            return fp;
        };
        std::set<std::string> existing;
        for (auto& er : *reply.listRules()) existing.insert(fingerprint(er));
        int skippedDup = 0;
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
            if (existing.count(fingerprint(rule))) { ++skippedDup; continue; }   // 完整重复 → 跳过
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
            if (reply.addRule(rule) >= 0) { existing.insert(fingerprint(rule)); ++imported; }
        }
        if (skippedDup > 0)
            DICE_LOG_INFO("importReplies: 跳过 {} 条完全重复的自定义回复（历史重复/重复导入）", skippedDup);
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

// Definitions moved to legacy_message_keys.h so live `.strXXX` compatibility
// and migration share one audited map. The former block remains excluded as
// nearby rationale for mappings which intentionally have no one-to-one slot.
#if 0
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
        // jrrp / roll results.  These entries have the same rendering context
        // in both versions (or are static text), so an override remains useful.
        {"strJrrp", "fun.jrrp"},
        {"strRollCriticalSuccess", "dice.crit"}, {"strRollFumble", "dice.fumble"},
        {"strCriticalSuccess", "dice.level.critical"}, {"strExtremeSuccess", "dice.level.extreme"},
        {"strHardSuccess", "dice.level.hard"}, {"strSuccess", "dice.level.regular"},
        {"strFailure", "dice.level.failure"}, {"strFumble", "dice.level.fumble"},
        {"strRollDice", "dice.roll.result"}, {"strRollDiceReason", "dice.roll.result_reason"},
        // deck
        {"strDeckEmpty", "deck.empty"}, {"strDeckNotFound", "deck.no_deck"}, {"strDeckNameEmpty", "deck.usage"},
        {"strDeckProSet", "deck.default_set"},
        // sanity / name / set
        {"strSanInvalid", "card.sc.invalid_san"},
        {"strNameTooLongErr", "fun.nn.too_long"}, {"strNameNumTooBig", "fun.name.too_many"},
        {"strNameNumCannotBeZero", "fun.name.too_many"},
        {"strSetTooBig", "fun.set.invalid"}, {"strSetCannotBeZero", "fun.set.invalid"},
        // welcome
        {"strWelcomeMsgUpdateNotice", "welcome.set"}, {"strWelcomeMsgClearNotice", "welcome.off"},
        {"strWelcomeMsgClearErr", "welcome.none"},
        // dismiss
        {"strDismiss", "dismiss.leaving"},
        // game log: only new/on still receive the legacy log name parameter.
        // The newer off/end callbacks only provide a message count, so mapping
        // the old templates would leave unresolved placeholders in replies.
        {"strLogNew", "log.new"}, {"strLogOn", "log.on"},
        // 全局 / 外置 停用提示 (console)
        {"strGlobalOff", "gate.global_silent"},
        {"strDisabledJrrpGlobal", "gate.jrrp_global"}, {"strDisabledMeGlobal", "gate.me_global"},
        // 私聊守卫 / 云黑群组 / 召唤
        {"strDismissPrivate", "dismiss.private"}, {"strWelcomePrivate", "welcome.private"},
        {"strBlackGroup", "event.blacklist_group"}, {"strSummonEmpty", "fun.summon_empty"},
        {"strLeaveUnused", "event.leave_unused"}, {"strMEDisabledErr", "me.disabled"},
        // 权限拒绝分级
        {"strNotMaster", "gate.not_master"}, {"strNotAdmin", "gate.not_admin"},
        {"strPermissionDeniedErr", "gate.perm_denied"}, {"strSelfPermissionErr", "gate.self_perm"},
        // 规则速查
        {"strRuleNotFound", "rule.not_found"},
        {"strPropNotFound", "card.attr_missing"},
        // 团务：新版保留了这些回执的同名语义与参数。
        {"strGameNew", "game.new"}, {"strGameAreaOpen", "game.area_open"},
        {"strGameAreaClosed", "game.area_closed"}, {"strGameMasterDenied", "game.master_denied"},
        {"strGameMastered", "game.mastered"}, {"strGameMasterList", "game.master_list"},
        {"strGameNotExist", "game.not_exist"}, {"strGameVoidHere", "game.void_here"},
        {"strGameNotMaster", "game.not_master"}, {"strGameItemSet", "game.item_set"},
        {"strGameItemShow", "game.item_show"}, {"strGameItemEmpty", "game.item_empty"},
        {"strGameJoined", "game.joined"}, {"strGamePlayerAlready", "game.player_already"},
        {"strGameExited", "game.exited"}, {"strGameNotJoined", "game.not_joined"},
        {"strGamePlayerEmpty", "game.player_empty"}, {"strGameKicked", "game.kicked"},
        {"strGameKickNotPlayer", "game.kick_not_player"}, {"strGameOver", "game.over"},
        {"strGameRouletteSet", "game.roulette_set"}, {"strGameRouletteHistory", "game.roulette_hist"},
        {"strGameRouletteEmpty", "game.roulette_empty"}, {"strGameRouletteReset", "game.roulette_reset"},
        {"strGameRouletteClear", "game.roulette_clear"}, {"strGameRouletteTooBig", "game.roulette_too_big"},
        // 旁观功能的新版回执与旧版参数也完全一致。
        {"strObEnter", "ob.joined"}, {"strObEnterAlready", "ob.already"},
        {"strObExit", "ob.exit"}, {"strObExitAlready", "ob.not_in"},
        // strObList is intentionally not mapped: the newer list slot also
        // owns the rendered member list/count, whereas the old override was
        // only an introductory line.
        {"strObListEmpty", "ob.empty"}, {"strObListClr", "ob.cleared"},
        {"strObOn", "ob.on"}, {"strObOff", "ob.off"},
        {"strObOnAlready", "ob.on_already"}, {"strObOffAlready", "ob.off_already"},
    };
    return M;
}

// The two projects used a small number of different placeholder spellings.
// Do this only for slots whose runtime arguments have been audited above;
// unsupported placeholders are deliberately left in the legacy bucket.
inline std::string normalizeLegacyTemplate(const std::string&, std::string text) {
    const std::pair<const char*, const char*> generic[] = {
        {"{pc}", "{nick}"}, {"{strSelfName}", "{self}"},
        {"{deck_name}", "{name}"}, {"{game.log_name}", "{name}"},
    };
    for (const auto& [from, to] : generic) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return text;
}
/// Reverse: DiceNext i18n key → original Dice!V2 key (empty if none). Powers the
/// The gray subtext shown under each key in the web command list.
inline std::string v2KeyFor(const std::string& ourKey) {
    for (auto& [orig, our] : msgKeyMap()) if (our == ourKey) return orig;
    return "";
}
#endif

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
                writeOv("zh-Hans", it->second, normalizeLegacyTemplate(origKey, text));
                                                                    // 仅导入简体中文，其他语言保留自带 i18n
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

inline std::string trimConsoleScalar(std::string value) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && space(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\''))) value = value.substr(1, value.size() - 2);
    return value;
}

/// Parse the integer map written by original Dice! under `config:` (YAML) or
/// `<conf><item>Key=Value</item>...</conf>` (legacy XML).
inline std::map<std::string, int> parseConsoleSettings(const std::string& body, bool xml) {
    std::map<std::string, int> out;
    auto add = [&](std::string key, std::string value) {
        key = trimConsoleScalar(std::move(key)); value = trimConsoleScalar(std::move(value));
        if (key.empty() || value.empty()) return;
        try { size_t used = 0; int n = std::stoi(value, &used); if (used == value.size()) out[key] = n; }
        catch (...) {}
    };
    if (!xml) {
        bool inConfig = false;
        std::istringstream lines(body); std::string line;
        while (std::getline(lines, line)) {
            auto hash = line.find('#'); if (hash != std::string::npos) line.erase(hash);
            const size_t indent = line.find_first_not_of(" \t");
            std::string clean = trimConsoleScalar(line);
            if (!inConfig) { if (clean == "config:") inConfig = true; continue; }
            if (clean.empty()) continue;
            if (indent == 0) break;
            auto colon = clean.find(':'); if (colon == std::string::npos) continue;
            add(clean.substr(0, colon), clean.substr(colon + 1));
        }
        return out;
    }
    size_t start = body.find("<conf");
    if (start == std::string::npos || (start = body.find('>', start)) == std::string::npos) return out;
    const size_t finish = body.find("</conf>", ++start);
    if (finish == std::string::npos) return out;
    while (start < finish) {
        const size_t open = body.find('<', start); if (open == std::string::npos || open >= finish) break;
        const size_t gt = body.find('>', open + 1); if (gt == std::string::npos || gt >= finish) break;
        if (body[open + 1] == '/') { start = gt + 1; continue; }
        const size_t close = body.find('<', gt + 1); if (close == std::string::npos || close > finish) break;
        std::string scalar = trimConsoleScalar(body.substr(gt + 1, close - gt - 1));
        const size_t eq = scalar.find('=');
        if (eq != std::string::npos) add(scalar.substr(0, eq), scalar.substr(eq + 1));
        start = close + 1;
    }
    return out;
}

// ── Console.xml / console.yaml → current formal settings ──────
inline int importMasters(ConfigManager& cfg, const fs::path& confDir) {
    std::string body = readFile(confDir / "Console.xml");
    if (body.empty()) body = readFile(confDir / "console.xml");  // case-sensitive filesystems
    const bool xml = !body.empty();
    if (body.empty()) body = readFile(confDir / "console.yaml");
    if (body.empty()) return 0;
    // Extract XML master nodes, or the single `master:` scalar from the YAML
    // format used by later Dice!V2 releases.  We intentionally do not import
    // WebUI/Git credentials, scheduled jobs, or arbitrary runtime flags.
    json arr = cfg.get<json>("dice/masters", json::array());
    if (!arr.is_array()) arr = json::array();
    auto hasMaster = [&](const std::string& id) {
        for (auto& m : arr) if (m.is_object() && m.value("id", "") == id) return true;
        return false;
    };
    int imported = 0;
    auto add = [&](std::string id) {
        // trim whitespace
        while (!id.empty() && (id.front() == ' ' || id.front() == '\t' || id.front() == '\r' || id.front() == '\n')) id.erase(id.begin());
        while (!id.empty() && (id.back() == ' ' || id.back() == '\t' || id.back() == '\r' || id.back() == '\n')) id.pop_back();
        if (id.size() >= 2 && ((id.front() == '"' && id.back() == '"') || (id.front() == '\'' && id.back() == '\''))) id = id.substr(1, id.size() - 2);
        if (!std::all_of(id.begin(), id.end(), [](unsigned char c){ return std::isdigit(c); })) return;
        if (!id.empty() && !hasMaster(id)) {
            arr.push_back(json{{"platform", "onebot_v11"}, {"id", id}});
            ++imported;
        }
    };
    if (xml) {
        size_t pos = 0;
        const std::string open = "<master>", close = "</master>";
        while ((pos = body.find(open, pos)) != std::string::npos) {
            size_t s = pos + open.size(), e = body.find(close, s);
            if (e == std::string::npos) break;
            add(body.substr(s, e - s));
            pos = e + close.size();
        }
    } else {
        std::istringstream lines(body); std::string line;
        while (std::getline(lines, line)) {
            auto hash = line.find('#'); if (hash != std::string::npos) line.erase(hash);
            auto colon = line.find(':'); if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            if (key == "master") { add(line.substr(colon + 1)); break; }
        }
    }

    // Consume original Console settings into active Dice!Next capabilities.
    // Obsolete cloud/private/discussion flags are deliberately ignored rather
    // than kept as inert historical records.
    const auto legacy = parseConsoleSettings(body, xml);
    auto get = [&](const char* key) -> std::optional<int> {
        auto it = legacy.find(key); if (it == legacy.end()) return std::nullopt; return it->second;
    };
    auto mapBool = [&](const char* oldKey, const char* newPath) {
        if (auto value = get(oldKey)) { cfg.set<bool>(newPath, *value != 0); ++imported; }
    };
    auto mapInt = [&](const char* oldKey, const char* newPath, int minValue, int maxValue) {
        if (auto value = get(oldKey)) {
            cfg.set<int>(newPath, (std::clamp)(*value, minValue, maxValue)); ++imported;
        }
    };
    mapBool("DisabledGlobal", "dice/silent_global");
    mapBool("DisabledJrrp", "dice/disabled_jrrp");
    mapBool("DisabledMe", "dice/disabled_me");
    mapBool("DisabledDeck", "dice/disabled_deck");
    mapBool("DisabledDraw", "dice/disabled_draw");
    mapBool("DisabledSend", "dice/disabled_send");
    // The old flag is negative (1 = do not listen while off), whereas the
    // current setting is positive (true = an @ mention may still trigger).
    if (auto value = get("DisabledListenAt")) {
        cfg.set<bool>("dice/listen_at_when_off", *value == 0); ++imported;
    }
    mapBool("ListenGroupRequest", "dice/listen_group_request");
    mapBool("ListenGroupAdd", "dice/listen_group_add");
    mapBool("ListenFriendRequest", "dice/listen_friend_request");
    mapBool("ListenFriendAdd", "dice/listen_friend_add");
    mapBool("LeaveBlackQQ", "dice/leave_black_qq");
    mapBool("ReferMsgReply", "dice/quote_reply");
    mapInt("InactiveUserLine", "dice/friend_clean_days", 0, 3650);
    mapInt("GroupClearLimit", "dice/group_clear_limit", 0, 10000);
    mapInt("GroupInvalidSize", "dice/max_group_size", 0, 1000000);
    if (auto allow = get("AllowStranger")) {
        cfg.set<std::string>("events/friend_policy",
            *allow <= 0 ? "whitelist" : *allow == 1 ? "group_used" : "nonblacklist");
        ++imported;
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

// ── conf/LinkList.json → dice/links ────────────────────────────
// Both versions implement the same directed message-forwarding model. Import
// only ordinary QQ group pairs: old private/discuss/channel records do not
// have a safe equivalent in Dice!Next's group-only forwarding command.
inline std::string legacyGroupId(const json& chat) {
    if (!chat.is_object() || chat.contains("chid") || !chat.contains("gid")) return {};
    const auto& id = chat["gid"];
    if (id.is_number_integer() || id.is_number_unsigned()) {
        const std::string value = std::to_string(id.get<long long>());
        return value == "0" ? std::string() : value;
    }
    if (id.is_string()) {
        const std::string s = id.get<std::string>();
        return s != "0" && !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }) ? s : std::string();
    }
    return {};
}

inline int importLinks(ConfigManager& cfg, const fs::path& confDir) {
    json old = json::parse(readFile(confDir / "LinkList.json"), nullptr, false);
    if (!old.is_array()) return 0;
    json links = cfg.get<json>("dice/links", json::array());
    if (!links.is_array()) links = json::array();
    int imported = 0;
    for (const auto& item : old) {
        if (!item.is_object()) continue;
        const std::string home = legacyGroupId(item.value("origin", json{}));
        const std::string target = legacyGroupId(item.value("target", json{}));
        if (home.empty() || target.empty() || home == target) continue;
        std::string mode = item.value("type", std::string("with"));
        if (mode != "with" && mode != "to" && mode != "from") continue;
        bool duplicate = false;
        for (const auto& current : links) if (current.is_object()
                && current.value("platform", std::string("onebot_v11")) == "onebot_v11"
                && current.value("home", std::string()) == home
                && current.value("target", std::string()) == target) { duplicate = true; break; }
        if (duplicate) continue;
        links.push_back(json{{"platform", "onebot_v11"}, {"home", home}, {"target", target},
                             {"mode", mode}, {"active", item.value("linking", true)}});
        ++imported;
    }
    if (imported) { cfg.set<json>("dice/links", links); cfg.save(); }
    return imported;
}

// ── conf/NoticeList.json → dice/notice/windows ────────────────
// Legacy notification windows carry the same QQ user/group target and the
// same bit-mask based severity subscription.  Newer fine-grained event lists
// stay empty so the imported window continues to use its original mask.
inline int importNotices(ConfigManager& cfg, const fs::path& confDir) {
    json old = json::parse(readFile(confDir / "NoticeList.json"), nullptr, false);
    if (!old.is_array()) return 0;
    json notice = cfg.get<json>("dice/notice", json::object());
    if (!notice.is_object()) notice = json::object();
    if (!notice.contains("windows") || !notice["windows"].is_array()) notice["windows"] = json::array();
    int imported = 0;
    for (const auto& item : old) {
        if (!item.is_object()) continue;
        const bool group = item.contains("gid");
        const char* idKey = group ? "gid" : "uid";
        if (!item.contains(idKey)) continue;
        std::string id;
        const auto& rawId = item[idKey];
        if (rawId.is_number_integer() || rawId.is_number_unsigned()) id = std::to_string(rawId.get<long long>());
        else if (rawId.is_string()) id = rawId.get<std::string>();
        if (id.empty() || id == "0" || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        bool duplicate = false;
        for (const auto& current : notice["windows"]) if (current.is_object()
                && current.value("platform", std::string("onebot_v11")) == "onebot_v11"
                && current.value("chat_id", std::string()) == id
                && current.value("is_group", true) == group) { duplicate = true; break; }
        if (duplicate) continue;
        int mask = item.value("type", 15);
        if (mask <= 0) continue;
        notice["windows"].push_back(json{{"platform", "onebot_v11"}, {"chat_id", id},
            {"is_group", group}, {"level_mask", mask}, {"events", json::array()}});
        ++imported;
    }
    if (imported) { cfg.set<json>("dice/notice", notice); cfg.save(); }
    return imported;
}

// ── conf/CustomCensor.json → dice/censor ──────────────────────
// Original values are integer levels 0..5. Existing Dice!Next entries win so a
// repeat migration never overwrites edits already made in the WebUI.
inline int importCensorWords(ConfigManager& cfg, const fs::path& confDir) {
    json old = json::parse(sanitizeJsonControls(readFile(confDir / "CustomCensor.json")),
                           nullptr, false);
    if (!old.is_object()) return 0;
    json current = cfg.get<json>("dice/censor",
        json{{"enabled", false}, {"words", json::object()}});
    if (!current.is_object()) current = json::object();
    if (!current.contains("words") || !current["words"].is_object())
        current["words"] = json::object();

    int imported = 0;
    for (const auto& [word, value] : old.items()) {
        if (!censor::validRuleWord(word) || current["words"].contains(word)) continue;
        int level = static_cast<int>(censor::Level::Warning);
        if (value.is_number_integer()) level = value.get<int>();
        else if (value.is_string())
            level = static_cast<int>(censor::parseLevel(value.get<std::string>()));
        current["words"][word] = censor::clampLevel(level);
        ++imported;
    }
    if (imported) {
        current["enabled"] = true;
        cfg.set("dice/censor", current);
        cfg.save();
    }
    return imported;
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

// ── Legacy `plugin/` → data/plugin/ ───────────────────────────
// Old Dice! distinguishes directory mods and single-file Lua plugins.  The
// latter are loaded by Dice!Next's Lua compatibility manager from data/plugin.
inline ImportResult importPlugins(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult result;
    fs::path src = root / "plugin", dst = fs::path("data") / "plugin";
    std::error_code ec;
    if (!fs::is_directory(src, ec) || ec) return result;
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::directory_iterator(src, ec)) {
        if (ec) break;
        ImportDetail detail; detail.name = entry.path().filename().string();
        fs::path target = dst / entry.path().filename();
        if (fs::exists(target, ec) && !opts.overwrite) {
            detail.status = "skipped"; detail.reason = "already exists (overwrite=false)";
            ++result.skipped; result.details.push_back(std::move(detail)); continue;
        }
        std::error_code copyEc;
        if (entry.is_directory()) fs::copy(entry.path(), target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, copyEc);
        else fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, copyEc);
        if (copyEc) { detail.status = "failed"; detail.reason = copyEc.message(); ++result.failed; }
        else { detail.status = "success"; ++result.success; }
        result.details.push_back(std::move(detail));
    }
    return result;
}

struct LegacyLogContext {
    std::string groupId;
    std::string name;
    std::string gmId;
    std::string gameCode;
    std::string gameName;
    int status = 2;  // completed by default
    std::string createdAt;
};

inline std::string legacyIsoFromEpoch(long long epoch) {
    if (epoch <= 0) return utils::nowIso8601();
    return utils::timestampToIso8601(std::chrono::system_clock::from_time_t((std::time_t)epoch));
}

inline json legacyIdList(const json& source) {
    json out = json::array();
    if (!source.is_array()) return out;
    for (const auto& id : source) {
        if (id.is_string()) out.push_back(id.get<std::string>());
        else if (id.is_number_integer() || id.is_number_unsigned()) out.push_back(std::to_string(id.get<long long>()));
    }
    return out;
}

inline std::vector<std::string> legacySessionAreas(const json& source) {
    std::vector<std::string> areas;
    auto add = [&](const json& v) {
        if (v.is_number_integer() || v.is_number_unsigned()) areas.push_back(std::to_string(v.get<long long>()));
        else if (v.is_string()) areas.push_back(v.get<std::string>());
        else if (v.is_object() && v.contains("gid")) {
            const auto& gid = v["gid"];
            if (gid.is_string()) areas.push_back(gid.get<std::string>());
            else if (gid.is_number_integer() || gid.is_number_unsigned()) areas.push_back(std::to_string(gid.get<long long>()));
        }
    };
    if (source.is_array()) for (const auto& v : source) add(v); else add(source);
    std::sort(areas.begin(), areas.end()); areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
    return areas;
}

inline std::string nextLegacyGameCode(const json& index) {
    static constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> pick(0, sizeof(alphabet) - 2);
    std::string code;
    do {
        code.clear(); code.reserve(16);
        for (int i = 0; i < 16; ++i) code.push_back(alphabet[pick(rng)]);
    } while (index.contains(code));
    return code;
}

/// Convert old user/session/*.json into the same Lua-backed game state used by
/// `.game`.  Every old session gets a fresh non-guessable Dice!Next game code;
/// old session filenames are only retained as metadata, never as credentials.
inline int importSessions(const fs::path& root, LuaPluginManager* luaMod,
                          std::map<std::string, LegacyLogContext>& logContexts) {
    if (!luaMod) return 0;
    const fs::path dir = root / "user" / "session";
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return 0;
    json index = json::parse(luaMod->confGet("game:index", "sessions"), nullptr, false);
    if (!index.is_object()) index = json::object();
    int imported = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> luaValues;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file() || entry.path().extension() != ".json") continue;
        json old = json::parse(readFile(entry.path()), nullptr, false);
        if (!old.is_object()) { DICE_LOG_WARN("LegacyImport: skipped invalid session {}", entry.path().string()); continue; }
        auto areas = legacySessionAreas(old.contains("chats") ? old["chats"] : old.value("room", json{}));
        if (areas.empty()) continue;  // private/non-locatable old rooms have no safe new .game target
        const std::string stem = entry.path().stem().string();
        const std::string sessionId = "legacy-" + stem + "-" + std::to_string((long long)std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const std::string scope = "game:session:" + sessionId;
        const std::string code = nextLegacyGameCode(index);
        const std::string name = old.value("name", stem);
        luaValues.emplace_back(scope, "__name", name);
        luaValues.emplace_back(scope, "__code", code);
        luaValues.emplace_back(scope, "__areas", json(areas).dump());
        if (old.contains("master")) luaValues.emplace_back(scope, "__gms", legacyIdList(old["master"]).dump());
        if (old.contains("player")) luaValues.emplace_back(scope, "__pls", legacyIdList(old["player"]).dump());
        if (old.contains("observer")) luaValues.emplace_back(scope, "legacyObservers", legacyIdList(old["observer"]).dump());
        if (old.contains("roulette") && old["roulette"].is_object()) {
            json roulette = json::object();
            for (auto& [face, value] : old["roulette"].items()) if (value.is_object()) {
                json item{{"copies", value.value("copy", 1)}};
                if (value.contains("pool") && value["pool"].is_array()) item["left"] = value["pool"];
                roulette[face] = std::move(item);
            }
            if (!roulette.empty()) luaValues.emplace_back(scope, "__rou", roulette.dump());
        }
        if (old.contains("data") && old["data"].is_object()) for (auto& [key, value] : old["data"].items()) {
            if (key.empty()) continue;
            luaValues.emplace_back(scope, key, value.is_string() ? value.get<std::string>() : value.dump());
        }
        for (const auto& groupId : areas) luaValues.emplace_back("game:" + groupId, "__session", sessionId);
        index[code] = sessionId;
        LegacyLogContext context;
        context.groupId = areas.front(); context.name = name; context.gameName = name; context.gameCode = code;
        context.createdAt = legacyIsoFromEpoch(old.value("create_time", 0LL));
        if (old.contains("master")) { auto gms = legacyIdList(old["master"]); if (!gms.empty()) context.gmId = gms.front().get<std::string>(); }
        if (old.contains("log") && old["log"].is_object()) {
            const auto& log = old["log"];
            context.name = log.value("name", name);
            context.status = log.value("start", 0LL) > 0 ? (log.value("logging", false) ? 0 : 1) : 2;
            std::string file = log.value("file", std::string());
            if (!file.empty()) logContexts[file] = context;
        }
        ++imported;
    }
    luaValues.emplace_back("game:index", "sessions", index.dump());
    if (!luaMod->confSetBatch(luaValues)) {
        DICE_LOG_ERROR("LegacyImport: failed to batch-import {} game session settings", luaValues.size());
        return 0;
    }
    return imported;
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

inline bool legacyLogHeader(const std::string& line, std::string& sender, std::string& uid, std::string& stamp) {
    const size_t close = line.rfind(") ");
    if (close == std::string::npos || close + 21 != line.size()) return false;
    stamp = line.substr(close + 2);
    if (stamp.size() != 19 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != ' '
        || stamp[13] != ':' || stamp[16] != ':') return false;
    for (size_t i = 0; i < stamp.size(); ++i)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && !std::isdigit((unsigned char)stamp[i])) return false;
    size_t open = line.rfind('(', close);
    if (open == std::string::npos || open == 0 || close <= open + 1) return false;
    sender = line.substr(0, open); uid = line.substr(open + 1, close - open - 1);
    return true;
}

inline std::string legacyLogIso(const std::string& stamp) {
    std::tm tm{}; std::istringstream in(stamp);
    in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (in.fail()) return utils::nowIso8601();
    std::time_t epoch = std::mktime(&tm);
    return epoch > 0 ? legacyIsoFromEpoch((long long)epoch) : utils::nowIso8601();
}

/// Import original text transcripts as editable Dice!Next log rows.  The old
/// format has no message IDs/media metadata; [图片] is therefore preserved as
/// text rather than inventing a broken attachment reference.
inline int importLogs(Database& db, const fs::path& root,
                      const std::map<std::string, LegacyLogContext>& contexts, int& messages) {
    auto* st = db.getLogStorage();
    const fs::path dir = root / "user" / "log";
    std::error_code ec;
    if (!st || !fs::is_directory(dir, ec) || ec) return 0;
    std::set<std::string> importedSources;
    try {
        for (auto& row : st->get_all<GameLogRow>()) {
            auto meta = json::parse(row.customRules, nullptr, false);
            if (meta.is_object() && meta.contains("legacySource") && meta["legacySource"].is_string())
                importedSources.insert(meta["legacySource"].get<std::string>());
        }
    } catch (...) {}
    int imported = 0;
    const int messagesBefore = messages;
    int lastReported = messages;
    const auto started = std::chrono::steady_clock::now();
    bool committed = false;
    try {
        committed = st->transaction([&] {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file() || entry.path().extension() != ".txt") continue;
        const std::string file = entry.path().filename().string();
        if (importedSources.count(file)) continue;
        LegacyLogContext ctx;
        if (auto it = contexts.find(file); it != contexts.end()) ctx = it->second;
        if (ctx.groupId.empty()) {
            std::string stem = entry.path().stem().string();
            std::string prefix = stem.substr(0, stem.find('_'));
            if (!prefix.empty() && prefix.front() == 'g') prefix.erase(prefix.begin());
            const bool numeric = !prefix.empty() && std::all_of(prefix.begin(), prefix.end(), [](unsigned char c){ return std::isdigit(c); });
            ctx.groupId = numeric ? prefix : ("legacy:" + stem);
            ctx.name = stem;
        }
        if (ctx.name.empty()) ctx.name = entry.path().stem().string();
        json meta{{"legacySource", file}};
        if (!ctx.gameCode.empty()) meta["gameCode"] = ctx.gameCode;
        if (!ctx.gameName.empty()) meta["gameName"] = ctx.gameName;
        GameLogRow log;
        log.groupId = ctx.groupId; log.gmId = ctx.gmId; log.name = ctx.name;
        log.players = "[]"; log.customRules = meta.dump(); log.status = ctx.status;
        log.createdAt = ctx.createdAt.empty() ? utils::nowIso8601() : ctx.createdAt;
        int logId = 0;
        try { logId = st->insert(log); } catch (...) { continue; }
        std::istringstream in(readFile(entry.path()));
        std::string line, sender, uid, stamp, content;
        auto flush = [&] {
            if (sender.empty()) return;
            GameLogMessageRow row;
            row.logId = logId; row.sender = sender; row.userId = uid;
            row.content = utils::trim(content); row.createdAt = legacyLogIso(stamp); row.images = "[]";
            try {
                st->insert(row);
                ++messages;
                if (messages - lastReported >= 25000) {
                    DICE_LOG_INFO("LegacyImport: imported {} log messages...", messages - messagesBefore);
                    lastReported = messages;
                }
            } catch (...) {}
            sender.clear(); uid.clear(); stamp.clear(); content.clear();
        };
        while (std::getline(in, line)) {
            // Original Windows builds write CRLF. getline removes only LF;
            // strip the remaining CR before testing the fixed-width header.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string nextSender, nextUid, nextStamp;
            if (legacyLogHeader(line, nextSender, nextUid, nextStamp)) {
                flush(); sender = std::move(nextSender); uid = std::move(nextUid); stamp = std::move(nextStamp);
            } else if (!sender.empty()) {
                if (!content.empty()) content.push_back('\n');
                content += line;
            }
        }
        flush();
        if (ctx.status == (int)GameLogStatus::kActive) {
            upsertGroupSetting(db, "onebot_v11", ctx.groupId, "activeLog", std::to_string(logId));
            upsertGroupSetting(db, "onebot_v11", ctx.groupId, "activeLogName", ctx.name);
        }
        ++imported;
            }
            return true;
        });
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("LegacyImport: log transaction failed: {}", e.what());
    }
    if (!committed) {
        messages = messagesBefore;
        return 0;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    DICE_LOG_INFO("LegacyImport: imported {} logs and {} messages in {} ms", imported, messages - messagesBefore, elapsedMs);
    return imported;
}

inline int importChatConf(Database& db, const fs::path& userDir, int& groups, LuaPluginManager* luaMod = nullptr) {
    fs::path file = legacyDataFile(userDir, {"ChatConf.dat", "ChatConf.dat.bak", "ChatConf.bak"});
    if (file.empty()) return 0;
    auto* st = db.getStorage();
    if (!st) return 0;
    const std::string K_DISABLE = "\xe5\x81\x9c\xe7\x94\xa8\xe6\x8c\x87\xe4\xbb\xa4"; // 停用指令
    const std::string K_COCRULE = "rc\xe6\x88\xbf\xe8\xa7\x84";                       // rc房规
    const std::string K_WELCOME = "\xe5\x85\xa5\xe7\xbe\xa4\xe6\xac\xa2\xe8\xbf\x8e"; // 入群欢迎
    int settings = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> luaValues;
    try {
        st->transaction([&] {
            legacy2::loadLLMap(file.string(), [&](legacy2::BReader& r, long long gid) {
        ++groups;
        LegacyChat c = readChat(r);
        const std::string plat = "onebot_v11";
        const std::string g = std::to_string(gid);
        // Preserve scalar AnysTable values for old Lua getGroupConf(gid,key).
        // Core settings are additionally mapped below when Dice!Next has a
        // first-class equivalent. Nested tables remain intentionally excluded.
        if (luaMod) for (auto& [key, value] : c.conf) {
            if (key.empty()) continue;
            if (value.kind == legacy2::AttrVal::Kind::Bool || value.kind == legacy2::AttrVal::Kind::Int
                    || value.kind == legacy2::AttrVal::Kind::Num || value.kind == legacy2::AttrVal::Kind::Str)
                luaValues.emplace_back("g:" + g, key, legacyScalar(value));
        }
        if (!c.name.empty()) { upsertGroupSetting(db, plat, g, "legacyGroupName", c.name); ++settings; }
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
            return true;
        });
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("LegacyImport: group config transaction failed: {}", e.what());
    }
    if (luaMod && !luaValues.empty() && !luaMod->confSetBatch(luaValues)) {
        DICE_LOG_ERROR("LegacyImport: failed to batch-import {} Lua group settings", luaValues.size());
    }
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

    const auto importStarted = std::chrono::steady_clock::now();
    auto timed = [](const char* stage, auto&& fn) {
        const auto started = std::chrono::steady_clock::now();
        DICE_LOG_INFO("LegacyImport: starting {}", stage);
        auto result = fn();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        DICE_LOG_INFO("LegacyImport: completed {} in {} ms", stage, elapsedMs);
        return result;
    };

    int users = 0;
    int cards = timed("cards", [&] { return importCards(db, userDir, users); });
    int profiles = timed("users", [&] { return importUsers(db, userDir, luaMod); });
    int black = timed("blacklist", [&] { return importBlacklist(db, confDir); });
    int replies = timed("reply rules", [&] { return importReplies(reply, confDir); });
    int help = timed("help", [&] { return importHelp(db, i18n, confDir); });
    int orphans = 0;
    int msgs = timed("custom messages", [&] { return importCustomMsg(db, i18n, confDir, orphans); });
    int masters = timed("masters", [&] { return importMasters(cfg, confDir); });
    int links = timed("links", [&] { return importLinks(cfg, confDir); });
    int notices = timed("notices", [&] { return importNotices(cfg, confDir); });
    int censorWords = timed("censor words", [&] { return importCensorWords(cfg, confDir); });

    // Structured deck/mod/plugin import results.  Old `plugin/` is distinct
    // from `mod/` and must not be silently ignored.
    auto deckResult = timed("decks", [&] { return importDecks(root, opts); });
    auto modResult = timed("Lua mods", [&] { return importMods(root, opts); });
    auto pluginResult = timed("Lua plugins", [&] { return importPlugins(root, opts); });

    std::map<std::string, LegacyLogContext> logContexts;
    int sessions = timed("sessions", [&] { return importSessions(root, luaMod, logContexts); });
    int logMessages = 0;
    int logs = timed("logs", [&] { return importLogs(db, root, logContexts, logMessages); });

    // Reload engines if import succeeded and pointers are provided
    if (deckResult.success > 0 && deck) {
        deck->loadDir("data/decks");
        DICE_LOG_INFO("LegacyImport: reloaded card decks after import");
    }
    if ((modResult.success > 0 || pluginResult.success > 0) && luaMod) {
        luaMod->reload();
        DICE_LOG_INFO("LegacyImport: reloaded Lua mods/plugins after import");
    }

    int chatGroups = 0;
    int chatSettings = timed("group settings", [&] { return importChatConf(db, userDir, chatGroups, luaMod); });
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - importStarted).count();

    DICE_LOG_INFO("LegacyImport: cards={} (users={}) profiles={} black={} replies={} help={} msgs={} orphans={} masters={} links={} notices={} censorWords={} decks(s/f/sk={}/{}/{}) mods(s/f/sk={}/{}/{}) plugins(s/f/sk={}/{}/{}) sessions={} logs={} logMessages={} chatGroups={} chatSettings={}",
                  cards, users, profiles, black, replies, help, msgs, orphans, masters, links, notices, censorWords,
                  deckResult.success, deckResult.failed, deckResult.skipped,
                  modResult.success, modResult.failed, modResult.skipped,
                  pluginResult.success, pluginResult.failed, pluginResult.skipped,
                  sessions, logs, logMessages,
                  chatGroups, chatSettings);
    DICE_LOG_INFO("LegacyImport: all stages completed in {} ms", elapsedMs);
    return json{
        {"ok", true},
        {"cards", cards}, {"cardUsers", users}, {"profiles", profiles},
        {"blacklist", black}, {"replies", replies}, {"help", help}, {"msgs", msgs},
        {"orphans", orphans}, {"masters", masters}, {"links", links}, {"notices", notices}, {"censorWords", censorWords},
        {"decks", deckResult.toJSON()}, {"mods", modResult.toJSON()}, {"plugins", pluginResult.toJSON()},
        {"sessions", sessions}, {"logs", logs}, {"logMessages", logMessages},
        {"chatGroups", chatGroups}, {"chatSettings", chatSettings},
        {"elapsedMs", elapsedMs}
    };
}

}  // namespace dice::legacyv2
