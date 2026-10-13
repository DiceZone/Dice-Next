#include "card_store.h"
#include "../../storage/database.h"
#include "../../common/logger.h"
#include "../rules_lock.h"

#include <sqlite_orm/sqlite_orm.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <limits>

namespace dice {

namespace orm = sqlite_orm;
using json = nlohmann::json;

CharacterCardStore::CharacterCardStore(Database& db) : db_(db) {}

// ─── Synonyms ────────────────────────────────────────────────

// Global synonym registry: alias (lowercased ASCII) → canonical name. Seeded
// with the built-in COC7 defaults so behavior is unchanged even with no rule
// packs present; rule packs (rules/*.json "alias") merge in additively at
// startup via loadRulePackAliases (P1 of the rule-pack system: GLOBAL merge —
// any alias works regardless of active system; per-rule scoping is later).
// Runtime reloads write under rulesLock() exclusively; canonical() reads under
// a shared lock. Callers already holding either lock use canonicalUnlocked().
static std::map<std::string, std::string> builtinAliases() {
    return {
        // ── 八大属性 + 英文缩写 ──
        {"str", "力量"}, {"con", "体质"}, {"siz", "体型"}, {"dex", "敏捷"},
        {"app", "外貌"}, {"int", "智力"}, {"pow", "意志"}, {"edu", "教育"},
        // DND5e ability-score aliases (the first four reuse the COC canonicals).
        {"strength", "力量"}, {"dexterity", "敏捷"}, {"constitution", "体质"},
        {"intelligence", "智力"}, {"wis", "感知"}, {"wisdom", "感知"},
        {"cha", "魅力"}, {"charisma", "魅力"},
        // ── 衍生检定 = 对应属性 ──
        {"灵感", "智力"}, {"idea", "智力"},
        {"知识", "教育"}, {"know", "教育"},
        // ── 理智 ──
        {"san", "理智"}, {"san值", "理智"}, {"理智值", "理智"}, {"sc", "理智"},
        // ── 幸运 ──
        {"luck", "幸运"}, {"luc", "幸运"}, {"运气", "幸运"},
        // ── 生命/魔法 ──
        {"hp", "生命值"}, {"生命", "生命值"}, {"体力", "生命值"},
        {"mp", "魔法值"}, {"魔法", "魔法值"}, {"mag", "魔法值"},
        // ── 临时生命（护盾，）：可临时超上限，受击优先扣除 ──
        {"hptemp", "临时生命值"}, {"temphp", "临时生命值"}, {"临时生命", "临时生命值"},
        {"临时血量", "临时生命值"}, {"临时hp", "临时生命值"}, {"护盾", "临时生命值"},
        // ── DND 钱币：1pp=10gp=100sp=1000cp，1ep=5sp；自动借位换算 ──
        {"pp", "铂金币"}, {"铂金", "铂金币"}, {"白金币", "铂金币"}, {"platinum", "铂金币"},
        {"gp", "金币"}, {"金", "金币"}, {"gold", "金币"},
        {"sp", "银币"}, {"银", "银币"}, {"silver", "银币"},
        {"cp", "铜币"}, {"铜", "铜币"}, {"copper", "铜币"},
        {"ep", "银金币"}, {"银金", "银金币"}, {"electrum", "银金币"},
        // ── 信用评级 ──
        {"信用", "信用评级"}, {"信誉", "信用评级"}, {"信用评分", "信用评级"}, {"cr", "信用评级"},
        // ── 伤害加值（公式属性：存投掷公式不摇）——db/DB/DamageBonus 等都归并 ──
        {"db", "伤害加值"}, {"damagebonus", "伤害加值"}, {"dmg", "伤害加值"},
        {"伤害", "伤害加值"}, {"伤害加成", "伤害加值"},
        // ── 技能别名（同一技能的不同写法）──
        {"侦察", "侦查"},
        {"计算机", "计算机使用"}, {"电脑", "计算机使用"},
        {"图书馆", "图书馆使用"},
        {"汽车", "驾驶"}, {"汽车驾驶", "驾驶"},
        {"领航", "导航"},
        {"自然学", "博物学"},
        {"克苏鲁", "克苏鲁神话"}, {"cm", "克苏鲁神话"}, {"神话", "克苏鲁神话"},
        {"开锁", "锁匠"}, {"撬锁", "锁匠"},
        {"重型", "操作重型机械"}, {"重型操作", "操作重型机械"}, {"重型机械", "操作重型机械"},
        {"魅惑", "取悦"},
        {"巧手", "妙手"},
        {"快速交谈", "话术"},
    };
}
static std::map<std::string, std::string>& aliasRegistry() {
    static std::map<std::string, std::string> reg = builtinAliases();
    return reg;
}
void CharacterCardStore::resetAliases() { aliasRegistry() = builtinAliases(); }

std::string CharacterCardStore::canonical(const std::string& name) {
    std::shared_lock<std::shared_mutex> lk(rulesLock());
    return canonicalUnlocked(name);
}

std::string CharacterCardStore::canonicalUnlocked(const std::string& name) {
    // Lowercase ASCII letters for the synonym lookup (Chinese unaffected).
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto& reg = aliasRegistry();
    auto it = reg.find(key);
    return it != reg.end() ? it->second : name;
}

void CharacterCardStore::registerAlias(const std::string& alias, const std::string& canonName) {
    if (alias.empty() || canonName.empty()) return;
    std::string key = alias;
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    aliasRegistry()[key] = canonName;
}

int CharacterCardStore::loadRulePackAliases(const std::string& dir) {
    int n = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !e.is_regular_file() || e.path().extension() != ".json") continue;
        try {
            std::ifstream f(e.path(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            json j = json::parse(body);
            if (!j.contains("alias") || !j["alias"].is_object()) continue;   // 非规则包/无 alias 区块
            for (auto& [canon, arr] : j["alias"].items()) {
                if (!arr.is_array()) continue;
                for (auto& a : arr)
                    if (a.is_string()) { registerAlias(a.get<std::string>(), canon); ++n; }
            }
        } catch (...) {}   // 坏文件跳过，不影响其它规则包
    }
    return n;
}

// ─── Internal helpers ────────────────────────────────────────

namespace {
bool sameAttribute(const std::string& key, const std::string& name) {
    return key == name || CharacterCardStore::canonical(key) == CharacterCardStore::canonical(name);
}
json::const_iterator findAttribute(const json& values, const std::string& name) {
    if (!values.is_object()) return values.end();
    auto it = values.find(name);
    if (it != values.end()) return it;
    it = values.find(CharacterCardStore::canonical(name));
    if (it != values.end()) return it;
    for (it = values.begin(); it != values.end(); ++it)
        if (sameAttribute(it.key(), name)) return it;
    return values.end();
}
bool eraseAttribute(json& values, const std::string& name) {
    if (!values.is_object()) return false;
    bool erased = false;
    for (auto it = values.begin(); it != values.end();) {
        if (sameAttribute(it.key(), name)) { it = values.erase(it); erased = true; }
        else ++it;
    }
    return erased;
}
std::string nowIso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}
}  // namespace

// All attributes of the card (user, name). Legacy rows may share name="" across
// groups, so we take the most-recently-updated match as the canonical one.
std::map<std::string, int> CharacterCardStore::attrsOf(
        const std::string& user, const std::string& name) const {
    std::map<std::string, int> result;
    auto* st = db_.getCardStorage();
    if (!st) return result;
    try {
        auto rows = st->get_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == name),
            orm::order_by(&CharacterCardRow::updatedAt).desc());
        if (!rows.empty() && !rows.front().attrs.empty()) {
            json j = json::parse(rows.front().attrs);
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_number_integer() && it.value() >= (std::numeric_limits<int>::min)()
                    && it.value() <= (std::numeric_limits<int>::max)())
                    result[it.key()] = it.value().get<int>();
        }
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CardStore: attrsOf failed: {}", e.what());
    }
    return result;
}

void CharacterCardStore::saveAttrsOf(const std::string& user, const std::string& name,
                                     const std::map<std::string, int>& attrs) {
    auto* st = db_.getCardStorage();
    if (!st) return;
    json j = json::object();
    for (const auto& [k, v] : attrs) j[k] = v;
    try {
        auto rows = st->get_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == name),
            orm::order_by(&CharacterCardRow::updatedAt).desc());
        // 保留旧 JSON 里的非整数键（"__meta" 锁/文本等），只重建整数属性。
        if (!rows.empty() && !rows.front().attrs.empty()) {
            try {
                json old = json::parse(rows.front().attrs);
                if (old.is_object())
                    for (auto it = old.begin(); it != old.end(); ++it)
                        if (!it.value().is_number_integer() && !attrs.count(it.key())) j[it.key()] = it.value();
            } catch (...) {}
        }
        if (rows.empty()) {
            CharacterCardRow row;
            row.userId = user;
            row.groupId = "";
            row.name = name;
            row.attrs = j.dump();
            row.updatedAt = nowIso();
            st->insert(row);
        } else {
            CharacterCardRow row = rows.front();
            row.attrs = j.dump();
            row.updatedAt = nowIso();
            st->update(row);
        }
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CardStore: saveAttrsOf failed: {}", e.what());
    }
}

// ─── 卡片元数据（"__meta"：锁 / 文本属性）────────────────────────
// 直接读写绑定卡整份 attrs JSON 的 "__meta" 对象（原版 CardTemp 锁定/文本移植）。

nlohmann::json CharacterCardStore::rawCardJson(const std::string& user, const std::string& name) const {
    auto* st = db_.getCardStorage();
    if (!st) return json::object();
    try {
        auto rows = st->get_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == name),
            orm::order_by(&CharacterCardRow::updatedAt).desc());
        if (!rows.empty() && !rows.front().attrs.empty()) {
            json j = json::parse(rows.front().attrs, nullptr, false);
            if (j.is_object()) return j;
        }
    } catch (...) {}
    return json::object();
}
void CharacterCardStore::saveRawCardJson(const std::string& user, const std::string& name,
                                         const nlohmann::json& j) {
    auto* st = db_.getCardStorage();
    if (!st) return;
    try {
        auto rows = st->get_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == name),
            orm::order_by(&CharacterCardRow::updatedAt).desc());
        if (rows.empty()) {
            CharacterCardRow row;
            row.userId = user; row.groupId = ""; row.name = name;
            row.attrs = j.dump(); row.updatedAt = nowIso();
            st->insert(row);
        } else {
            CharacterCardRow row = rows.front();
            row.attrs = j.dump(); row.updatedAt = nowIso();
            st->update(row);
        }
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CardStore: saveRawCardJson failed: {}", e.what());
    }
}

bool CharacterCardStore::cardLocked(const std::string& user, const std::string& scope,
                                    const std::string& key) const {
    return cardLockedByName(user, boundCard(user, scope), key);
}
bool CharacterCardStore::cardLockedByName(const std::string& user, const std::string& cardName,
                                          const std::string& key) const {
    json j = rawCardJson(user, cardName);
    if (!j.contains("__meta") || !j["__meta"].is_object()) return false;
    std::string locks = j["__meta"].value("locks", std::string());
    return ("," + locks + ",").find("," + key + ",") != std::string::npos;
}
bool CharacterCardStore::lockCard(const std::string& user, const std::string& scope,
                                  const std::string& key) {
    return lockCardByName(user, boundCard(user, scope), key);
}
bool CharacterCardStore::lockCardByName(const std::string& user, const std::string& card,
                                        const std::string& key) {
    if (key.empty()) return false;
    json j = rawCardJson(user, card);
    if (!j["__meta"].is_object()) j["__meta"] = json::object();
    std::string locks = j["__meta"].value("locks", std::string());
    if (("," + locks + ",").find("," + key + ",") != std::string::npos) return false;   // 已锁
    j["__meta"]["locks"] = locks.empty() ? key : (locks + "," + key);
    saveRawCardJson(user, card, j);
    return true;
}
bool CharacterCardStore::unlockCard(const std::string& user, const std::string& scope,
                                    const std::string& key) {
    return unlockCardByName(user, boundCard(user, scope), key);
}
bool CharacterCardStore::unlockCardByName(const std::string& user, const std::string& card,
                                          const std::string& key) {
    if (key.empty()) return false;
    json j = rawCardJson(user, card);
    if (!j.contains("__meta") || !j["__meta"].is_object()) return false;
    std::string locks = j["__meta"].value("locks", std::string());
    std::string padded = "," + locks + ",";
    auto p = padded.find("," + key + ",");
    if (p == std::string::npos) return false;
    padded.erase(p, key.size() + 1);
    std::string out = padded.substr(1, padded.size() >= 2 ? padded.size() - 2 : 0);
    j["__meta"]["locks"] = out;
    saveRawCardJson(user, card, j);
    return true;
}

std::map<std::string, std::string> CharacterCardStore::getTexts(const std::string& user,
                                                                 const std::string& scope) const {
    return getTextsByName(user, boundCard(user, scope));
}
std::map<std::string, std::string> CharacterCardStore::getTextsByName(
        const std::string& user, const std::string& cardName) const {
    std::map<std::string, std::string> out;
    json j = rawCardJson(user, cardName);
    if (j.contains("__meta") && j["__meta"].is_object()
        && j["__meta"].contains("texts") && j["__meta"]["texts"].is_object())
        for (auto it = j["__meta"]["texts"].begin(); it != j["__meta"]["texts"].end(); ++it)
            if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    return out;
}
void CharacterCardStore::setText(const std::string& user, const std::string& scope,
                                 const std::string& name, const std::string& value) {
    setTextByName(user, boundCard(user, scope), name, value);
}
void CharacterCardStore::setTextByName(const std::string& user, const std::string& card,
                                       const std::string& name, const std::string& value) {
    json j = rawCardJson(user, card);
    if (!j["__meta"].is_object()) j["__meta"] = json::object();
    if (!j["__meta"]["texts"].is_object()) j["__meta"]["texts"] = json::object();
    if (value.empty()) j["__meta"]["texts"].erase(name);
    else j["__meta"]["texts"][name] = value;
    saveRawCardJson(user, card, j);
}

// ─── Binding (which card is active in a scope) ───────────────

std::map<std::string, std::string> CharacterCardStore::getExpressionsByName(
        const std::string& user, const std::string& card) const {
    std::map<std::string, std::string> out;
    auto j = rawCardJson(user, card);
    if (!j.contains("__meta") || !j["__meta"].is_object()) return out;
    // Read previously imported cards too; new edits use the native key.
    for (const auto* name : {"legacyExpressions", "expressions"}) {
        const auto& meta = j["__meta"];
        if (!meta.contains(name) || !meta[name].is_object()) continue;
        for (const auto& [key, value] : meta[name].items())
            if (value.is_string()) out[canonical(key)] = value.get<std::string>();
    }
    return out;
}

json CharacterCardStore::getValuesByName(const std::string& user, const std::string& card) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto raw = rawCardJson(user, card);
    json values = json::object();
    for (const auto& [key, value] : raw.items())
        if (!key.empty() && key.rfind("__", 0) != 0) values[key] = value;
    const auto meta = raw.value("__meta", json::object());
    if (meta.is_object() && meta.contains("texts") && meta["texts"].is_object())
        for (const auto& [key, value] : meta["texts"].items())
            if (key.rfind("__", 0) != 0) values[key] = value;
    for (const auto* field : {"legacyExpressions", "expressions"})
        if (meta.is_object() && meta.contains(field) && meta[field].is_object())
            for (const auto& [key, value] : meta[field].items())
                if (!key.empty() && key.rfind("__", 0) != 0 && value.is_string()) values["&" + key] = value;
    return values;
}

void CharacterCardStore::replaceValuesByName(const std::string& user, const std::string& card,
        const json& values) {
    if (!values.is_object()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto previous = rawCardJson(user, card);
    auto meta = previous.value("__meta", json::object());
    if (!meta.is_object()) meta = json::object();
    meta.erase("texts"); meta.erase("expressions"); meta.erase("legacyExpressions");
    json data = json::object();
    for (const auto& [key, value] : values.items()) {
        if (key.empty() || key.rfind("__", 0) == 0) continue;
        if (key.front() == '&') {
            if (key.size() > 1 && value.is_string()) meta["expressions"][key.substr(1)] = value;
        } else if (value.is_string()) meta["texts"][key] = value;
        else data[key] = value;
    }
    if (!meta.empty()) data["__meta"] = std::move(meta);
    saveRawCardJson(user, card, data);
}

std::string CharacterCardStore::displayName(const std::string& user, const std::string& card) const {
    if (!card.empty()) return card;
    auto value = getValueByName(user, card, "__Name");
    return value && value->is_string() ? value->get<std::string>() : card;
}

std::optional<json> CharacterCardStore::getValueByName(
        const std::string& user, const std::string& card, const std::string& key) const {
    auto j = rawCardJson(user, card);
    if (!j.is_object()) return std::nullopt;
    const json meta = j.value("__meta", json::object());
    if (key == "__Name") {
        if (card.empty() && meta.is_object() && meta.contains("displayName") && meta["displayName"].is_string())
            return std::optional<json>(std::in_place, meta["displayName"]);
        return std::optional<json>(std::in_place, card);
    }
    if (key == "__Type" && meta.is_object()) {
        for (const auto* field : {"type", "legacyType"})
            if (meta.contains(field) && meta[field].is_string() && !meta[field].get<std::string>().empty())
                return std::optional<json>(std::in_place, meta[field]);
    }
    const bool expressionOnly = !key.empty() && key.front() == '&';
    const auto canonicalKey = canonical(expressionOnly ? key.substr(1) : key);
    if (!expressionOnly) {
        if (auto it = findAttribute(j, key); it != j.end()) return std::optional<json>(std::in_place, *it);
        if (meta.is_object() && meta.contains("texts") && meta["texts"].is_object()) {
            const auto& texts = meta["texts"];
            if (auto it = findAttribute(texts, key); it != texts.end()) return std::optional<json>(std::in_place, *it);
        }
    }
    const auto expressions = getExpressionsByName(user, card);
    if (auto it = expressions.find(canonicalKey); it != expressions.end()) return it->second;
    return std::nullopt;
}

void CharacterCardStore::setValueByName(const std::string& user, const std::string& card,
        const std::string& key, const json& value) {
    if (key.empty() || key == "__meta" || canonical(key) == "__meta") return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto j = rawCardJson(user, card);
    if (!j["__meta"].is_object()) j["__meta"] = json::object();
    auto& meta = j["__meta"];
    if (key == "__Type" || key == "__Name") {
        const char* metaKey = key == "__Type" ? "type" : "displayName";
        if (value.is_null()) meta.erase(metaKey);
        else if (value.is_string()) meta[metaKey] = value;
    } else if (key.front() == '&') {
        const auto name = canonical(key.substr(1));
        if (!meta["expressions"].is_object()) meta["expressions"] = json::object();
        // Remove obsolete raw-copy fallback when deleting or editing.
        if (meta.contains("legacyExpressions")) eraseAttribute(meta["legacyExpressions"], name);
        eraseAttribute(meta["expressions"], name);
        if (value.is_string()) meta["expressions"][name] = value;
    } else {
        const auto name = canonical(key);
        eraseAttribute(j, name);
        if (meta.contains("texts")) eraseAttribute(meta["texts"], name);
        if (!value.is_null()) j[name] = value;
    }
    saveRawCardJson(user, card, j);
}

std::string CharacterCardStore::boundCard(const std::string& user,
                                          const std::string& scope) const {
    auto* st = db_.getStorage();
    if (!st) return "";
    try {
        auto rows = st->get_all<UserSettingRow>(
            orm::where(orm::c(&UserSettingRow::userId) == user
                       and orm::c(&UserSettingRow::groupId) == scope
                       and orm::c(&UserSettingRow::key) == std::string("cardBind")));
        if (!rows.empty()) return rows.front().value;
        if (!scope.empty()) {
            rows = st->get_all<UserSettingRow>(orm::where(orm::c(&UserSettingRow::userId) == user
                and orm::c(&UserSettingRow::groupId) == std::string()
                and orm::c(&UserSettingRow::key) == std::string("cardBind")));
            if (!rows.empty()) return rows.front().value;
        }
    } catch (...) {}
    return "";
}

void CharacterCardStore::bindCard(const std::string& user, const std::string& scope,
                                  const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getStorage();
    if (!st) return;
    try {
        auto rows = st->get_all<UserSettingRow>(
            orm::where(orm::c(&UserSettingRow::userId) == user
                       and orm::c(&UserSettingRow::groupId) == scope
                       and orm::c(&UserSettingRow::key) == std::string("cardBind")));
        if (rows.empty()) {
            UserSettingRow r;
            r.userId = user; r.groupId = scope; r.key = "cardBind"; r.value = name;
            st->insert(r);
        } else {
            auto r = rows.front(); r.value = name; st->update(r);
        }
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CardStore: bindCard failed: {}", e.what());
    }
}

// ─── Active-card attribute API ───────────────────────────────

std::map<std::string, int> CharacterCardStore::getAttrs(
        const std::string& user, const std::string& scope) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto attrs = attrsOf(user, boundCard(user, scope));
    std::map<std::string, int> values;
    // Explicit canonical fields win a collision; raw JSON remains intact for
    // audit/typed export. Old alias-only cards work without destructive rewrites.
    for (const auto& [name, value] : attrs)
        if (name.rfind("__", 0) != 0 && canonical(name) == name) values[name] = value;
    for (const auto& [name, value] : attrs)
        if (name.rfind("__", 0) != 0) values.try_emplace(canonical(name), value);
    return values;
}

std::optional<int> CharacterCardStore::getAttr(
        const std::string& user, const std::string& scope, const std::string& name) const {
    return getAttrByName(user, boundCard(user, scope), name);
}
std::optional<int> CharacterCardStore::getAttrByName(
        const std::string& user, const std::string& cardName, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto attrs = attrsOf(user, cardName);
    if (auto exact = attrs.find(name); exact != attrs.end()) return exact->second;
    auto it = attrs.find(canonical(name));
    if (it != attrs.end()) return it->second;
    for (const auto& [key, value] : attrs) if (sameAttribute(key, name)) return value;
    return std::nullopt;
}

void CharacterCardStore::setAttr(const std::string& user, const std::string& scope,
                                 const std::string& name, int value) {
    setAttrByName(user, boundCard(user, scope), name, value);
}
void CharacterCardStore::setAttrByName(const std::string& user, const std::string& card,
                                       const std::string& name, int value) {
    setValueByName(user, card, name, value);
}

bool CharacterCardStore::clear(const std::string& user, const std::string& scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string card = boundCard(user, scope);
    auto raw = rawCardJson(user, card);
    json empty = json::object();
    if (raw.contains("__meta") && raw["__meta"].is_object()) {
        auto meta = raw["__meta"];
        meta.erase("texts"); meta.erase("expressions"); meta.erase("legacyExpressions");
        if (!meta.empty()) empty["__meta"] = std::move(meta);
    }
    if (raw == empty) return false;
    saveRawCardJson(user, card, empty);
    return true;
}

bool CharacterCardStore::eraseAttr(const std::string& user, const std::string& scope,
                                   const std::string& name) {
    return eraseAttrByName(user, boundCard(user, scope), name);
}
bool CharacterCardStore::eraseAttrByName(const std::string& user, const std::string& card,
                                         const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name.empty() || canonical(name) == "__meta" || name == "__Name" || name == "__Type") return false;
    auto raw = rawCardJson(user, card);
    const bool expressionOnly = name.front() == '&';
    const auto key = expressionOnly ? name.substr(1) : name;
    bool removed = !expressionOnly && eraseAttribute(raw, key);
    if (raw.contains("__meta") && raw["__meta"].is_object()) {
        auto& meta = raw["__meta"];
        if (!expressionOnly && meta.contains("texts")) removed = eraseAttribute(meta["texts"], key) || removed;
        for (const auto* field : {"expressions", "legacyExpressions"})
            if (meta.contains(field)) removed = eraseAttribute(meta[field], key) || removed;
    }
    if (!removed) return false;
    saveRawCardJson(user, card, raw);
    return true;
}

// ─── Multi-card management (.pc) ─────────────────────────────

std::vector<std::string> CharacterCardStore::listCards(const std::string& user) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    std::set<std::string> seen;
    auto* st = db_.getCardStorage();
    if (!st) return names;
    try {
        auto rows = st->get_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user),
            orm::order_by(&CharacterCardRow::updatedAt).desc());
        for (const auto& r : rows)
            if (seen.insert(r.name).second) names.push_back(r.name);
    } catch (const std::exception& e) {
        DICE_LOG_WARN("CardStore: listCards failed: {}", e.what());
    }
    return names;
}

bool CharacterCardStore::cardExists(const std::string& user, const std::string& name) const {
    auto* st = db_.getCardStorage();
    if (!st) return false;
    try {
        return st->count<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == name)) > 0;
    } catch (...) {}
    return false;
}

bool CharacterCardStore::createCard(const std::string& user, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cardExists(user, name)) return false;
    auto* st = db_.getCardStorage();
    if (!st) return false;
    try {
        CharacterCardRow row;
        row.userId = user; row.groupId = ""; row.name = name;
        row.attrs = "{}"; row.updatedAt = nowIso();
        st->insert(row);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CardStore: createCard failed: {}", e.what());
        return false;
    }
}

bool CharacterCardStore::deleteCard(const std::string& user, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cardExists(user, name)) return false;
    auto* st = db_.getCardStorage();
    if (!st) return false;
    try {
        st->remove_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == name));
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CardStore: deleteCard failed: {}", e.what());
        return false;
    }
}

bool CharacterCardStore::renameCard(const std::string& user, const std::string& oldName,
                                    const std::string& newName) {
    if (oldName == newName) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cardExists(user, oldName)) return false;
    if (cardExists(user, newName)) return false;       // would collide
    auto* cst = db_.getCardStorage();   // cards live in cards.db
    auto* st  = db_.getStorage();       // bindings live in the main db
    if (!cst || !st) return false;
    try {
        // Rename the card row(s) in cards.db.
        auto rows = cst->get_all<CharacterCardRow>(
            orm::where(orm::c(&CharacterCardRow::userId) == user
                       and orm::c(&CharacterCardRow::name) == oldName));
        for (auto r : rows) { r.name = newName; r.updatedAt = nowIso(); cst->update(r); }
        // Repoint any scope bindings that referenced the old name (main db).
        auto binds = st->get_all<UserSettingRow>(
            orm::where(orm::c(&UserSettingRow::userId) == user
                       and orm::c(&UserSettingRow::key) == std::string("cardBind")
                       and orm::c(&UserSettingRow::value) == oldName));
        for (auto b : binds) { b.value = newName; st->update(b); }
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("CardStore: renameCard failed: {}", e.what());
        return false;
    }
}

int CharacterCardStore::attrCount(const std::string& user, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(attrsOf(user, name).size());
}
bool CharacterCardStore::copyCard(const std::string& user, const std::string& source,
                                  const std::string& destination) {
    if (destination.empty() || !cardExists(user, source)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    saveRawCardJson(user, destination, rawCardJson(user, source));
    return true;
}

std::map<std::string, std::string> CharacterCardStore::listBindings(
        const std::string& user) const {
    std::map<std::string, std::string> out;
    auto* st = db_.getStorage();
    if (!st) return out;
    try {
        auto rows = st->get_all<UserSettingRow>(
            orm::where(orm::c(&UserSettingRow::userId) == user
                       and orm::c(&UserSettingRow::key) == std::string("cardBind")));
        for (const auto& row : rows) if (!row.value.empty()) out[row.groupId] = row.value;
    } catch (...) {}
    return out;
}

nlohmann::json CharacterCardStore::exportCard(const std::string& user,
                                               const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nlohmann::json{{"name", name}, {"data", rawCardJson(user, name)}};
}

std::optional<CharacterCardStore::Snapshot> CharacterCardStore::snapshot(
        const std::string& user, const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getCardStorage(); if (!st) return std::nullopt;
    auto rows = st->get_all<CharacterCardRow>(orm::where(
        orm::c(&CharacterCardRow::userId) == user and orm::c(&CharacterCardRow::name) == name));
    if (rows.size() != 1) return std::nullopt;
    auto data = json::parse(rows.front().attrs, nullptr, false);
    if (!data.is_object()) return std::nullopt;
    return Snapshot{rows.front().id, rows.front().name, std::move(data)};
}

std::optional<CharacterCardStore::Snapshot> CharacterCardStore::snapshotById(
        const std::string& user, int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getCardStorage(); if (!st) return std::nullopt;
    auto rows = st->get_all<CharacterCardRow>(orm::where(
        orm::c(&CharacterCardRow::userId) == user and orm::c(&CharacterCardRow::id) == id));
    if (rows.size() != 1) return std::nullopt;
    auto data = json::parse(rows.front().attrs, nullptr, false);
    if (!data.is_object()) return std::nullopt;
    return Snapshot{rows.front().id, rows.front().name, std::move(data)};
}

std::optional<CharacterCardStore::Snapshot> CharacterCardStore::importSnapshot(
        const std::string& user, const std::string& name, const json& data,
        const std::optional<Snapshot>& expected) {
    if (name.empty() || !data.is_object()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    auto* st = db_.getCardStorage(); if (!st) return std::nullopt;
    auto rows = st->get_all<CharacterCardRow>(orm::where(
        orm::c(&CharacterCardRow::userId) == user and orm::c(&CharacterCardRow::name) == name));
    CharacterCardRow row;
    if (expected) {
        if (rows.size() != 1 || rows.front().id != expected->id || name != expected->name ||
            json::parse(rows.front().attrs, nullptr, false) != expected->data) return std::nullopt;
        row = rows.front();
        // Cloud import must not bypass a local write lock.
        auto meta = expected->data.value("__meta", json::object());
        if (meta.is_object() && meta.contains("locks") && meta["locks"].is_string()) {
            std::istringstream locks(meta["locks"].get<std::string>()); std::string key;
            while (std::getline(locks, key, ',')) if (key == "w") return std::nullopt;
        }
    } else {
        if (!rows.empty()) return std::nullopt;
        row.userId = user; row.name = name;
    }
    row.attrs = data.dump(); row.updatedAt = nowIso();
    if (expected) {
        // Also defend against writers outside this store's mutex (e.g. WebUI).
        st->update_all(orm::set(orm::c(&CharacterCardRow::attrs) = row.attrs,
                              orm::c(&CharacterCardRow::updatedAt) = row.updatedAt),
            orm::where(orm::c(&CharacterCardRow::id) == row.id and
                       orm::c(&CharacterCardRow::userId) == user and
                       orm::c(&CharacterCardRow::name) == name and
                       orm::c(&CharacterCardRow::attrs) == rows.front().attrs));
        if (st->changes() != 1) return std::nullopt;
    } else row.id = st->insert(row);
    return Snapshot{row.id, row.name, data};
}

}  // namespace dice
