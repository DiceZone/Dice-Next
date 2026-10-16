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
#include "group_account_settings.h"
#include "legacy_dice2.h"
#include "legacy_message_keys.h"
#include "legacy_check_text_upgrade.h"
#include "legacy_import_resources.h"
#include "legacy_blacklist_import.h"
#include "legacy_import_reporting.h"
#include "../core/deck/deck_file_utils.h"
#include "../core/character/card_store.h"
#include "../config/config_manager.h"
#include "../i18n/i18n.h"
#include "../core/reply/reply_manager.h"
#include "../common/utils.h"
#include "../common/logger.h"
#include "../common/legacy_conf_value.h"
#include "../common/session_deck.h"
#include "../common/legacy_reply_references.h"
#include "../core/reply/reply_definition.h"
#include "../service/sensitive_word_filter.h"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
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
    bool overwriteData = false; ///< Explicit opt-in for existing Next database/config values.
    std::string targetPlatform = "onebot_v11";
    std::string targetAdapterId;
    std::string beforeImportBackup; // Server-created snapshot; never accepted as a client path.
    std::map<std::string, std::string> sessionMappings; // Source key -> confirmed existing id or __new__.
};

/// Preserve account isolation and owner edits. Never infer an Official OpenID
/// from a legacy numeric QQ id; callers must select a compatible target.
inline bool upsertImportedGroupSetting(Database& db, const ImportOptions& opts,
                                      const std::string& gid, const std::string& key,
                                      const std::string& value) {
    auto* st = db.getStorage();
    if (!st) throw std::runtime_error("群设置数据库不可用");
    if (!opts.targetAdapterId.empty()) {
        auto rows = st->get_all<GroupAccountSettingRow>(orm::where(
            orm::c(&GroupAccountSettingRow::adapterId) == opts.targetAdapterId and
            orm::c(&GroupAccountSettingRow::groupId) == gid and
            orm::c(&GroupAccountSettingRow::key) == key));
        if (!rows.empty() && !opts.overwriteData) return rows.front().value == value;
        setAccountGroupSetting(*st, opts.targetAdapterId, opts.targetPlatform, gid, gid, key, value);
        return true;
    }
    auto rows = st->get_all<GroupSettingRow>(orm::where(
        orm::c(&GroupSettingRow::platform) == opts.targetPlatform and
        orm::c(&GroupSettingRow::groupId) == gid and orm::c(&GroupSettingRow::key) == key));
    if (!rows.empty()) {
        if (!opts.overwriteData) return rows.front().value == value;
        auto row = rows.front(); row.value = value; st->update(row);
    } else {
        GroupSettingRow row; row.platform = opts.targetPlatform; row.groupId = gid;
        row.key = key; row.value = value; st->insert(row);
    }
    return true;
}

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
    json confirmations = json::array();

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
        j["confirmations"] = confirmations;
        return j;
    }
};

inline void legacyResourceDetail(ImportResult& result, std::string name,
                                 std::string status, std::string reason);

inline void reportUnsupportedValue(const legacy2::AttrVal& value, const std::string& path,
                                   ImportResult* result) {
    if (!result) return;
    if (value.kind == legacy2::AttrVal::Kind::Other) {
        ++result->skipped;
        result->details.push_back({path, "preserved", "旧函数不执行；函数原数据保留在源文件中，迁入结构仅记录类型与长度"});
    }
    for (const auto& [key, child] : value.table) reportUnsupportedValue(child, path + "/" + key, result);
    for (size_t i = 0; i < value.set.size(); ++i) reportUnsupportedValue(value.set[i], path + "/" + std::to_string(i), result);
}

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
inline fs::path legacyDataFile(const fs::path& dir, std::initializer_list<const char*> names,
        const std::function<void(const fs::path&)>& validator = {}, ImportResult* diagnostics = nullptr) {
    std::string errors;
    for (const char* name : names) {
        fs::path candidate = dir / name;
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) || ec) continue;
        try {
            if (validator) validator(candidate);
            if (diagnostics && !errors.empty()) diagnostics->details.push_back({migration_resources::utf8(candidate.filename()), "success", "已从有效备份恢复；主文件未通过完整校验"});
            return candidate;
        } catch (const std::exception& e) {
            if (!errors.empty()) errors += "; ";
            errors += migration_resources::utf8(candidate.filename()) + ": " + e.what();
            if (diagnostics) diagnostics->details.push_back({migration_resources::utf8(candidate.filename()), "warning", e.what()});
        }
    }
    if (!errors.empty()) throw std::runtime_error("旧二进制文件及备份均未通过校验: " + errors);
    return {};
}

inline std::string legacyScalar(const legacy2::AttrVal& value) {
    return value.kind == legacy2::AttrVal::Kind::Other ? std::string() : legacy_conf::encode(value.toJson());
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
    auto* st = db.getStorage(); if (!st) throw std::runtime_error("用户设置数据库不可用");
    auto rows = st->get_all<UserSettingRow>(orm::where(
        orm::c(&UserSettingRow::userId) == uid and orm::c(&UserSettingRow::groupId) == gid
        and orm::c(&UserSettingRow::key) == key));
    if (rows.empty()) { UserSettingRow row; row.userId = uid; row.groupId = gid; row.key = key; row.value = value; st->insert(row); }
    else { auto row = rows.front(); row.value = value; st->update(row); }
}

// ── PlayerCards.RDconf → cards.db ────────────────────────────
inline int importCards(Database& db, const fs::path& userDir, int& users,
        const ImportOptions& opts = {}, ImportResult* diagnostics = nullptr) {
    auto* cst = db.getCardStorage();
    if (!cst) return 0;
    auto* mainSt = db.getStorage();
    std::vector<std::tuple<std::string, std::string, std::string>> bindings;
    std::vector<std::pair<long long, legacy2::LegacyPlayer>> players;
    fs::path file = legacyDataFile(userDir, {"PlayerCards.RDconf", "PlayerCards.RDconf.bak", "PlayerCards.bak"},
        [&](const fs::path& candidate) {
            players = legacy2::readRecords<legacy2::LegacyPlayer>(candidate, legacy2::readPlayer);
        }, diagnostics);
    if (file.empty()) return 0;
    int imported = 0;
    cst->transaction([&] {
      for (const auto& [uid, p] : players) {
        if (p.cards.empty()) continue;
        ++users;
        std::string user = std::to_string(uid);
        std::map<unsigned short, std::string> cardNames;
        for (const auto& c : p.cards) {
            // idx 0 = the default/active card → import as the unnamed default card.
            std::string name = (c.idx == 0) ? std::string() : c.name;
            json attrs = json::object();
            json meta = json::object();
            json texts = json::object(), expressions = json::object();
            if (!c.name.empty()) meta["displayName"] = c.name;
            for (auto& [k, v] : c.attrs) {
                reportUnsupportedValue(v, "card:" + user + "/" + name + "/" + k, diagnostics);
                if (k == "__Name" || k == "__Type") continue;
                if (k.rfind("&", 0) == 0) {                   // old inline dice expression
                    if (v.kind == legacy2::AttrVal::Kind::Str) expressions[k.substr(1)] = v.s;
                    continue;
                }
                if (v.kind == legacy2::AttrVal::Kind::Int || v.kind == legacy2::AttrVal::Kind::Num
                    || v.kind == legacy2::AttrVal::Kind::Bool || v.kind == legacy2::AttrVal::Kind::Table
                    || v.kind == legacy2::AttrVal::Kind::Set || v.kind == legacy2::AttrVal::Kind::Nil)
                    attrs[k] = v.toJson();
                else if (v.kind == legacy2::AttrVal::Kind::Str)
                    texts[k] = v.s;
            }
            for (auto& [k, v] : c.diceExp) expressions[k] = v;
            for (auto& [k, v] : c.info) texts[k] = v;
            if (!c.note.empty()) texts["note"] = c.note;
            if (!texts.empty()) meta["texts"] = texts;
            if (!expressions.empty()) { meta["expressions"] = expressions; meta["legacyExpressions"] = expressions; }
            if (!c.type.empty()) { meta["type"] = c.type; meta["legacyType"] = c.type; }
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
            if (diagnostics) for (const auto& [key, value] : attrs.items()) {
                if (key.rfind("__", 0) == 0) continue;
                const auto canon = CharacterCardStore::canonical(key);
                if (canon != key && attrs.contains(canon) && attrs[canon] != value) {
                    ++diagnostics->skipped;
                    diagnostics->details.push_back({"card:" + user + "/" + name + "/" + key,
                        "preserved", "同义属性与标准字段「" + canon + "」值冲突；两份原值均保留，请确认"});
                }
            }
            if (attrs.empty() && name.empty()) continue;
                auto existing = cst->get_all<CharacterCardRow>(
                    orm::where(orm::c(&CharacterCardRow::userId) == user
                               and orm::c(&CharacterCardRow::name) == name));
                if (!existing.empty()) {
                    if (!opts.overwriteData && json::parse(existing.front().attrs, nullptr, false) != attrs) {
                        if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({"card:" + user + "/" + name, "skipped", "保留现有人物卡；未授权覆盖数据库数据"}); }
                        continue;
                    }
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
        }
        // Old Player stores a gid → card-index map.  Dice!Next stores the same
        // concept as UserSettingRow(cardBind), so this is a direct conversion.
        for (auto& [gid, idx] : p.groupBind) {
            auto it = cardNames.find(idx);
            if (it != cardNames.end()) bindings.emplace_back(user, gid == 0 ? std::string() : std::to_string(gid), it->second);
        }
      }
      return true;
    });
    if (mainSt && !bindings.empty()) {
            mainSt->transaction([&] {
                for (const auto& [user, group, card] : bindings) {
                    const auto current = mainSt->get_all<UserSettingRow>(orm::where(
                        orm::c(&UserSettingRow::userId) == user and orm::c(&UserSettingRow::groupId) == group
                        and orm::c(&UserSettingRow::key) == std::string("cardBind")));
                    if (!opts.overwriteData && !current.empty() && current.front().value != card) {
                        if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({"cardBind:" + user + "/" + group, "skipped", "保留现有选卡绑定"}); }
                        continue;
                    }
                    upsertUserSetting(db, user, group, "cardBind", card);
                }
                return true;
            });
    }
    return imported;
}

// ── UserConf.dat → player_profiles + Lua-compatible user config ─
inline int importUsers(Database& db, const fs::path& userDir, LuaPluginManager* luaMod = nullptr,
        const ImportOptions& opts = {}, ImportResult* diagnostics = nullptr) {
    auto* st = db.getStorage();
    if (!st) return 0;
    std::vector<std::pair<long long, legacy2::LegacyUser>> records;
    fs::path file = legacyDataFile(userDir, {"UserConf.dat", "UserConf.dat.bak", "UserConf.bak", "UserConf.RDconf"},
        [&](const fs::path& candidate) {
            const bool ancient = candidate.filename() == "UserConf.RDconf";
            records = legacy2::readRecords<legacy2::LegacyUser>(candidate, [&](legacy2::BReader& r) {
                return ancient ? legacy2::readOldUser(r) : legacy2::readUser(r);
            });
        }, diagnostics);
    if (file.empty()) return 0;
    int imported = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> luaValues;
    st->transaction([&] {
      for (const auto& [uid, u] : records) {
        for (const auto& [key, value] : u.conf)
            reportUnsupportedValue(value, "userConf:" + std::to_string(uid) + "/" + key, diagnostics);
        int trust = 0, favor = 0;
        auto nativeInt = [&](const char* key) {
            const auto it = u.conf.find(key); if (it == u.conf.end()) return 0;
            const auto value = it->second.asInt();
            if (value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)())
                throw std::runtime_error(std::string("旧玩家字段超出整数范围: ") + key);
            return static_cast<int>(value);
        };
        trust = nativeInt("trust"); favor = nativeInt("favor");
        std::string nick;
        if (auto it = u.nicks.find(0); it != u.nicks.end()) nick = it->second;
        else if (!u.nicks.empty()) nick = u.nicks.begin()->second;
        bool hasScalarConfig = std::any_of(u.conf.begin(), u.conf.end(), [](const auto& kv) {
            const auto kind = kv.second.kind;
            return !kv.first.empty() && (kind == legacy2::AttrVal::Kind::Bool
                || kind == legacy2::AttrVal::Kind::Int || kind == legacy2::AttrVal::Kind::Num
                || kind == legacy2::AttrVal::Kind::Str || kind == legacy2::AttrVal::Kind::Table
                || kind == legacy2::AttrVal::Kind::Set || kind == legacy2::AttrVal::Kind::Nil);
        });
        // A user with only Lua/plugin configuration is still meaningful.  The
        // previous early return silently dropped all such settings.
        if (trust == 0 && favor == 0 && nick.empty() && !hasScalarConfig) continue;
        std::string user = std::to_string(uid);
            auto rows = st->get_all<PlayerProfileRow>(
                orm::where(orm::c(&PlayerProfileRow::platform) == opts.targetPlatform
                           and orm::c(&PlayerProfileRow::userId) == user), orm::limit(1));
            if (rows.empty()) {
                PlayerProfileRow row;
                row.platform = opts.targetPlatform; row.userId = user; row.nickname = nick;
                row.trustLevel = trust; row.favor = favor; row.createdAt = utils::nowIso8601();
                st->insert(row);
            } else if (opts.overwriteData) {
                auto row = rows.front();
                if (!nick.empty()) row.nickname = nick;
                row.trustLevel = trust;
                row.favor = favor;
                st->update(row);
            } else if (diagnostics && (rows.front().trustLevel != trust || rows.front().favor != favor
                    || (!nick.empty() && rows.front().nickname != nick))) {
                ++diagnostics->skipped;
                diagnostics->details.push_back({"profile:" + user, "skipped", "保留现有玩家档案和权限；未授权覆盖数据"});
            }
            // Keep every scalar old User::AnysTable field in the compatibility
            // store.  Old Lua getUserConf(uid,key) maps to this exact scope.
            // Structured/function values cannot be faithfully represented by
            // the old decoder and are deliberately not fabricated.
            if (luaMod) for (const auto& [key, value] : u.conf) {
                // A protected native permission must not be bypassed through
                // a newly-imported compatibility variable read by a Lua plugin.
                if (!opts.overwriteData && !rows.empty() &&
                    ((key == "trust" && rows.front().trustLevel != trust) ||
                     (key == "favor" && rows.front().favor != favor))) {
                    if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({"userConf:" + user + "/" + key,
                        "skipped", "保留新版档案值；不通过兼容变量改变现有权限/好感"}); }
                    continue;
                }
                std::string scalar = legacyScalar(value);
                if (!key.empty() && (value.kind == legacy2::AttrVal::Kind::Bool
                        || value.kind == legacy2::AttrVal::Kind::Int
                        || value.kind == legacy2::AttrVal::Kind::Num
                        || value.kind == legacy2::AttrVal::Kind::Str || value.kind == legacy2::AttrVal::Kind::Table
                        || value.kind == legacy2::AttrVal::Kind::Set || value.kind == legacy2::AttrVal::Kind::Nil)) {
                    const auto current = luaMod->confGet("u:" + user, key);
                    if (!opts.overwriteData && !current.empty()) {
                        if (diagnostics && current != scalar) { ++diagnostics->skipped; diagnostics->details.push_back({"userConf:" + user + "/" + key, "skipped", "保留现有插件变量"}); }
                    } else luaValues.emplace_back("u:" + user, key, std::move(scalar));
                }
            }
            // Old NN/Nick is per-group.  Preserve it as the current .nn
            // setting rather than collapsing all group cards into one profile.
            auto saveSetting = [&](const std::string& group, const std::string& key, const std::string& value) {
                const auto current = st->get_all<UserSettingRow>(orm::where(orm::c(&UserSettingRow::userId) == user
                    and orm::c(&UserSettingRow::groupId) == group and orm::c(&UserSettingRow::key) == key));
                if (!opts.overwriteData && !current.empty() && current.front().value != value) {
                    if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({"userSetting:" + user + "/" + key, "skipped", "保留现有用户设置"}); }
                    return;
                }
                upsertUserSetting(db, user, group, key, value);
            };
            for (const auto& [gid, nickForGroup] : u.nicks)
                if (!nickForGroup.empty()) saveSetting(gid == 0 ? std::string() : std::to_string(gid), "nick", nickForGroup);
            if (auto it = u.conf.find("默认骰"); it != u.conf.end()) {
                const auto face = it->second.asInt();
                if (face > 0 && face <= 1000000) saveSetting("", "defaultDice", std::to_string(face));
                else if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({"defaultDice:" + user, "skipped", "旧默认骰面数超出安全范围"}); }
            }
            ++imported;
      }
      return true;
    });
    if (luaMod && !luaValues.empty() && !luaMod->confSetBatch(luaValues)) {
        throw std::runtime_error("failed to import Lua user configuration");
    }
    return imported;
}

// ── BlackList.json → banlist (distinct ban/kick offenders) ───
inline int importBlacklist(Database& db, const fs::path& confDir, json* diagnostics = nullptr) {
    return importBlacklistSafe(db, confDir, diagnostics);
}

// ── CustomMsgReply.json → reply_rules (via ReplyManager) ─────
// 原版 DiceMsgReply::writeJson 的真实格式（见 Dice-Old DiceMsgReply.cpp）：
//   match[]/prefix[]/search[]/regex[]  四种模式各一个词表（可并存，任一命中）
//   mode+keyword                       更老的单模式写法（keyword 按 | 分行，Regex 除外）
//   echo: Text|Deck|Lua|JS|Py          回复形式；answer: 字符串或数组（Deck）
//   limit: "prob:30;cd:15;grp_id:!123&456;…"  触发限制串
// 以前只认 match[]（完全匹配）：原版的前缀/包含/正则规则、mode 写法、触发限制
// 全部被丢弃。Lua/JS/Py 代码型回复无法转成文本规则，跳过并留日志。
inline int importReplies(ReplyManager& reply, const fs::path& confDir,
                         const legacy_reply_references::Catalog& catalog = {},
                         legacy_reply_references::Report* migrationReport = nullptr, json* diagnostics = nullptr) {
    legacy_reply_references::Report localReport;
    auto& report = migrationReport ? *migrationReport : localReport;
    // 与旧版源码（DiceMod.cpp custom_reply 段）一致：
    // CustomMsgReply.json 非空才读取；不存在或为空时回退迁移 CustomReply.json
    // （完全匹配）与 CustomRegexReply.json（正则），迁移后旧版会写回
    // CustomMsgReply.json。CustomMsg.json 是独立的全局文案，不参与本判断。
    std::string body = reply_import_safety::readSource(confDir / "CustomMsgReply.json");
    json obj = json::object();
    if (!reply_import_safety::trim(body).empty()) {
        obj = json::parse(sanitizeJsonControls(body));
        if (!obj.is_object()) throw std::runtime_error("CustomMsgReply.json 必须是对象");
    }
    if (obj.empty()) {
        auto addLegacy = [&](const char* filename, const char* matchKey, bool isRegex) {
            const auto raw = reply_import_safety::readSource(confDir / filename);
            if (reply_import_safety::trim(raw).empty()) return;
            const auto source = json::parse(sanitizeJsonControls(raw));
            if (!source.is_object()) throw std::runtime_error(std::string(filename) + " 必须是对象");
            for (const auto& [key, answers] : source.items()) {
                if (key.empty() || !answers.is_array()) {
                    reply_import_safety::diagnostic(diagnostics, filename, key, "failed", {"invalid legacy answers"}, answers);
                    continue;
                }
                obj[key + (isRegex ? "#legacy-regex" : "#legacy")] = json{
                    {matchKey, json::array({key})}, {"echo", "Deck"}, {"answer", answers}};
            }
        };
        addLegacy("CustomReply.json", "match", false);
        addLegacy("CustomRegexReply.json", "regex", true);
    }
    if (obj.empty()) return 0;
    int imported = 0, skippedCode = 0;
    try {
        // 完整规则级去重（条件集合+回复+触发限制都相同才算重复）：既能幂等重导，
        // 也不会像旧的“只按首条件”那样误杀“同匹配词、不同回复”的规则；
        // 同时剔除历史 Dice 文件里因旧版 bug 产生的完全重复条目。
        auto fingerprint = [](const ReplyRule& r) -> std::string {
            std::vector<std::pair<int, std::string>> conds;
            conds.reserve(r.conditions.size());
            for (const auto& c : r.conditions) conds.emplace_back(static_cast<int>(c.type), c.content);
            // Condition order affects which prefix/regex supplies captures.
            std::vector<std::pair<std::string, int>> rs;
            for (size_t i = 0; i < r.results.size(); ++i) {
                const auto parsed = weighted_reply::parse(r.results[i]);
                const bool formal = r.resultWeights.size() == r.results.size();
                rs.emplace_back(formal ? r.results[i] : std::string(parsed.text), formal ? r.resultWeights[i] : static_cast<int>(parsed.weight));
            }
            std::sort(rs.begin(), rs.end());
            // Structured encoding avoids collisions with delimiters in authored text.
            return json{{"conditions", conds}, {"results", rs}, {"prob", r.prob},
                {"channelScope", r.channelScope}, {"channelTarget", r.channelTarget},
                {"cooldownSec", r.cooldownSec}, {"dayLimit", r.dayLimit}, {"scopeMode", r.scopeMode},
                {"scopeIds", r.scopeIds}, {"scopeUsersMode", r.scopeUsersMode}, {"scopeUsers", r.scopeUsers},
                {"cooldownNotice", r.cooldownNotice}, {"dayLimitNotice", r.dayLimitNotice}, {"logic", r.logic}}.dump();
        };
        std::set<std::string> existing;
        std::map<std::string, ReplyRule> existingRules;
        std::map<std::string, ReplyRule> pendingRules;
        for (auto& er : *reply.listRules()) {
            existing.insert(fingerprint(er)); existingRules.emplace(fingerprint(er), er);
            if (er.legacyReferences) {
                auto normalized = er; normalized.legacyReferences = false;
                legacy_reply_references::Report ignored;
                for (auto& text : normalized.results) text = legacy_reply_references::migrate(text, catalog, ignored, "", "", normalized.legacyReferences);
                normalized.cooldownNotice = legacy_reply_references::migrate(normalized.cooldownNotice, catalog, ignored, "", "", normalized.legacyReferences);
                normalized.dayLimitNotice = legacy_reply_references::migrate(normalized.dayLimitNotice, catalog, ignored, "", "", normalized.legacyReferences);
                pendingRules.emplace(fingerprint(normalized), er);
            }
        }
        int skippedDup = 0;
        for (auto& [key, r] : obj.items()) {
            if (!r.is_object()) {
                reply_import_safety::diagnostic(diagnostics, "CustomMsgReply.json", key, "failed", {"reply is not an object"}, r);
                continue;
            }
            const auto echo = r.value("echo", std::string("Text"));
            if (echo != "Text" && echo != "Deck") {
                ++skippedCode;
                reply_import_safety::diagnostic(diagnostics, "CustomMsgReply.json", key, "preserved",
                    {"代码或未知回复形式不执行；原稿已保留"}, r);
                if (diagnostics) diagnostics->back()["requiresReview"] = true;
                continue;
            }
            std::vector<std::string> reasons;
            ReplyRule rule = reply_import_safety::convert(key, r, reasons);
            bool valid = !rule.conditions.empty() && !rule.results.empty();
            for (const auto& condition : rule.conditions)
                if (condition.type == MatchType::kRegex && !ReplyMatcher::validateRegex(condition.content)) valid = false;
            if (!valid) {
                reply_import_safety::diagnostic(diagnostics, "CustomMsgReply.json", key, "preserved", reasons, r);
                if (diagnostics) diagnostics->back()["requiresReview"] = true;
                continue;
            }
            const auto originalFingerprint = fingerprint(rule);
            for (size_t i = 0; i < rule.results.size(); ++i)
                rule.results[i] = legacy_reply_references::migrate(rule.results[i], catalog, report, key,
                    "results[" + std::to_string(i) + "]", rule.legacyReferences);
            rule.cooldownNotice = legacy_reply_references::migrate(rule.cooldownNotice, catalog, report, key, "cooldownNotice", rule.legacyReferences);
            rule.dayLimitNotice = legacy_reply_references::migrate(rule.dayLimitNotice, catalog, report, key, "dayLimitNotice", rule.legacyReferences);
            const auto migratedFingerprint = fingerprint(rule);
            if (!rule.enabled) {
                const auto found = existingRules.find(migratedFingerprint);
                if (found != existingRules.end()) {
                    const bool disabled = !found->second.enabled;
                    reply_import_safety::diagnostic(diagnostics, "CustomMsgReply.json", key,
                        disabled ? "disabled" : "conflict", reasons, r, found->second.id, disabled);
                    ++skippedDup;
                    continue; // Existing owner-defined rules cannot be identified as old imports.
                }
            }
            // Re-import can identify previously imported bare templates by a full
            // definition match. Preserve owner-enabled/priority settings when upgrading.
            if (auto old = existingRules.find(originalFingerprint); old != existingRules.end()) {
                if (originalFingerprint == migratedFingerprint && old->second.legacyReferences == rule.legacyReferences) {
                    ++skippedDup; continue;
                }
                rule.enabled = old->second.enabled; rule.priority = old->second.priority;
                if (reply.updateRule(old->second.id, rule)) existing.insert(migratedFingerprint);
                ++skippedDup; continue;
            }
            if (existing.count(migratedFingerprint)) { ++skippedDup; continue; }
            if (auto old = pendingRules.find(migratedFingerprint); old != pendingRules.end()) {
                rule.enabled = old->second.enabled; rule.priority = old->second.priority;
                if (reply.updateRule(old->second.id, rule)) existing.insert(migratedFingerprint);
                ++skippedDup; continue;
            }
            const int id = reply.addRule(rule);
            if (id < 0) throw std::runtime_error("无法保存已校验的旧回复规则");
            existing.insert(migratedFingerprint); ++imported;
            if (!rule.enabled) reply_import_safety::diagnostic(diagnostics, "CustomMsgReply.json", key,
                "disabled", reasons, r, id, true);
        }
        if (skippedDup > 0)
            DICE_LOG_INFO("importReplies: 跳过 {} 条完全重复的自定义回复（历史重复/重复导入）", skippedDup);
    } catch (const std::exception&) { throw; }
    if (skippedCode > 0)
        DICE_LOG_INFO("importReplies: {} 条 Lua/JS/Py 代码型回复无法转为文本规则，已跳过", skippedCode);
    return imported;
}

// ── CustomHelp.json → i18n help.topic.* overrides ────────────
inline int importHelp(Database& db, I18n& i18n, const fs::path& confDir,
                      const ImportOptions& opts = {}, ImportResult* diagnostics = nullptr) {
    auto* st = db.getStorage(); if (!st) throw std::runtime_error("帮助数据库不可用");
    const auto body = readFile(confDir / "CustomHelp.json");
    if (body.empty()) return 0;
    const auto obj = json::parse(sanitizeJsonControls(body));
    if (!obj.is_object()) throw std::runtime_error("CustomHelp.json 必须是对象");
    int imported = 0;
    for (const auto& [topic, value] : obj.items()) {
        if (!value.is_string()) {
            if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({topic, "preserved", "非文本帮助词条未接入"}); }
            continue;
        }
        const auto key = "help.topic." + topic, text = value.get<std::string>();
        auto rows = st->get_all<I18nOverrideRow>(orm::where(
            orm::c(&I18nOverrideRow::locale) == std::string("zh-Hans") and orm::c(&I18nOverrideRow::key) == key));
        if (!rows.empty() && rows.front().value != text && !opts.overwriteData) {
            if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({key, "skipped", "保留已有帮助；未授权覆盖数据"}); }
            continue;
        }
        if (rows.empty()) { I18nOverrideRow row; row.locale = "zh-Hans"; row.key = key; row.value = text; st->insert(row); }
        else { auto row = rows.front(); row.value = text; st->update(row); }
        i18n.setOverride(Locale::kZhHans, key, text);
        // Other locales may contain owner-authored translations. Never clear them.
        ++imported;
    }
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
//
// strSelfName / strSelfCall 不是文案，是骰娘的名字和回执里的自称（原版 GlobalVar.cpp：
// strSelfName 默认空=登录昵称，strSelfCall 默认 "&strSelfName"=同名）。DiceNext 把它们
// 放在设置 dice/self_name、dice/self_call 里。给了 cfg 就迁进去；以前随其他认不得的键
// 进了「无效文本」，迁完骰娘的自称就成了 QQ 昵称。
inline int importCustomMsg(Database& db, I18n& i18n, const fs::path& confDir, int& orphans, json* details = nullptr,
                           const ImportOptions& opts = {}, ConfigManager* cfg = nullptr) {
    auto* st = db.getStorage();
    if (!st) return 0;
    std::string raw = readFile(confDir / "CustomMsg.json");
    if (raw.empty()) return 0;
    int imported = 0;
    bool selfChanged = false;
    static const std::set<std::string> SKIP = {"Number", "str", "strHttpCode"};   // metadata, not text
    auto writeOv = [&](const char* loc, const std::string& key, const std::string& text) {
        auto rows = st->get_all<I18nOverrideRow>(
            orm::where(orm::c(&I18nOverrideRow::locale) == std::string(loc)
                       and orm::c(&I18nOverrideRow::key) == key));
        if (rows.empty()) { I18nOverrideRow ro; ro.locale = loc; ro.key = key; ro.value = text; st->insert(ro); }
        else {
            if (!opts.overwriteData && rows.front().value != text) return false;
            auto ro = rows.front(); ro.value = text; st->update(ro);
        }
        i18n.setOverride(localeFromString(loc), key, text);
        return true;
    };
    try {
        json obj = json::parse(sanitizeJsonControls(raw));   // UTF-8, raw CR/LF in strings
        if (!obj.is_object()) throw std::runtime_error("CustomMsg.json 必须是对象");
        const auto& M = msgKeyMap();
        for (auto& [origKey, val] : obj.items()) {
            if (!val.is_string() || SKIP.count(origKey)) continue;
            std::string text = val.get<std::string>();
            if (text.empty()) continue;
            if (cfg && (origKey == "strSelfName" || origKey == "strSelfCall")) {
                std::string name = text;
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
                const auto first = name.find_first_not_of(" \t\r\n");
                name = first == std::string::npos ? std::string() : name.substr(first);
                // 原版的默认值，在 DiceNext 里就是「没设，跟名字一样」
                if (origKey == "strSelfCall" && name == "&strSelfName") continue;
                // 别的「&键名」引用和 {…} 模板不是名字，照旧留在无效文本里让人看
                if (!name.empty() && name.front() != '&' && name.find('{') == std::string::npos) {
                    const std::string path = origKey == "strSelfName" ? "dice/self_name" : "dice/self_call";
                    const auto current = cfg->get<std::string>(path, std::string());
                    if (current.empty() || current == name || opts.overwriteData) {
                        cfg->set<std::string>(path, name); selfChanged = true; ++imported;
                        if (details) details->push_back({{"source", origKey}, {"target", path},
                            {"status", "active"}, {"issues", json::array()}});
                        continue;
                    }
                    writeOv("zh-Hans", "legacy." + origKey, text); ++orphans;
                    if (details) details->push_back({{"source", origKey}, {"target", path}, {"status", "conflict"},
                        {"issues", {"保留已有自称；未授权覆盖数据"}}, {"original", text}});
                    continue;
                }
            }
            auto it = M.find(origKey);
            if (it != M.end()) {                                  // mapped → real i18n key
                const auto normalized = normalizeLegacyTemplate(origKey, text);
                if (legacy_check_replies::isKey(it->second)) {
                    // Keep the untouched original, even for a successful mapping.
                    writeOv("zh-Hans", "legacy." + origKey, text);
                    auto detail = checkTextDetail(origKey, it->second, normalized);
                    const auto existing = st->get_all<I18nOverrideRow>(orm::where(
                        orm::c(&I18nOverrideRow::locale) == std::string("zh-Hans") and orm::c(&I18nOverrideRow::key) == it->second));
                    if (!opts.overwriteData && !existing.empty() && existing.front().value != normalized) detail["status"] = "conflict";
                    if (detail["status"] == "active" || detail["status"] == "partial") {
                        writeOv("zh-Hans", it->second, normalized); ++imported;
                    } else ++orphans;
                    if (details) details->push_back(detail);
                    continue;
                }
                if (!writeOv("zh-Hans", it->second, normalized)) {
                    ++orphans;
                    if (details) details->push_back({{"source", origKey}, {"target", it->second}, {"status", "conflict"},
                        {"issues", {"保留已有文案；未授权覆盖数据"}}, {"original", text}});
                    continue;
                }
                ++imported;
                if (details) {
                    const auto program = safe_template::compile(normalized);
                    details->push_back({{"source", origKey}, {"target", it->second},
                        {"status", program.issues.empty() ? "active" : "partial"}, {"issues", program.issues}});
                }
            } else {                                              // unmapped → 无效文本
                writeOv("zh-Hans", "legacy." + origKey, text);
                ++orphans;
                if (details) details->push_back({{"source", origKey}, {"target", ""}, {"status", "preserved"}, {"issues", {"no audited mapping"}}});
            }
        }
    } catch (const std::exception&) { throw; }
    if (selfChanged && !cfg->save()) throw std::runtime_error("无法保存迁入的骰娘自称");
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
inline int importMasters(ConfigManager& cfg, const fs::path& confDir,
                         int* settingsImported = nullptr, const ImportOptions& opts = {}, ImportResult* diagnostics = nullptr) {
    if (settingsImported) *settingsImported = 0;
    std::error_code sourceEc;
    const fs::path yamlPath = confDir / "console.yaml";
    const bool hasYaml = fs::exists(yamlPath, sourceEc);
    if (sourceEc) throw std::runtime_error("Cannot inspect legacy console.yaml: " + sourceEc.message());
    std::string body;
    bool xml = !hasYaml;
    if (hasYaml) {
        if (!migration_resources::safePath(confDir, yamlPath)) throw std::runtime_error("Unsafe legacy console.yaml path");
        body = readFile(yamlPath);
        // The original loader gives YAML precedence. Invalid/empty YAML must
        // be reported, never silently replaced by a stale XML permission list.
        auto document = YAML::Load(body);
        if (!document.IsMap()) throw std::runtime_error("Legacy console.yaml must be a YAML mapping");
        if (document["master"] && !document["master"].IsScalar())
            throw std::runtime_error("Legacy console.yaml master must be a scalar account ID");
    } else {
        auto xmlPath = confDir / "Console.xml";
        if (!fs::exists(xmlPath)) xmlPath = confDir / "console.xml";
        if (!fs::exists(xmlPath)) return 0;
        if (!migration_resources::safePath(confDir, xmlPath)) throw std::runtime_error("Unsafe legacy Console.xml path");
        body = readFile(xmlPath);
        if (body.empty() || body.find('<') == std::string::npos)
            throw std::runtime_error("Legacy Console.xml is empty or invalid");
    }
    // Extract XML master nodes, or the single `master:` scalar from the YAML
    // format used by later Dice!V2 releases.  We intentionally do not import
    // WebUI/Git credentials, scheduled jobs, or arbitrary runtime flags.
    json arr = cfg.get<json>("dice/masters", json::array());
    if (!arr.is_array()) arr = json::array();
    auto hasMaster = [&](const std::string& id) {
        for (auto& m : arr) if (m.is_object() && m.value("id", "") == id
                && m.value("platform", std::string("onebot_v11")) == opts.targetPlatform
                && m.value("adapter_id", m.value("adapterId", std::string())) == opts.targetAdapterId) return true;
        return false;
    };
    int imported = 0;
    auto add = [&](std::string id) {
        // trim whitespace
        while (!id.empty() && (id.front() == ' ' || id.front() == '\t' || id.front() == '\r' || id.front() == '\n')) id.erase(id.begin());
        while (!id.empty() && (id.back() == ' ' || id.back() == '\t' || id.back() == '\r' || id.back() == '\n')) id.pop_back();
        if (id.size() >= 2 && ((id.front() == '"' && id.back() == '"') || (id.front() == '\'' && id.back() == '\''))) id = id.substr(1, id.size() - 2);
        if (id.empty() || id == "0") return;
        if (!std::all_of(id.begin(), id.end(), [](unsigned char c){ return std::isdigit(c); }))
            throw std::runtime_error("旧骰主 ID 不是有效的 QQ 数字身份");
        if (!id.empty() && !hasMaster(id)) {
            arr.push_back(json{{"platform", opts.targetPlatform}, {"adapter_id", opts.targetAdapterId}, {"id", id}});
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
        const auto document = YAML::Load(body);
        if (document["master"]) add(document["master"].as<std::string>());
    }

    // Consume original Console settings into active Dice!Next capabilities.
    // Obsolete cloud/private/discussion flags are deliberately ignored rather
    // than kept as inert historical records.
    const auto legacy = parseConsoleSettings(body, xml);
    if ((!xml && YAML::Load(body)["clock"] && !YAML::Load(body)["clock"].IsNull())
        || (xml && body.find("<clock") != std::string::npos)) {
        if (diagnostics) legacyResourceDetail(*diagnostics, "console/clock", "preserved",
            "旧定时任务未自动执行；请在新版定时任务中确认并重新配置，原任务仍保留在旧配置文件中");
    }
    int settings = 0;
    auto maySet = [&](const char* path) {
        // A freshly-created Next configuration is safe to initialize. Existing
        // owner settings require the separate, explicit data-overwrite choice.
        if (opts.overwriteData || cfg.createdOnLoad() || !fs::exists(cfg.configPath())) return true;
        return cfg.get<json>(path, json()).is_null();
    };
    auto get = [&](const char* key) -> std::optional<int> {
        auto it = legacy.find(key); if (it == legacy.end()) return std::nullopt; return it->second;
    };
    auto mapBool = [&](const char* oldKey, const char* newPath) {
        if (auto value = get(oldKey); value) {
            if (maySet(newPath)) { cfg.set<bool>(newPath, *value != 0); ++settings; }
            else if (diagnostics && cfg.get<json>(newPath, json()) != json(*value != 0))
                legacyResourceDetail(*diagnostics, std::string("console/") + oldKey, "skipped", "保留当前运行设置；未授权覆盖");
        }
    };
    auto mapInt = [&](const char* oldKey, const char* newPath, int minValue, int maxValue) {
        if (auto value = get(oldKey); value) {
            const auto next = (std::clamp)(*value, minValue, maxValue);
            if (maySet(newPath)) { cfg.set<int>(newPath, next); ++settings; }
            else if (diagnostics && cfg.get<json>(newPath, json()) != json(next))
                legacyResourceDetail(*diagnostics, std::string("console/") + oldKey, "skipped", "保留当前运行设置；未授权覆盖");
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
    if (auto value = get("DisabledListenAt"); value) {
        if (maySet("dice/listen_at_when_off")) { cfg.set<bool>("dice/listen_at_when_off", *value == 0); ++settings; }
        else if (diagnostics && cfg.get<json>("dice/listen_at_when_off", json()) != json(*value == 0))
            legacyResourceDetail(*diagnostics, "console/DisabledListenAt", "skipped", "保留当前运行设置；未授权覆盖");
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
    if (auto allow = get("AllowStranger"); allow) {
        const std::string policy = *allow <= 0 ? "whitelist" : *allow == 1 ? "group_used" : "nonblacklist";
        if (maySet("events/friend_policy")) { cfg.set<std::string>("events/friend_policy", policy); ++settings; }
        else if (diagnostics && cfg.get<json>("events/friend_policy", json()) != json(policy))
            legacyResourceDetail(*diagnostics, "console/AllowStranger", "skipped", "保留当前好友策略；未授权覆盖");
    }
    if (imported || settings) {
        cfg.set<json>("dice/masters", arr);
        if (!cfg.save()) throw std::runtime_error("Cannot persist imported Console settings");
    }
    if (settingsImported) *settingsImported = settings;
    return imported;
}
// ── <root>/mod/ → data/mod/（Lua 模块目录）──────────────────────────
// 模块目录包含 descriptor.json、model.xml、rulebook 和 reply.lua 等资源。
// 或 DiceDir/mod/<名>.json。整棵复制到我们的 data/mod/，启动扫描即自动识别。
// Enhanced with ImportResult return + ImportOptions (skip/overwrite).
inline void legacyResourceDetail(ImportResult& result, std::string name,
                                 std::string status, std::string reason = {}) {
    if (status == "success") ++result.success;
    else if (status == "failed") ++result.failed;
    else ++result.skipped;
    result.details.push_back({std::move(name), std::move(status), std::move(reason)});
}

/// Copy one known module/plugin directory without following links. Unlike
/// filesystem::copy(recursive), this checks every descendant and destination.
inline bool legacyResourceTree(const fs::path& root, const fs::path& source,
                               const fs::path& destination, bool overwrite,
                               ImportResult& result) {
    using namespace migration_resources;
    std::error_code ec;
    if (!safePath(root, source)) {
        legacyResourceDetail(result, utf8(source.lexically_relative(root)), "failed", "unsafe source link/reparse point"); return false;
    }
    bool ok = true;
    auto visit = [&](const fs::path& file) {
        const auto target = fs::is_directory(source) ? destination / file.lexically_relative(source) : destination;
        if (fs::exists(target, ec) && !overwrite) return;
        const auto error = copyFile(file, target, overwrite);
        if (!error.empty()) { ok = false; legacyResourceDetail(result, utf8(file.lexically_relative(root)), "failed", error); }
    };
    if (fs::is_directory(source, ec)) {
        walk(root, source, visit, [&](const auto& path, const auto& error) {
            ok = false; legacyResourceDetail(result, utf8(path.lexically_relative(root)), "failed", error);
        });
        if (ok && safePath(fs::current_path(), destination)) {
            fs::create_directories(destination, ec);
            if (ec) { ok = false; legacyResourceDetail(result, utf8(destination), "failed", ec.message()); }
        }
    } else if (fs::is_regular_file(source, ec)) visit(source);
    else { ok = false; legacyResourceDetail(result, utf8(source.lexically_relative(root)), "failed", "not a regular resource file/directory"); }
    return ok;
}

inline ImportResult importMods(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult result;
    fs::path src = root / "mod";
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return result;
    fs::path dst = fs::path("data") / "mod";
    if (!migration_resources::safePath(root, src) || !migration_resources::safePath(fs::current_path(), dst)) {
        legacyResourceDetail(result, "mod", "failed", "unsafe source/destination resource path"); return result;
    }
    fs::create_directories(dst, ec);
    std::set<std::string> importedNames;
    json order = json::array();
    std::set<std::string> inactive;
    const auto modListPath = root / "conf" / "ModList.json";
    if (fs::exists(modListPath) && migration_resources::safePath(root, modListPath)) {
        auto list = json::parse(readFile(modListPath), nullptr, false);
        if (list.is_array()) for (const auto& item : list) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
            const auto name = item["name"].get<std::string>();
            if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..") continue;
            order.push_back(name);
            if (item.contains("active") && item["active"].is_boolean() && !item["active"].get<bool>()) {
                inactive.insert(name); inactive.insert(name + ".json");
            }
        } else legacyResourceDetail(result, "conf/ModList.json", "failed", "invalid module state list");
    }
    auto fromUtf8 = [](const std::string& value) { return fs::path(std::u8string(value.begin(), value.end())); };
    for (const auto& entry : fs::directory_iterator(src, ec)) {
        const fs::path& p = entry.path();
        const auto name8 = p.filename().u8string();
        std::string name(name8.begin(), name8.end());
        fs::path target = dst / fromUtf8(name + (inactive.count(name) ? ".disabled" : ""));
        ImportDetail detail;
        detail.name = name;

        // Check if target already exists
        bool exists = fs::exists(target, ec)
            || (!opts.overwrite && fs::exists(dst / fromUtf8(name), ec));
        if (exists && !opts.overwrite) {
            detail.status = "skipped";
            detail.reason = "already exists (overwrite=false)";
            ++result.skipped;
            result.details.push_back(detail);
            continue;
        }

        if (legacyResourceTree(root, p, target, opts.overwrite, result)) {
            detail.status = "success";
            ++result.success;
            importedNames.insert(name);
        } else {
            detail.status = "failed";
            detail.reason = "one or more module files were not migrated; see file details";
            // The individual errors above already contribute to failed.
        }
        result.details.push_back(detail);
    }

    if (!order.empty() && !importedNames.empty()) {
        const auto orderPath = dst.parent_path() / "mod_order.json";
        if (opts.overwrite || !fs::exists(orderPath)) {
            const auto error = migration_resources::writeFile(orderPath, order.dump(2));
            if (!error.empty()) legacyResourceDetail(result, "mod_order.json", "failed", error);
        }
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

inline int importLinks(ConfigManager& cfg, const fs::path& confDir, const ImportOptions& opts = {}) {
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
                && current.value("platform", std::string("onebot_v11")) == opts.targetPlatform
                && current.value("home", std::string()) == home
                && current.value("target", std::string()) == target) { duplicate = true; break; }
        if (duplicate) continue;
        links.push_back(json{{"platform", opts.targetPlatform}, {"home", home}, {"target", target},
                             {"mode", mode}, {"active", item.value("linking", true)}});
        ++imported;
    }
    if (imported) { cfg.set<json>("dice/links", links); if (!cfg.save()) throw std::runtime_error("无法保存迁入的链接/通知配置"); }
    return imported;
}

// ── conf/NoticeList.json → dice/notice/windows ────────────────
// Legacy notification windows carry the same QQ user/group target and the
// same bit-mask based severity subscription.  Newer fine-grained event lists
// stay empty so the imported window continues to use its original mask.
inline int importNotices(ConfigManager& cfg, const fs::path& confDir, const ImportOptions& opts = {}) {
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
                && current.value("platform", std::string("onebot_v11")) == opts.targetPlatform
                && current.value("chat_id", std::string()) == id
                && current.value("adapter_id", std::string()) == opts.targetAdapterId
                && current.value("is_group", true) == group) { duplicate = true; break; }
        if (duplicate) continue;
        int mask = item.value("type", 15);
        if (mask <= 0) continue;
        notice["windows"].push_back(json{{"platform", opts.targetPlatform}, {"adapter_id", opts.targetAdapterId}, {"chat_id", id},
            {"is_group", group}, {"level_mask", mask}, {"events", json::array()}});
        ++imported;
    }
    if (imported) { cfg.set<json>("dice/notice", notice); if (!cfg.save()) throw std::runtime_error("无法保存迁入的链接/通知配置"); }
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
inline std::string legacyDeckTargetName(const fs::path& root, const fs::path& path) {
    using namespace migration_resources;
    std::string filename = utf8(path.filename());
    if (lowerExtension(path) == ".json") filename = utf8(path.stem()) + ".json";
    else filename += ".json"; // Old Dice! also scanned valid JSON in .txt files.
    if (path.parent_path() == root / "PublicDeck" && deck_files::isSafeJsonFilename(filename)) return filename;
    std::string stem = utf8(path.stem());
    for (char& c : stem) if (static_cast<unsigned char>(c) < 32 || std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';
    stem = utf8Truncate(stem, 150);
    return "legacy-" + hash(sourceIdentity(root, path)) + "-" + stem + ".json";
}

inline std::string legacyMediaReference(const fs::path& root, const fs::path& deck,
                                        const std::string& reference, const ImportOptions& opts,
                                        std::string& problem) {
    using namespace migration_resources;
    if (reference.empty() || reference.find("{ ") != std::string::npos) return reference;
    if (reference.rfind("http://", 0) == 0 || reference.rfind("https://", 0) == 0
        || reference.rfind("base64://", 0) == 0 || reference.find('{') != std::string::npos) return reference;
    std::string local = reference;
    if (local.rfind("file://", 0) == 0) local.erase(0, 7);
    for (char& c : local) if (c == '\\') c = '/';
    const auto path = fromUtf8(local);
    fs::path found;
    for (const auto& candidate : {deck.parent_path() / path, root / path, root / "image" / path, root / "PublicDeck" / path}) {
        std::error_code ec;
        if (safePath(root, candidate) && fs::is_regular_file(candidate, ec) && !ec && mediaFile(candidate)) { found = candidate; break; }
    }
    if (found.empty()) { problem = "local media path is missing, unsupported or outside the selected legacy root"; return reference; }
    std::error_code ec;
    if (fs::file_size(found, ec) > 128ULL * 1024 * 1024 || ec) { problem = "local media file cannot be read or exceeds 128 MiB"; return reference; }
    const auto relative = fs::absolute(found).lexically_normal().lexically_relative(fs::absolute(root).lexically_normal());
    const auto destination = fs::path("data/legacy_assets") / hash(sourceIdentity(root, root)) / relative;
    if (!fs::exists(destination, ec) || opts.overwrite) {
        problem = copyFile(found, destination, opts.overwrite);
        if (!problem.empty()) return reference;
    } else if (!safePath(fs::current_path(), destination)) {
        problem = "unsafe existing media destination"; return reference;
    }
    return utf8(destination);
}

inline std::string rewriteLegacyDeckMedia(const fs::path& root, const fs::path& deck,
                                          const std::string& text, const ImportOptions& opts,
                                          std::set<std::string>& problems) {
    std::string out; std::size_t pos = 0;
    while (pos < text.size()) {
        const auto open = text.find('[', pos);
        if (open == std::string::npos) { out += text.substr(pos); break; }
        out += text.substr(pos, open - pos);
        const auto close = text.find(']', open);
        if (close == std::string::npos) { out += text.substr(open); break; }
        std::string token = text.substr(open, close - open + 1);
        std::size_t start = std::string::npos, end = token.size() - 1;
        if (token.rfind("[CQ:image,", 0) == 0 || token.rfind("[CQ:record,", 0) == 0
            || token.rfind("[CQ:video,", 0) == 0 || token.rfind("[img,", 0) == 0) {
            const auto file = token.find("file=");
            if (file != std::string::npos) {
                start = file + 5;
                const auto comma = token.find(',', start);
                if (comma != std::string::npos) end = comma;
            }
        } else if (token.rfind("[图片:", 0) == 0) start = std::string("[图片:").size();
        else if (token.rfind("[图:", 0) == 0) start = std::string("[图:").size();
        if (start != std::string::npos && end > start) {
            std::string reference = token.substr(start, end - start), problem;
            for (const auto& pair : {std::pair<std::string, std::string>{"&#44;", ","}, {"&#91;", "["}, {"&#93;", "]"}, {"&amp;", "&"}}) {
                std::size_t at = 0;
                while ((at = reference.find(pair.first, at)) != std::string::npos) { reference.replace(at, pair.first.size(), pair.second); at += pair.second.size(); }
            }
            auto rewritten = legacyMediaReference(root, deck, reference, opts, problem);
            if (!problem.empty()) problems.insert(problem);
            else if (rewritten != reference) {
                std::string escaped;
                for (char c : rewritten) {
                    if (c == '&') escaped += "&amp;"; else if (c == ',') escaped += "&#44;";
                    else if (c == '[') escaped += "&#91;"; else if (c == ']') escaped += "&#93;"; else escaped += c;
                }
                token.replace(start, end - start, escaped);
            }
        }
        out += token; pos = close + 1;
    }
    return out;
}

inline ImportResult importDecks(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult result;
    fs::path src = root / "PublicDeck";
    std::error_code ec;
    if (!fs::exists(src, ec) || !fs::is_directory(src, ec)) return result;
    fs::path dst = fs::path("data") / "decks";
    if (!migration_resources::safePath(fs::current_path(), dst)) {
        legacyResourceDetail(result, "PublicDeck", "failed", "unsafe destination deck path"); return result;
    }
    fs::create_directories(dst, ec);
    migration_resources::walk(root, src, [&](const fs::path& path) {
        std::string name = migration_resources::utf8(path.lexically_relative(src));
        ImportDetail detail;
        detail.name = name;
        if (migration_resources::mediaFile(path)) {
            legacyResourceDetail(result, name, "ignored", "媒体依赖由后续资源阶段校验并迁入"); return;
        }
        std::string content = readFile(path);
        std::string validationError = validateDeckJson(content);
        if (!validationError.empty()) {
            legacyResourceDetail(result, name, migration_resources::lowerExtension(path) == ".json" ? "failed" : "skipped",
                                 migration_resources::lowerExtension(path) == ".json" ? validationError : "not a legacy JSON deck"); return;
        }
        fs::path target = dst / migration_resources::fromUtf8(legacyDeckTargetName(root, path));
        bool exists = fs::exists(target, ec);
        if (exists && !opts.overwrite) {
            detail.status = "skipped";
            detail.reason = "already exists (overwrite=false)";
            ++result.skipped;
            result.details.push_back(detail);
            return;
        }
        auto deck = json::parse(content);
        std::set<std::string> mediaProblems;
        for (auto& [key, cards] : deck.items()) for (auto& card : cards) if (card.is_string())
            card = rewriteLegacyDeckMedia(root, path, card.get<std::string>(), opts, mediaProblems);
        const auto error = migration_resources::writeFile(target, deck.dump(2));
        if (error.empty()) {
            detail.status = "success";
            detail.reason = "destination: " + migration_resources::utf8(target);
            ++result.success;
        } else {
            detail.status = "failed";
            detail.reason = error;
            ++result.failed;
        }
        result.details.push_back(detail);
        for (const auto& problem : mediaProblems) {
            ++result.skipped;
            result.details.push_back({name, "preserved", problem + "; original reference retained, review before use"});
        }
    }, [&](const auto& path, const auto& error) {
        legacyResourceDetail(result, migration_resources::utf8(path.lexically_relative(src)), "failed", error);
    });
    return result;
}

// ── Legacy `plugin/` → data/plugin/ ───────────────────────────
// Old Dice! distinguishes directory mods and single-file Lua plugins.  The
// latter are loaded by Dice!Next's Lua compatibility manager from data/plugin.
inline ImportResult importPlugins(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult result;
    const auto source = root / "plugin", target = fs::path("data/plugin");
    if (!fs::exists(source)) return result;
    if (!migration_resources::safePath(root, source) || !fs::is_directory(source))
        throw std::runtime_error("旧插件目录不是安全目录");
    for (const auto& entry : fs::directory_iterator(source)) {
        const auto path = target / entry.path().filename();
        const auto name = migration_resources::utf8(entry.path().filename());
        if (fs::exists(path) && !opts.overwrite) { legacyResourceDetail(result, name, "skipped", "already exists (overwrite=false)"); continue; }
        if (legacyResourceTree(root, entry.path(), path, opts.overwrite, result)) legacyResourceDetail(result, name, "success");
    }
    return result;
}

struct LegacyLogContext {
    std::string groupId;
    std::vector<std::string> groupIds;
    std::string name;
    std::string gmId;
    std::string gameCode;
    std::string gameName;
    int status = 2;  // completed by default
    std::string createdAt;
    bool pendingSession = false;
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
    auto add = [&](const json& value) {
        json raw = value;
        if (raw.is_object()) {
            if (raw.contains("chid") || !raw.contains("gid")) return;
            raw = raw["gid"];
        }
        std::string id;
        if (raw.is_number_integer()) { const auto n = raw.get<long long>(); if (n > 0) id = std::to_string(n); }
        else if (raw.is_string()) id = raw.get<std::string>();
        if (!id.empty() && id != "0" && std::all_of(id.begin(), id.end(), [](unsigned char c){ return c >= '0' && c <= '9'; }))
            areas.push_back(id);
    };
    if (source.is_array()) for (const auto& value : source) add(value); else add(source);
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
                          std::map<std::string, LegacyLogContext>& logContexts,
                          const ImportOptions& opts = {}, ImportResult* diagnostics = nullptr) {
    if (!luaMod) return 0;
    const auto dir = root / "user/session";
    if (!fs::exists(dir)) return 0;
    const auto rawIndex = luaMod->confGet("game:index", "sessions");
    json index = rawIndex.empty() ? json::object() : json::parse(rawIndex);
    if (!index.is_object()) throw std::runtime_error("已有团务索引损坏；未替换");
    int imported = 0;
    migration_resources::walk(root, dir, [&](const fs::path& path) {
        if (migration_resources::lowerExtension(path) != ".json") return;
        const auto name = migration_resources::utf8(path.filename());
        const auto old = json::parse(readFile(path));
        if (!old.is_object()) throw std::runtime_error("旧团务不是对象");
        const auto areas = legacySessionAreas(old.contains("chats") ? old["chats"] : old.value("room", json{}));
        if (areas.empty()) {
            if (diagnostics) legacyResourceDetail(*diagnostics, name, "preserved", "私聊/频道/无可定位群的团务未自动启用");
            return;
        }
        const auto source = migration_resources::sourceIdentity(root, path), key = migration_resources::hash(source);
        std::string session = luaMod->confGet("game:legacy-index", key);
        const auto gameName = old.value("name", migration_resources::utf8(path.stem()));
        if (session.empty()) {
            json candidates = json::array();
            std::set<std::string> seen;
            auto sortedAreas = areas; std::sort(sortedAreas.begin(), sortedAreas.end());
            for (const auto& [code, value] : index.items()) {
                if (!value.is_string()) throw std::runtime_error("已有团务索引条目格式错误");
                const auto id = value.get<std::string>(), candidateScope = "game:session:" + id;
                if (id.rfind("legacy-", 0) != 0 || !seen.insert(id).second) continue;
                const auto priorName = luaMod->confGet(candidateScope, "__name");
                const auto original = luaMod->confGet(candidateScope, "__legacyOriginal");
                auto priorAreas = json::parse(luaMod->confGet(candidateScope, "__areas"), nullptr, false);
                if (!priorAreas.is_array()) continue;
                auto comparableAreas = priorAreas.get<std::vector<std::string>>();
                std::sort(comparableAreas.begin(), comparableAreas.end());
                if ((priorName == gameName && comparableAreas == sortedAreas) || original == old.dump())
                    candidates.push_back({{"id", id}, {"code", code}, {"name", priorName}});
            }
            const auto selected = opts.sessionMappings.find(key);
            if (selected != opts.sessionMappings.end() && selected->second != "__new__") {
                bool valid = false;
                for (const auto& candidate : candidates) if (candidate["id"] == selected->second) valid = true;
                if (!valid) throw std::runtime_error("所确认的旧团务衔接目标不在本次候选中；未修改现有团务");
                session = selected->second;
            } else if (!candidates.empty() && selected == opts.sessionMappings.end()) {
                if (diagnostics) {
                    diagnostics->confirmations.push_back({{"key", key}, {"file", name}, {"name", gameName}, {"candidates", candidates}});
                    legacyResourceDetail(*diagnostics, name, "preserved", "发现早期导入记录或相同团务副本；请确认衔接目标，未新建重复团务");
                } else throw std::runtime_error("旧团务需确认衔接目标；未新建重复团务");
                if (old.contains("log") && old["log"].is_object()) {
                    const auto file = old["log"].value("file", std::string());
                    if (!file.empty()) { LegacyLogContext pending; pending.pendingSession = true; logContexts[file] = pending; }
                }
                return;
            }
        }
        const bool existing = !session.empty();
        if (!existing) session = "legacy-" + key;
        const auto scope = "game:session:" + session;
        const auto previous = luaMod->confGet(scope, "__legacySource");
        const auto rawAliases = luaMod->confGet(scope, "__legacyAliases");
        json aliases = rawAliases.empty() ? json::array() : json::parse(rawAliases);
        if (!aliases.is_array()) throw std::runtime_error("团务来源别名损坏；未修改");
        bool knownAlias = false;
        for (const auto& alias : aliases) if (alias == source) knownAlias = true;
        if (!previous.empty() && previous != source && !knownAlias && !opts.sessionMappings.count(key))
            throw std::runtime_error("团务来源标识冲突");
        auto code = existing ? luaMod->confGet(scope, "__code") : nextLegacyGameCode(index);
        if (code.empty()) throw std::runtime_error("已有迁移团号缺失；未重新生成团务");
        std::vector<std::tuple<std::string, std::string, std::string>> values;
        auto set = [&](const std::string& field, const std::string& value) {
            const auto current = luaMod->confGet(scope, field);
            if (!existing || current.empty() || current == value || opts.overwriteData) values.emplace_back(scope, field, value);
            else if (diagnostics) legacyResourceDetail(*diagnostics, name + "/" + field, "skipped", "保留现有团务变量");
        };
        set("__name", gameName); set("__code", code); set("__areas", json(areas).dump());
        // The original owner/source stays intact when a confirmed copy is
        // attached. Each new path gets its own alias to the same stable id.
        if (previous.empty() || previous == source) set("__legacySource", source);
        else if (!knownAlias) { aliases.push_back(source); values.emplace_back(scope, "__legacyAliases", aliases.dump()); }
        set("__legacyOriginal", old.dump());
        values.emplace_back("game:legacy-index", key, session);
        if (old.contains("master")) set("__gms", legacyIdList(old["master"]).dump());
        if (old.contains("player")) set("__pls", legacyIdList(old["player"]).dump());
        for (const auto* field : {"observer", "decks", "tables"}) if (old.contains(field) && !old[field].empty()) {
            set(std::string("__legacy_") + field, old[field].dump());
            if (std::string_view(field) == "decks") {
                std::string error;
                if (session_deck::validCollection(old[field], error)) set("__decks", old[field].dump());
                else if (diagnostics) legacyResourceDetail(*diagnostics, name + "/decks", "preserved", "原牌堆存档已保留，未启用无效状态：" + error);
            } else if (diagnostics) legacyResourceDetail(*diagnostics, name + "/" + field, "preserved", "原状态已保留，未伪装成新版活动状态");
        }
        if (old.contains("observer")) set("legacyObservers", legacyIdList(old["observer"]).dump());
        if (old.contains("roulette")) {
            if (!old["roulette"].is_object()) throw std::runtime_error("旧轮盘格式错误");
            json roulette = json::object();
            for (const auto& [face, value] : old["roulette"].items()) {
                if (!value.is_object() || !value.contains("pool") || !value["pool"].is_array()) throw std::runtime_error("旧轮盘池格式错误");
                const auto pool = value["pool"];
                const auto rest = value.value("rest", static_cast<int>(pool.size()));
                if (rest < 0 || static_cast<size_t>(rest) > pool.size()) throw std::runtime_error("旧轮盘剩余数量无效");
                json left = json::array(); for (int i = 0; i < rest; ++i) left.push_back(pool[i]);
                roulette[face] = {{"copies", value.value("copy", 1)}, {"left", left}};
            }
            set("__rou", roulette.dump());
        }
        json data = old.value("conf", json::object());
        if (!data.is_object()) throw std::runtime_error("旧团务 conf 不是对象");
        if (old.contains("data")) {
            if (!old["data"].is_object()) throw std::runtime_error("旧团务 data 不是对象");
            for (const auto& [field, value] : old["data"].items()) data[field] = value;
        }
        for (const auto& [field, value] : data.items()) {
            if (field.empty() || field.rfind("__", 0) == 0) {
                if (diagnostics) legacyResourceDetail(*diagnostics, name + "/" + field, "preserved", "保留旧原稿；不覆盖新版内部状态");
            } else set(field, legacy_conf::encode(value));
        }
        for (const auto& group : areas) {
            const auto current = luaMod->confGet("game:" + group, "__session");
            if (current.empty() || current == session || opts.overwriteData) values.emplace_back("game:" + group, "__session", session);
            else if (diagnostics) legacyResourceDetail(*diagnostics, name + "/" + group, "skipped", "保留本群当前活动团务");
        }
        index[code] = session; values.emplace_back("game:index", "sessions", index.dump());
        if (!luaMod->confSetBatch(values)) throw std::runtime_error("无法保存迁移团务");
        LegacyLogContext context; context.groupId = areas.front(); context.groupIds = areas;
        context.name = gameName; context.gameName = gameName; context.gameCode = code;
        context.createdAt = legacyIsoFromEpoch(old.value("create_time", 0LL));
        if (old.contains("master")) { auto masters = legacyIdList(old["master"]); if (!masters.empty()) context.gmId = masters.front().get<std::string>(); }
        if (old.contains("log") && old["log"].is_object()) {
            const auto& log = old["log"];
            context.name = log.value("name", gameName);
            context.status = log.value("start", 0LL) > 0 ? (log.value("logging", false) ? 0 : 1) : 2;
            const auto file = log.value("file", std::string());
            if (!file.empty()) logContexts[file] = context;
        }
        ++imported;
    }, [&](const auto& path, const auto& reason) {
        if (diagnostics) legacyResourceDetail(*diagnostics, migration_resources::utf8(path.filename()), "failed", reason);
        else throw std::runtime_error(reason);
    });
    return imported;
}

// ── <root>/user/ChatConf.dat → group_settings  (群配置) ──
// Chat::readb（ManagerSystem.cpp）: 记录体 = [ll 内部ID(丢弃)] + tag 流 + short(-1)。
// 现代格式把配置都写进 tag 11 的 AnysTable（停用指令/rc房规/入群欢迎/许可使用…）；
// tag 0/4=群名，1/2/3=旧式 set/map，10=GBK表，21=ChConf(频道子配置，解析丢弃)。
struct LegacyChat {
    std::string name;
    std::map<std::string, legacy2::AttrVal> conf;
    json channels = json::object();
};
inline LegacyChat readChat(legacy2::BReader& r) {
    LegacyChat c; r.rLL();
    auto count = [&] { const auto n = r.rShort(); if (n < 0) throw std::runtime_error("旧群配置包含负数量"); return n; };
    for (int guard = 0; guard < 100000; ++guard) {
        const int16_t tag = r.rShort();
        if (tag == -1) return c;
        switch (tag) {
            case 0: c.name = legacy2::gbkToUtf8(r.rStr()); break;
            case 4: c.name = r.rStr(); break;
            case 1: { const auto n = count(); for (int i = 0; i < n; ++i) {
                auto key = legacy2::gbkToUtf8(r.rStr()); legacy2::AttrVal value;
                value.kind = legacy2::AttrVal::Kind::Bool; value.b = true; c.conf[key] = value;
            } } break;
            case 2: { const auto n = count(); for (int i = 0; i < n; ++i) {
                auto key = legacy2::gbkToUtf8(r.rStr()); legacy2::AttrVal value;
                value.kind = legacy2::AttrVal::Kind::Int; value.i = r.rInt(); c.conf[key] = value;
            } } break;
            case 3: { const auto n = count(); for (int i = 0; i < n; ++i) {
                auto key = legacy2::gbkToUtf8(r.rStr()); legacy2::AttrVal value;
                value.kind = legacy2::AttrVal::Kind::Str; value.s = legacy2::gbkToUtf8(r.rStr()); c.conf[key] = value;
            } } break;
            case 10: case 11: {
                for (auto& [key, value] : legacy2::readAnysTableImpl(r, tag == 10))
                    c.conf[key] = value;
            } break;
            case 20: case 21: { const auto n = count(); for (int i = 0; i < n; ++i) {
                const auto channel = std::to_string(r.rLL());
                json values = json::object();
                for (const auto& [key, value] : legacy2::readAnysTableImpl(r, tag == 20)) values[key] = value.toJson();
                c.channels[channel] = values;
            } } break;
            default: throw std::runtime_error("不支持的旧群配置标签: " + std::to_string(tag));
        }
    }
    throw std::runtime_error("旧群配置缺少结束标签");
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
                      const std::map<std::string, LegacyLogContext>& contexts, int& messages,
                      const ImportOptions& opts = {}, ImportResult* diagnostics = nullptr) {
    auto* st = db.getLogStorage(); if (!st) throw std::runtime_error("日志数据库不可用");
    const auto dir = root / "user/log"; if (!fs::exists(dir)) return 0;
    int imported = 0;
    auto rowsHash = [](const std::vector<GameLogMessageRow>& rows, size_t count) {
        json value = json::array();
        for (size_t i = 0; i < count; ++i) value.push_back({rows[i].sender, rows[i].userId, rows[i].createdAt, rows[i].content, rows[i].images});
        return migration_resources::hash(value.dump());
    };
    migration_resources::walk(root, dir, [&](const fs::path& path) {
        if (migration_resources::lowerExtension(path) != ".txt") return;
        const auto file = migration_resources::utf8(path.filename()), text = readFile(path);
        const auto source = migration_resources::sourceIdentity(root, path);
        LegacyLogContext context;
        if (const auto it = contexts.find(file); it != contexts.end()) context = it->second;
        if (context.pendingSession) {
            if (diagnostics) legacyResourceDetail(*diagnostics, file, "preserved", "关联团务需要确认衔接；未创建未关联或状态错误的日志");
            return;
        }
        if (context.groupId.empty()) {
            const auto stem = migration_resources::utf8(path.stem());
            auto prefix = stem.substr(0, stem.find('_')); if (!prefix.empty() && prefix[0] == 'g') prefix.erase(0, 1);
            const bool numeric = !prefix.empty() && prefix != "0" && std::all_of(prefix.begin(), prefix.end(), [](unsigned char c){ return c >= '0' && c <= '9'; });
            context.groupId = numeric ? prefix : "legacy:" + stem; context.name = stem;
        }
        std::vector<GameLogMessageRow> parsed;
        std::istringstream input(text); std::string line, sender, user, stamp, content;
        auto flush = [&] {
            if (sender.empty()) return;
            GameLogMessageRow row; row.sender = sender; row.userId = user; row.createdAt = legacyLogIso(stamp);
            row.content = content; row.images = "[]"; parsed.push_back(row);
            sender.clear(); user.clear(); stamp.clear(); content.clear();
        };
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string nextSender, nextUser, nextStamp;
            if (legacyLogHeader(line, nextSender, nextUser, nextStamp)) {
                flush(); sender = nextSender; user = nextUser; stamp = nextStamp;
            } else if (!sender.empty()) { if (!content.empty()) content += '\n'; content += line; }
        }
        flush();
        if (parsed.empty() && !text.empty()) throw std::runtime_error("旧日志没有可识别的消息头");
        std::optional<GameLogRow> existing;
        json meta;
        for (const auto& row : st->get_all<GameLogRow>()) {
            const auto value = json::parse(row.customRules, nullptr, false);
            if (!value.is_object()) continue;
            if (value.value("legacySourceKey", std::string()) == source) { existing = row; meta = value; break; }
            if (!value.contains("legacySourceKey") && value.value("legacySource", std::string()) == file) {
                if (diagnostics) legacyResourceDetail(*diagnostics, file, "preserved", "已有同名旧导入日志缺少来源标识，需人工确认，未重复新增");
                return;
            }
        }
        size_t from = 0;
        if (existing) {
            const auto previousBytes = meta.value("legacySourceBytes", size_t(0));
            from = meta.value("legacyMessageCount", size_t(0));
            if (text.size() == previousBytes && migration_resources::hash(text) == meta.value("legacyContentHash", std::string())) return;
            bool append = text.size() >= previousBytes && parsed.size() >= from
                && migration_resources::hash(text.substr(0, previousBytes)) == meta.value("legacyContentHash", std::string())
                && rowsHash(parsed, from) == meta.value("legacyImportedRowsHash", std::string());
            if (append) {
                const auto current = st->get_all<GameLogMessageRow>(orm::where(orm::c(&GameLogMessageRow::logId) == existing->id),
                    orm::order_by(&GameLogMessageRow::id).asc(), orm::limit(static_cast<int>(from)));
                append = current.size() == from && rowsHash(current, from) == meta.value("legacyImportedRowsHash", std::string());
            }
            if (!append) {
                if (diagnostics) legacyResourceDetail(*diagnostics, file, "preserved", "源日志不是整条追加或已有消息被编辑；保留新版，未覆盖");
                return;
            }
        }
        if (!meta.is_object()) meta = json::object();
        meta["legacySource"] = file; meta["legacySourceKey"] = source;
        meta["legacySourceBytes"] = text.size(); meta["legacyContentHash"] = migration_resources::hash(text);
        meta["legacyMessageCount"] = parsed.size(); meta["legacyImportedRowsHash"] = rowsHash(parsed, parsed.size());
        meta["legacyGroups"] = context.groupIds;
        if (!context.gameCode.empty()) meta["gameCode"] = context.gameCode;
        if (!context.gameName.empty()) meta["gameName"] = context.gameName;
        int logId = existing ? existing->id : 0;
        st->transaction([&] {
            GameLogRow log;
            if (existing) log = *existing;
            else {
                log.groupId = context.groupId; log.gmId = context.gmId; log.name = context.name;
                log.players = "[]"; log.status = context.status; log.createdAt = context.createdAt.empty() ? utils::nowIso8601() : context.createdAt;
            }
            log.customRules = meta.dump();
            if (existing) st->update(log); else logId = st->insert(log);
            for (size_t i = from; i < parsed.size(); ++i) { auto row = parsed[i]; row.logId = logId; st->insert(row); }
            return true;
        });
        messages += static_cast<int>(parsed.size() - from); ++imported;
        if (!existing && context.status == 0) {
            auto groups = context.groupIds; if (groups.empty()) groups.push_back(context.groupId);
            for (const auto& group : groups) {
                if (upsertImportedGroupSetting(db, opts, group, "activeLog", std::to_string(logId)))
                    upsertImportedGroupSetting(db, opts, group, "activeLogName", context.name);
                else if (diagnostics) legacyResourceDetail(*diagnostics, file + "/" + group, "skipped", "保留已有活动日志");
            }
        }
    }, [&](const auto& path, const auto& reason) {
        if (diagnostics) legacyResourceDetail(*diagnostics, migration_resources::utf8(path.filename()), "failed", reason);
        else throw std::runtime_error(reason);
    });
    return imported;
}

inline int importChatConf(Database& db, const fs::path& userDir, int& groups,
                         LuaPluginManager* luaMod = nullptr, const ImportOptions& opts = {},
                         ImportResult* diagnostics = nullptr) {
    std::vector<std::pair<long long, LegacyChat>> records;
    const auto file = legacyDataFile(userDir, {"ChatConf.dat", "ChatConf.dat.bak", "ChatConf.bak", "ChatConf.RDconf"},
        [&](const fs::path& candidate) {
            records = legacy2::readRecords<LegacyChat>(candidate, readChat);
        }, diagnostics);
    if (file.empty()) return 0;
    auto* st = db.getStorage(); if (!st) throw std::runtime_error("群设置数据库不可用");
    int settings = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> values;
    auto detail = [&](const std::string& name, const std::string& reason) {
        if (diagnostics) { ++diagnostics->skipped; diagnostics->details.push_back({name, "preserved", reason}); }
    };
    st->transaction([&] {
        for (const auto& [id, chat] : records) {
            for (const auto& [key, value] : chat.conf)
                reportUnsupportedValue(value, "groupConf:" + std::to_string(id) + "/" + key, diagnostics);
            const auto group = std::to_string(id), scope = "g:" + group;
            auto write = [&](const std::string& key, const std::string& value) {
                if (upsertImportedGroupSetting(db, opts, group, key, value)) ++settings;
                else detail(group + "/" + key, "保留现有群设置；未授权覆盖数据");
            };
            auto on = [&](const std::string& key) {
                const auto it = chat.conf.find(key);
                if (it == chat.conf.end()) return false;
                return it->second.kind == legacy2::AttrVal::Kind::Str
                    ? (it->second.s == "true" || it->second.s == "1") : it->second.asInt() > 0;
            };
            for (const auto& [key, value] : chat.conf) {
                if (value.kind == legacy2::AttrVal::Kind::Other) { detail(group + "/" + key, "旧函数变量不自动执行"); continue; }
                if (luaMod) {
                    const auto converted = legacyScalar(value), current = luaMod->confGet(scope, key);
                    if (current.empty() || current == converted || opts.overwriteData) values.emplace_back(scope, key, converted);
                    else detail(group + "/" + key, "保留现有插件群变量");
                }
            }
            if (!chat.name.empty()) write("legacyGroupName", chat.name);
            if (chat.conf.count("停用指令")) write("enabled", on("停用指令") ? "0" : "1");
            if (chat.conf.count("忽略")) write("locked", on("忽略") ? "1" : "0");
            if (chat.conf.count("禁用回复")) write("replyDisabled", on("禁用回复") ? "1" : "0");
            if (const auto it = chat.conf.find("rc房规"); it != chat.conf.end()) {
                const auto rule = it->second.asInt();
                if (rule >= 0 && rule <= 7) write("cocRule", std::to_string(rule));
                else detail(group + "/rc房规", "房规编号超出 0–7；原值保留，未启用");
            }
            if (const auto it = chat.conf.find("入群欢迎"); it != chat.conf.end() && it->second.isStr())
                write("welcome", it->second.s);
            std::vector<std::string> disabled;
            for (const auto* command : {"jrrp", "draw", "me", "help", "ob"})
                if (on(std::string("禁用") + command)) disabled.push_back(command);
            if (on("禁用draw")) disabled.push_back("drawh");
            if (on("禁用jrrp")) { disabled.push_back("mrrp"); disabled.push_back("zrrp"); }
            const bool hasSwitch = chat.conf.count("禁用jrrp") || chat.conf.count("禁用draw")
                || chat.conf.count("禁用me") || chat.conf.count("禁用help") || chat.conf.count("禁用ob");
            if (hasSwitch) {
                std::string list; for (const auto& command : disabled) { if (!list.empty()) list += ','; list += command; }
                write("disabledCmds", list);
            }
            for (const auto* flag : {"免清", "免黑", "许可使用", "未审核", "协议无效"})
                if (on(flag)) detail(group + "/" + flag, "旧许可/清理策略没有等价的安全映射；已保留兼容变量，需在新版确认");
            if (!chat.channels.empty()) {
                if (luaMod) values.emplace_back(scope, "__legacyChannels", legacy_conf::encode(chat.channels));
                detail(group + "/channels", "频道子配置已保留，未自动对应新版频道身份");
            }
        }
        return true;
    });
    if (luaMod && !values.empty() && !luaMod->confSetBatch(values)) throw std::runtime_error("无法保存兼容群变量");
    groups += static_cast<int>(records.size());
    return settings;
}

// ── Orchestrator ─────────────────────────────────────────────
inline json importAncillaryResources(const fs::path& root, const ImportOptions& opts = {}) {
    ImportResult files;
    json preserved = json::array(), unsupported = json::array();
    const auto sourceKey = migration_resources::hash(migration_resources::sourceIdentity(root, root));
    for (const auto* name : {"selfdata", "image", "PublicDeck", "CardTemp", "conf/censor"}) {
        const auto directory = root / name;
        if (!fs::exists(directory)) continue;
        migration_resources::walk(root, directory, [&](const fs::path& path) {
            const auto relative = path.lexically_relative(directory);
            const auto label = std::string(name) + "/" + migration_resources::utf8(relative);
            const bool self = std::string(name) == "selfdata";
            const bool activeSelf = self && migration_resources::lowerExtension(path) == ".json" && relative.parent_path().empty();
            const bool deckMedia = std::string(name) == "PublicDeck" && migration_resources::mediaFile(path);
            if (std::string(name) == "PublicDeck" && !deckMedia) return;
            const bool activeMedia = std::string(name) == "image" && migration_resources::mediaFile(path);
            fs::path target;
            bool overwrite = opts.overwrite;
            if (activeSelf) {
                const auto value = json::parse(readFile(path));
                if (!value.is_object() && !value.is_array()) throw std::runtime_error("旧 selfdata 必须是 JSON 对象或数组");
                target = fs::path("data/self_data") / relative; target.replace_extension(".json");
                overwrite = opts.overwriteData;
            } else if (activeMedia) target = fs::path("image") / relative;
            else if (deckMedia) target = fs::path("data/legacy_assets") / sourceKey / "PublicDeck" / relative;
            else {
                target = fs::path("data/legacy-import-preserved") / sourceKey / name / relative;
                preserved.push_back({{"name", label}, {"reason", "原文件已保留，尚无等价运行时格式"},
                    {"originalPath", migration_resources::utf8(target)}});
            }
            if (fs::exists(target) && !overwrite) {
                if (readFile(target) == readFile(path)) legacyResourceDetail(files, label, "success", "相同文件已存在");
                else legacyResourceDetail(files, label, "skipped", "保留已有资源；未授权覆盖");
                return;
            }
            const auto error = migration_resources::copyFile(path, target, overwrite);
            legacyResourceDetail(files, label, error.empty() ? "success" : "failed", error);
        }, [&](const auto& path, const auto& error) {
            legacyResourceDetail(files, migration_resources::utf8(path.lexically_relative(root)), "failed", error);
        });
    }
    return {{"files", files.toJSON()}, {"preservedOnly", preserved}, {"unsupported", unsupported}};
}
// Enhanced runImport accepts CardDeck* and LuaPluginManager* for
// post-import reload, and ImportOptions for skip/overwrite control.
// The deck/mod results are structured ImportResult objects.
inline json runImport(Database& db, ConfigManager& cfg, I18n& i18n, ReplyManager& reply,
                      const std::string& diceDataDir,
                      CardDeck* deck = nullptr, LuaPluginManager* luaMod = nullptr,
                      const ImportOptions& opts = {}, const std::vector<std::string>& availableHelp = {}) {
    std::unique_lock<std::mutex> lock(migration_report::importMutex(), std::try_to_lock);
    if (!lock.owns_lock()) return {{"ok", false}, {"complete", false}, {"error", "已有迁移正在执行"}};
    const auto root = migration_resources::fromUtf8(diceDataDir), userDir = root / "user", confDir = root / "conf";
    if (const auto reason = migration_report::validateRoot(root); !reason.empty())
        return {{"ok", false}, {"complete", false}, {"error", reason}};
    if (opts.targetPlatform != "onebot_v11" && opts.targetPlatform != "milky")
        return {{"ok", false}, {"complete", false}, {"error", "旧 QQ 数字身份只能直接迁入 OneBot 或 Milky；其他平台需要先确认身份映射"}};
    const auto started = std::chrono::steady_clock::now();
    json errors = json::array(), stages = json::array(), safety = json::array(), details = json::array();
    ImportResult cardsDetails, usersDetails, helpDetails, groupDetails, sessionDetails, logDetails, consoleDetails;
    auto timed = [&](const char* name, auto&& action) {
        using Result = decltype(action());
        const auto begin = std::chrono::steady_clock::now();
        DICE_LOG_INFO("正在迁移旧数据：{}", name);
        try {
            auto result = action();
            stages.push_back({{"name", name}, {"status", "success"}, {"elapsedMs",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count()}});
            return result;
        } catch (const std::exception& error) {
            errors.push_back({{"stage", name}, {"reason", error.what()}});
            stages.push_back({{"name", name}, {"status", "failed"}, {"reason", error.what()}});
            DICE_LOG_ERROR("旧数据迁移阶段失败：{}；详情已加入迁移报告", name);
            return Result{};
        }
    };
    int users = 0, orphans = 0, consoleSettings = 0, groups = 0, logMessages = 0;
    int cards = timed("人物卡", [&] { return importCards(db, userDir, users, opts, &cardsDetails); });
    int profiles = timed("用户数据", [&] { return importUsers(db, userDir, luaMod, opts, &usersDetails); });
    int blacklist = timed("黑名单", [&] { return importBlacklist(db, confDir, &safety); });
    int help = timed("自定义帮助", [&] {
        migration_report::validateJsonSource(confDir / "CustomHelp.json", false);
        return importHelp(db, i18n, confDir, opts, &helpDetails);
    });
    json customTexts = json::array();
    int msgs = timed("自定义文案", [&] {
        migration_report::validateJsonSource(confDir / "CustomMsg.json", false);
        return importCustomMsg(db, i18n, confDir, orphans, &customTexts, opts, &cfg);
    });
    int masters = timed("控制台配置", [&] { return importMasters(cfg, confDir, &consoleSettings, opts, &consoleDetails); });
    int links = timed("跨群链接", [&] {
        migration_report::validateJsonSource(confDir / "LinkList.json", true);
        return importLinks(cfg, confDir, opts);
    });
    int notices = timed("通知窗口", [&] {
        migration_report::validateJsonSource(confDir / "NoticeList.json", true);
        return importNotices(cfg, confDir, opts);
    });
    int censor = timed("敏感词", [&] {
        migration_report::validateJsonSource(confDir / "CustomCensor.json", false);
        return importCensorWords(cfg, confDir);
    });
    auto decks = timed("牌堆", [&] { return importDecks(root, opts); });
    if (decks.success > 0 && deck) timed("重新读取牌堆", [&] { deck->reload({"decks", "data/decks"}); return 0; });
    legacy_reply_references::Catalog references;
    auto addHelp = [&](const std::string& name) {
        const auto normalized = legacy_reply_references::lower(legacy_reply_references::trim(name));
        if (!normalized.empty()) { references.help.insert(normalized); references.helpTargets[normalized] = name; }
    };
    for (const auto& name : availableHelp) addHelp(name);
    if (deck) for (const auto& name : deck->deckNames()) references.decks.insert(legacy_reply_references::lower(name));
    const auto deckDir = fs::path("data/decks");
    if (fs::exists(deckDir)) for (const auto& entry : fs::directory_iterator(deckDir)) {
        if (!entry.is_regular_file() || migration_resources::lowerExtension(entry.path()) != ".json") continue;
        const auto values = json::parse(readFile(entry.path()), nullptr, false);
        if (values.is_object()) for (const auto& [name, value] : values.items())
            if (value.is_array()) references.decks.insert(legacy_reply_references::lower(name));
    }
    for (const auto& [key, value] : i18n.flatten(Locale::kZhHans))
        if (key.rfind("help.topic.", 0) == 0 && !value.empty()) addHelp(key.substr(11));
    if (auto* st = db.getStorage()) for (const auto& row : st->get_all<I18nOverrideRow>()) {
        if (row.locale == "zh-Hans" && row.key.rfind("help.topic.", 0) == 0 && !row.value.empty()) addHelp(row.key.substr(11));
        if (row.locale == "zh-Hans" && row.key.rfind("legacy.", 0) == 0) references.texts.insert(row.key.substr(7));
    }
    for (const auto& [original, mapped] : msgKeyMap()) if (i18n.hasOverride(Locale::kZhHans, mapped)) references.texts.insert(original);
    legacy_reply_references::Report referenceReport;
    int replies = timed("高级回复", [&] { return importReplies(reply, confDir, references, &referenceReport, &safety); });
    auto mods = timed("Lua 模组", [&] { return importMods(root, opts); });
    auto plugins = timed("Lua 插件", [&] { return importPlugins(root, opts); });
    auto assets = timed("插件存档与资源", [&] { return importAncillaryResources(root, opts); });
    for (const auto* legacyOnly : {"conf/BlackListEx.json", "com.w4123.dice/BlackMarks.json", "com.w4123.dice/BlackHistory.json"}) {
        if (!fs::exists(root / legacyOnly)) continue;
        if (!migration_resources::safePath(root, root / legacyOnly)) {
            errors.push_back({{"stage", "旧历史数据"}, {"reason", "旧历史数据文件是不安全的链接或越界路径"}});
        } else safety.push_back({{"source", legacyOnly}, {"name", legacyOnly}, {"status", "preserved"}, {"requiresReview", true},
            {"reason", "旧历史/外部黑名单尚未自动转为本地处罚；原文件保留，请确认迁移策略"}});
    }
    int settings = timed("群设置", [&] { return importChatConf(db, userDir, groups, luaMod, opts, &groupDetails); });
    std::map<std::string, LegacyLogContext> contexts;
    int sessions = timed("团务", [&] { return importSessions(root, luaMod, contexts, opts, &sessionDetails); });
    int logs = timed("跑团日志", [&] { return importLogs(db, root, contexts, logMessages, opts, &logDetails); });
    bool review = orphans > 0 || referenceReport.ambiguous > 0 || referenceReport.unresolved > 0;
    auto addDetails = [&](const char* stage, const ImportResult& result) {
        for (const auto& item : result.details) {
            details.push_back({{"stage", stage}, {"name", item.name}, {"status", item.status}, {"reason", item.reason}});
            if (item.status == "failed") errors.push_back({{"stage", stage}, {"reason", item.reason}});
            if (item.status == "skipped" || item.status == "preserved") review = true;
        }
    };
    addDetails("人物卡", cardsDetails); addDetails("用户数据", usersDetails); addDetails("帮助", helpDetails);
    addDetails("控制台配置", consoleDetails);
    addDetails("群设置", groupDetails); addDetails("团务", sessionDetails); addDetails("日志", logDetails);
    addDetails("牌堆", decks); addDetails("模组", mods); addDetails("插件", plugins);
    if (assets.is_object()) {
        for (const auto& item : assets.value("files", json::object()).value("details", json::array())) {
            auto detail = item; detail["stage"] = "资源"; details.push_back(detail);
            if (item.value("status", "") == "failed") errors.push_back({{"stage", "资源"}, {"reason", item.value("reason", "")}});
            if (item.value("status", "") == "skipped") review = true;
        }
        if (!assets.value("preservedOnly", json::array()).empty() || !assets.value("unsupported", json::array()).empty()) review = true;
    }
    for (const auto& item : safety) {
        details.push_back(item);
        if (item.value("status", "") == "failed") errors.push_back({{"stage", "名单/回复"}, {"reason", item.value("reason", "")}});
        if (item.value("requiresReview", false)) review = true;
    }
    for (const auto& item : customTexts) if (item.value("status", "") != "active") review = true;
    const bool ok = errors.empty();
    json report = {{"ok", ok}, {"complete", ok && !review}, {"partial", !ok || review},
        {"cards", cards}, {"cardUsers", users}, {"profiles", profiles}, {"blacklist", blacklist},
        {"help", help}, {"msgs", msgs}, {"orphans", orphans}, {"masters", masters}, {"consoleSettings", consoleSettings},
        {"links", links}, {"notices", notices}, {"censorWords", censor}, {"replies", replies},
        {"decks", decks.toJSON()}, {"mods", mods.toJSON()}, {"plugins", plugins.toJSON()}, {"assets", assets},
        {"chatGroups", groups}, {"chatSettings", settings}, {"sessions", sessions}, {"logs", logs}, {"logMessages", logMessages},
        {"sessionConfirmations", sessionDetails.confirmations},
        {"replyReferences", referenceReport.toJSON()}, {"customTextDetails", customTexts},
        {"diagnostics", details}, {"errors", errors}, {"stages", stages},
        {"targetPlatform", opts.targetPlatform}, {"targetAdapterId", opts.targetAdapterId}, {"backupPath", opts.beforeImportBackup},
        {"elapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()},
        {"reloadRequired", mods.success > 0 || plugins.success > 0}};
    try { migration_report::save(report); }
    catch (const std::exception& error) {
        report["ok"] = false; report["complete"] = false; report["partial"] = true;
        report["errors"].push_back({{"stage", "保存报告"}, {"reason", error.what()}});
    }
    DICE_LOG_INFO("旧数据迁移结束：人物卡 {}，回复 {}，团务 {}，日志 {}；错误 {}，{}",
        cards, replies, sessions, logs, report["errors"].size(), report.value("complete", false) ? "完整完成" : "请查看报告中的待确认项");
    return report;
}

}  // namespace dice::legacyv2
