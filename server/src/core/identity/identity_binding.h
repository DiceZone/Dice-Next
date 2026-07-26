#pragma once

// QQ identity registry.  A public QQ/群号 is always numeric; transport-specific
// identifiers (notably QQ Official OpenIDs) are stored in identity_endpoints.

#include "../../storage/database.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace dice::identity {
enum class Kind { Group, User };

class BindingStore {
public:
    static BindingStore& instance() { static BindingStore value; return value; }
    void attach(Database& db) {
        std::lock_guard lock(mu_);
        database_ = &db;
        migrateLegacyVirtualNumbers(db);
        cleanupOrphanVirtualGroupSettings(db);
    }
    Database* database() const { std::lock_guard lock(mu_); return database_; }
    static std::string kindName(Kind kind) { return kind == Kind::Group ? "group" : "user"; }
    static std::string prefix(Kind kind) { return kind == Kind::Group ? "QQGroup:" : "QQ:"; }
    static std::string qualified(Kind kind, const std::string& id) { return prefix(kind) + id; }
    static std::string officialId(const std::string& botId, const std::string& openId) { return "QQ-Official-" + botId + ":" + openId; }
    static bool isOfficialId(const std::string& value) {
        const auto p = value.find(':'); return value.rfind("QQ-Official-", 0) == 0 && p != std::string::npos && p > 12 && p + 1 < value.size();
    }
    static bool isRealQQ(const std::string& value) {
        if (value.size() < 5 || value.size() > 12) return false;
        return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    }
    static std::string rawQQ(const std::string& value) {
        if (value.rfind("QQ:", 0) == 0) return value.substr(3);
        if (value.rfind("QQGroup:", 0) == 0) return value.substr(8);
        return value;
    }
    static bool isVirtual(const std::string& id) {
        return id.size() == 13 && (id[0] == '1' || id[0] == '2');
    }

    // Observe a native QQ endpoint. Existing legacy numeric data retains the
    // same public ID, so Lua/JS compatibility is preserved.
    std::string observeQQ(Database& db, const std::string& adapterType, const std::string& account,
                          const std::string& raw, Kind kind) {
        if (raw.empty()) return {};
        std::lock_guard lock(mu_);
        return ensureEndpoint(db, adapterType, account, raw, kind, raw, false).publicId;
    }
    std::string observeOfficial(Database& db, const std::string& botId, const std::string& openId, Kind kind) {
        if (botId.empty() || openId.empty()) return {};
        std::lock_guard lock(mu_);
        auto row = ensureEndpoint(db, "qq_official", botId, openId, kind, {}, true);
        // Upgrade the short-lived v1 alias format (QQ:<id>/QQGroup:<id>) on
        // first sight, so existing test data is merged rather than duplicated.
        try {
            const auto key = "identity/v1/alias/" + officialId(botId, openId);
            auto old = db.getStorage()->get_all<DiceConfigRow>(orm::where(orm::c(&DiceConfigRow::key) == key), orm::limit(1));
            if (!old.empty()) {
                const auto real = rawQQ(old.front().value);
                if (isRealQQ(real)) {
                    auto target = findByPublic(db, real, kind); if (!target.id) target = createEntity(db, kind, real, false);
                    if (row.id != target.id) merge(db, row, target, kind);
                    migrateLegacyRows(db, kind, qualified(kind, real), real);
                    return real;
                }
            }
        } catch (...) {}
        if (row.isVirtual && row.publicId.size() != 13) {
            const std::string old = row.publicId;
            row.publicId = nextVirtual(db, kind); db.getStorage()->update(row);
            migrateLegacyRows(db, kind, old, row.publicId);
        }
        return row.publicId;
    }
    bool isKnownOfficial(Database& db, const std::string& value, Kind kind) {
        std::string bot, open; if (!parseOfficial(value, bot, open)) return false;
        std::lock_guard lock(mu_); return findEndpoint(db, "qq_official", bot, open, kind).id != 0;
    }
    std::string publicForOfficial(Database& db, const std::string& bot, const std::string& open, Kind kind) {
        std::lock_guard lock(mu_); auto ep = findEndpoint(db, "qq_official", bot, open, kind); return ep.id ? entity(db, ep.identityId).publicId : std::string();
    }
    std::string officialTransport(Database& db, const std::string& botId, const std::string& publicId, Kind kind) {
        std::lock_guard lock(mu_);
        auto rows = endpointsForPublic(db, publicId, kind);
        for (const auto& row : rows) if (row.adapterType == "qq_official" && row.adapterAccount == botId) return row.endpointId;
        return {};
    }
    std::vector<IdentityEndpointRow> endpoints(Database& db, const std::string& publicId, Kind kind) {
        std::lock_guard lock(mu_); return endpointsForPublic(db, publicId, kind);
    }

    // ─── 通用平台（Discord/KOOK 等）：与 QQ 官方同一套虚拟号模型 ──────
    // 平台原生 id 可能与 QQ 号数字碰撞（KOOK 用户 id 就是纯数字），一律经
    // 虚拟公共号隔离；绑定真实 QQ 后数据自动合并互通。

    /// 观察一个平台原生端点：已绑定 → 返回绑定的公共号；未绑定 → 分配/复用虚拟号。
    std::string observeVirtual(Database& db, const std::string& type, const std::string& account,
                               const std::string& raw, Kind kind) {
        if (type.empty() || raw.empty()) return {};
        std::lock_guard lock(mu_);
        auto row = ensureEndpoint(db, type, account, raw, kind, {}, true);
        if (row.isVirtual && row.publicId.size() != 13) {   // 升级早期短格式
            const std::string old = row.publicId;
            row.publicId = nextVirtual(db, kind); db.getStorage()->update(row);
            migrateLegacyRows(db, kind, old, row.publicId);
        }
        return row.publicId;
    }
    /// 公共号 → 该平台的原生 id（任一账号的端点均可；空 = 无此平台端点）。
    std::string transportEndpoint(Database& db, const std::string& type,
                                  const std::string& publicId, Kind kind) {
        std::lock_guard lock(mu_);
        for (const auto& row : endpointsForPublic(db, publicId, kind))
            if (row.adapterType == type) return row.endpointId;
        return {};
    }
    /// 该平台是否观察过此原生 id（.bind 防空绑）。
    bool isKnownPlatform(Database& db, const std::string& type, const std::string& raw, Kind kind) {
        std::lock_guard lock(mu_);
        return findEndpointAnyAccount(db, type, raw, kind).id != 0;
    }
    /// 把平台原生 id 绑定到真实 QQ/群号（虚拟数据合并迁移）。
    bool bindPlatformToQQ(Database& db, const std::string& type, const std::string& raw,
                          const std::string& real, Kind kind, std::string& error) {
        if (!isRealQQ(real)) { error = "绑定标识格式无效"; return false; }
        std::lock_guard lock(mu_);
        auto sourceEp = findEndpointAnyAccount(db, type, raw, kind);
        if (!sourceEp.id) { error = "未发现该平台身份；请先让机器人在该平台收到对方的一条消息"; return false; }
        auto source = entity(db, sourceEp.identityId);
        auto target = findByPublic(db, real, kind);
        if (!target.id) target = createEntity(db, kind, real, false);
        if (source.id != target.id) merge(db, source, target, kind);
        return true;
    }

    // Bind an observed QQ Official endpoint to a real QQ/群号. The real number
    // may be reserved before OneBot sees it. Existing virtual data is migrated.
    bool bindOfficialToQQ(Database& db, const std::string& official, const std::string& real,
                          Kind kind, std::string& error) {
        std::string bot, open;
        if (!parseOfficial(official, bot, open) || !isRealQQ(real)) { error = "绑定标识格式无效"; return false; }
        std::lock_guard lock(mu_);
        auto sourceEp = findEndpoint(db, "qq_official", bot, open, kind);
        if (!sourceEp.id) { error = "未发现该官方身份，不能空绑定 QQ-Official-xxx"; return false; }
        auto source = entity(db, sourceEp.identityId);
        auto target = findByPublic(db, real, kind);
        if (!target.id) target = createEntity(db, kind, real, false);
        if (source.id != target.id) merge(db, source, target, kind);
        migrateLegacyRows(db, kind, official, real);
        migrateLegacyRows(db, kind, qualified(kind, real), real);
        return true;
    }
    // Reverse binding from a real QQ/群 window to an already observed official endpoint.
    bool bindOfficialToCurrentQQ(Database& db, const std::string& official, const std::string& current,
                                 Kind kind, std::string& error) {
        return bindOfficialToQQ(db, official, current, kind, error);
    }

private:
    void migrateLegacyVirtualNumbers(Database& db) {
        auto* st = db.getStorage(); if (!st) return;
        try {
            const auto rows = st->get_all<IdentityRow>();
            for (auto row : rows) {
                if (!row.isVirtual || isVirtual(row.publicId)) continue;
                const Kind kind = row.kind == "group" ? Kind::Group : Kind::User;
                const std::string old = row.publicId;
                row.publicId = nextVirtual(db, kind);
                st->update(row);
                migrateLegacyRows(db, kind, old, row.publicId);
            }
        } catch (...) {}
    }
    // Older development builds could leave group_settings rows behind after a
    // virtual identity had already been merged and removed.  Such a row has no
    // transport endpoint and must not appear as a second group in WebUI.
    static void cleanupOrphanVirtualGroupSettings(Database& db) {
        auto* st = db.getStorage(); if (!st) return;
        try {
            std::vector<std::string> live;
            for (const auto& row : st->get_all<IdentityRow>(orm::where(
                     orm::c(&IdentityRow::kind) == std::string("group")))) {
                if (isVirtual(row.publicId)) live.push_back(row.publicId);
            }
            for (const auto& row : st->get_all<GroupSettingRow>()) {
                if (!isVirtual(row.groupId)
                    || std::find(live.begin(), live.end(), row.groupId) != live.end()) continue;
                st->remove<GroupSettingRow>(row.id);
            }
        } catch (...) {}
    }
    static std::string now() { return std::to_string(static_cast<long long>(std::time(nullptr))); }
    static bool parseOfficial(const std::string& value, std::string& bot, std::string& open) {
        if (!isOfficialId(value)) return false;
        const auto colon = value.find(':'); bot = value.substr(12, colon - 12); open = value.substr(colon + 1); return !bot.empty() && !open.empty();
    }
    IdentityRow ensureEndpoint(Database& db, const std::string& type, const std::string& account,
                               const std::string& raw, Kind kind, const std::string& wanted, bool virtualId) {
        if (auto ep = findEndpoint(db, type, account, raw, kind); ep.id) return entity(db, ep.identityId);
        IdentityRow row = wanted.empty() ? createEntity(db, kind, nextVirtual(db, kind), true) : findByPublic(db, wanted, kind);
        if (!row.id) row = createEntity(db, kind, wanted, virtualId);
        IdentityEndpointRow ep; ep.identityId = row.id; ep.kind = kindName(kind); ep.adapterType = type; ep.adapterAccount = account; ep.endpointId = raw; ep.createdAt = now();
        db.getStorage()->insert(ep); return row;
    }
    IdentityEndpointRow findEndpointAnyAccount(Database& db, const std::string& type,
                                               const std::string& raw, Kind kind) const {
        auto* st = db.getStorage(); if (!st) return {};
        try {
            auto rows = st->get_all<IdentityEndpointRow>(orm::where(
                orm::c(&IdentityEndpointRow::adapterType) == type and
                orm::c(&IdentityEndpointRow::endpointId) == raw and
                orm::c(&IdentityEndpointRow::kind) == kindName(kind)), orm::limit(1));
            return rows.empty() ? IdentityEndpointRow{} : rows.front();
        } catch (...) { return {}; }
    }
    IdentityEndpointRow findEndpoint(Database& db, const std::string& type, const std::string& account,
                                     const std::string& raw, Kind kind) const {
        auto* st = db.getStorage(); if (!st) return {};
        try { auto rows = st->get_all<IdentityEndpointRow>(orm::where(orm::c(&IdentityEndpointRow::adapterType) == type and orm::c(&IdentityEndpointRow::adapterAccount) == account and orm::c(&IdentityEndpointRow::endpointId) == raw and orm::c(&IdentityEndpointRow::kind) == kindName(kind)), orm::limit(1)); return rows.empty() ? IdentityEndpointRow{} : rows.front(); } catch (...) { return {}; }
    }
    IdentityRow findByPublic(Database& db, const std::string& publicId, Kind kind) const {
        auto* st = db.getStorage(); if (!st) return {};
        try { auto rows = st->get_all<IdentityRow>(orm::where(orm::c(&IdentityRow::publicId) == publicId and orm::c(&IdentityRow::kind) == kindName(kind)), orm::limit(1)); return rows.empty() ? IdentityRow{} : rows.front(); } catch (...) { return {}; }
    }
    IdentityRow entity(Database& db, int id) const { try { return db.getStorage()->get<IdentityRow>(id); } catch (...) { return {}; } }
    IdentityRow createEntity(Database& db, Kind kind, const std::string& publicId, bool virtualId) const {
        IdentityRow row; row.kind = kindName(kind); row.publicId = publicId; row.isVirtual = virtualId; row.createdAt = now(); row.id = (int)db.getStorage()->insert(row); return row;
    }
    std::string nextVirtual(Database& db, Kind kind) const {
        long long maxId = kind == Kind::Group ? 1000000000000LL : 2000000000000LL;
        try { for (const auto& row : db.getStorage()->get_all<IdentityRow>(orm::where(orm::c(&IdentityRow::kind) == kindName(kind)))) if (isVirtual(row.publicId)) maxId = (std::max)(maxId, std::stoll(row.publicId)); } catch (...) {}
        return std::to_string(maxId + 1);
    }
    std::vector<IdentityEndpointRow> endpointsForPublic(Database& db, const std::string& publicId, Kind kind) const {
        auto e = findByPublic(db, publicId, kind); if (!e.id) return {};
        try { return db.getStorage()->get_all<IdentityEndpointRow>(orm::where(orm::c(&IdentityEndpointRow::identityId) == e.id)); } catch (...) { return {}; }
    }
    void merge(Database& db, const IdentityRow& source, const IdentityRow& target, Kind kind) {
        auto* st = db.getStorage(); if (!st) return;
        try {
            for (auto ep : st->get_all<IdentityEndpointRow>(orm::where(orm::c(&IdentityEndpointRow::identityId) == source.id))) { ep.identityId = target.id; st->update(ep); }
            const std::string from = source.publicId, to = target.publicId;
            for (auto p : st->get_all<PlayerProfileRow>(orm::where(orm::c(&PlayerProfileRow::userId) == from))) { p.userId = to; st->update(p); }
            for (auto s : st->get_all<UserSettingRow>(orm::where(orm::c(&UserSettingRow::userId) == from))) { s.userId = to; st->update(s); }
            for (auto r : st->get_all<RollStatRow>(orm::where(orm::c(&RollStatRow::userId) == from))) { r.userId = to; st->update(r); }
            if (kind == Kind::Group) for (auto g : st->get_all<GroupSettingRow>(orm::where(orm::c(&GroupSettingRow::groupId) == from))) { g.groupId = to; st->update(g); }
            if (auto* cards = db.getCardStorage()) for (auto c : cards->get_all<CharacterCardRow>()) { bool changed=false; if (kind == Kind::User && c.userId == from) { c.userId = to; changed=true; } if (kind == Kind::Group && c.groupId == from) { c.groupId = to; changed=true; } if (changed) cards->update(c); }
            if (auto* chat = db.getChatStorage()) for (auto c : chat->get_all<ChatMsgRow>()) { bool changed=false; if (kind == Kind::User && c.userId == from) { c.userId = to; changed=true; } if (kind == Kind::Group && c.groupId == from) { c.groupId = to; changed=true; } if (changed) chat->update(c); }
            st->remove<IdentityRow>(source.id);
        } catch (...) {}
    }
    void migrateLegacyRows(Database& db, Kind kind, const std::string& from, const std::string& to) {
        auto* st = db.getStorage(); if (!st || from == to) return;
        try {
            for (auto p : st->get_all<PlayerProfileRow>(orm::where(orm::c(&PlayerProfileRow::userId) == from))) { p.userId = to; st->update(p); }
            for (auto s : st->get_all<UserSettingRow>(orm::where(orm::c(&UserSettingRow::userId) == from))) { s.userId = to; st->update(s); }
            for (auto r : st->get_all<RollStatRow>(orm::where(orm::c(&RollStatRow::userId) == from))) { r.userId = to; st->update(r); }
            if (kind == Kind::Group) for (auto g : st->get_all<GroupSettingRow>(orm::where(orm::c(&GroupSettingRow::groupId) == from))) { g.groupId = to; st->update(g); }
            if (auto* cards = db.getCardStorage()) for (auto c : cards->get_all<CharacterCardRow>()) { bool changed=false; if (kind == Kind::User && c.userId == from) { c.userId=to; changed=true; } if (kind == Kind::Group && c.groupId == from) { c.groupId=to; changed=true; } if (changed) cards->update(c); }
            if (auto* chat = db.getChatStorage()) for (auto c : chat->get_all<ChatMsgRow>()) { bool changed=false; if (kind == Kind::User && c.userId == from) { c.userId=to; changed=true; } if (kind == Kind::Group && c.groupId == from) { c.groupId=to; changed=true; } if (changed) chat->update(c); }
        } catch (...) {}
    }
    mutable std::mutex mu_;
    Database* database_ = nullptr;
};
} // namespace dice::identity
