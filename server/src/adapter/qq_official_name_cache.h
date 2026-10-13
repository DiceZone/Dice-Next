#pragma once

#include "../storage/database.h"
#include "../core/identity/identity_binding.h"
#include "../common/utils.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dice {

// Only platform-supplied names belong here. Empty values, presentation fallbacks
// and failed requests must not replace a known name. OpenIDs/union IDs are scoped
// by AppID; sharing a name never grants permissions or merges public identities.
class QQOfficialNameCache {
public:
    static std::string text(const json& object, const char* field) {
        if (!object.is_object()) return {};
        const auto it = object.find(field);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }
    static std::string name(const std::string& value, const std::string& id = {}) {
        const auto trimmed = utils::trim(value);
        if (trimmed.empty() || trimmed.size() > 1024 || trimmed == id) return {};
        // Persist only valid UTF-8; do not turn malformed input into a name.
        try { (void)json(trimmed).dump(); } catch (...) { return {}; }
        return trimmed;
    }
    void attach(Database& db) {
        std::lock_guard lock(mu_);
        if (db_ != &db) { records_.clear(); db_ = &db; }
    }
    uint64_t revision() const { std::lock_guard lock(mu_); return revision_; }

    std::string rememberUser(const std::string& app, const std::string& openid,
                             const std::string& username, const std::string& unionId = {},
                             uint64_t observedBefore = (std::numeric_limits<uint64_t>::max)()) {
        if (app.empty() || openid.empty() || app.size() > 256 || openid.size() > 256) return {};
        std::string resolved;
        {
            std::lock_guard lock(mu_);
            const auto key = cacheKey(app, "user", openid);
            auto record = read(key);
            const auto validUnion = record.revision <= observedBefore && unionId.size() <= 256 ? unionId : std::string();
            bool changed = false;
            if (!validUnion.empty() && record.unionId != validUnion) { record.unionId = validUnion; changed = true; }
            const auto fresh = name(username, openid);
            auto united = record.unionId.empty() ? Record{} : read(cacheKey(app, "union", record.unionId));
            if (!fresh.empty() && record.revision <= observedBefore && united.revision <= observedBefore) {
                record.revision = ++revision_;
                if (record.value != fresh) { record.value = fresh; changed = true; }
                if (!record.unionId.empty()) {
                    const bool renamed = united.value != fresh;
                    united.value = fresh; united.revision = record.revision;
                    const auto unionKey = cacheKey(app, "union", record.unionId);
                    if (renamed) write(unionKey, united); else records_[unionKey] = united;
                }
            }
            if (changed) write(key, record);
            else records_[key] = record;
            resolved = united.value.empty() ? record.value : united.value;
        }
        if (!resolved.empty()) publishUser(app, openid, resolved);
        return resolved;
    }
    std::string user(const std::string& app, const std::string& openid) const {
        if (app.empty() || openid.empty() || app.size() > 256 || openid.size() > 256) return {};
        std::lock_guard lock(mu_);
        const auto record = read(cacheKey(app, "user", openid));
        const auto united = record.unionId.empty() ? Record{} : read(cacheKey(app, "union", record.unionId));
        return united.value.empty() ? record.value : united.value;
    }
    void rememberGroup(const std::string& app, const std::string& openid,
                       const std::string& groupName,
                       uint64_t observedBefore = (std::numeric_limits<uint64_t>::max)()) {
        const auto fresh = name(groupName, openid);
        if (app.empty() || openid.empty() || app.size() > 256 || openid.size() > 256 || fresh.empty()) return;
        bool accepted = false;
        {
            std::lock_guard lock(mu_);
            const auto key = cacheKey(app, "group", openid);
            auto record = read(key);
            if (record.revision > observedBefore) return;
            accepted = record.value != fresh;
            record.value = fresh; record.revision = ++revision_;
            if (accepted) write(key, record); else records_[key] = record;
        }
        if (accepted && db_ && db_->getStorage()) {
            try {
                const auto publicId = identity::BindingStore::instance().observeOfficial(*db_, app, openid, identity::Kind::Group);
                auto* st = db_->getStorage();
                auto rows = st->get_all<GroupSettingRow>(orm::where(
                    orm::c(&GroupSettingRow::platform) == std::string("qq_official") and
                    orm::c(&GroupSettingRow::groupId) == publicId and orm::c(&GroupSettingRow::key) == std::string("name")), orm::limit(1));
                if (rows.empty()) { GroupSettingRow row; row.platform = "qq_official"; row.groupId = publicId; row.key = "name"; row.value = fresh; st->insert(row); }
                else st->update_all(orm::set(orm::assign(&GroupSettingRow::value, fresh)), orm::where(orm::c(&GroupSettingRow::id) == rows.front().id));
            } catch (...) { /* The in-memory cache remains useful when persistence fails. */ }
        }
    }
    std::string group(const std::string& app, const std::string& openid) const {
        if (app.empty() || openid.empty() || app.size() > 256 || openid.size() > 256) return {};
        std::lock_guard lock(mu_);
        return read(cacheKey(app, "group", openid)).value;
    }

private:
    struct Record { std::string value, unionId; uint64_t revision = 0; };
    static std::string component(const std::string& value) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string encoded; encoded.reserve(value.size() * 2);
        for (unsigned char c : value) { encoded += hex[c >> 4]; encoded += hex[c & 15]; }
        return encoded;
    }
    static std::string cacheKey(const std::string& app, const char* kind, const std::string& id) {
        return "__qq_official_names/" + component(app) + "/" + kind + "/" + component(id);
    }
    Record read(const std::string& key) const {
        const auto found = records_.find(key);
        if (found != records_.end()) return found->second;
        Record record;
        if (db_ && db_->getStorage()) {
            try {
                auto rows = db_->getStorage()->get_all<DiceConfigRow>(orm::where(orm::c(&DiceConfigRow::key) == key), orm::limit(1));
                if (!rows.empty()) {
                    const auto saved = json::parse(rows.front().value, nullptr, false);
                    record.value = name(text(saved, "name"));
                    record.unionId = text(saved, "union_openid");
                    if (record.unionId.size() > 256) record.unionId.clear();
                }
            } catch (...) {}
        }
        if (records_.size() >= 4096) records_.clear(); // Persistent records are lazily reloaded.
        records_[key] = record;
        return record;
    }
    void write(const std::string& key, const Record& record) {
        if (records_.size() >= 4096 && !records_.count(key)) records_.clear();
        records_[key] = record;
        if (!db_ || !db_->getStorage()) return;
        try {
            // Multiple adapters may share an AppID; serialize cache upserts.
            static std::mutex persistenceMutex;
            std::lock_guard lock(persistenceMutex);
            auto* st = db_->getStorage();
            const auto value = json{{"name", record.value}, {"union_openid", record.unionId}}.dump();
            auto rows = st->get_all<DiceConfigRow>(orm::where(orm::c(&DiceConfigRow::key) == key), orm::limit(1));
            if (rows.empty()) { DiceConfigRow row; row.key = key; row.value = value; st->insert(row); }
            else st->update_all(orm::set(orm::assign(&DiceConfigRow::value, value)), orm::where(orm::c(&DiceConfigRow::id) == rows.front().id));
        } catch (...) {}
    }
    void publishUser(const std::string& app, const std::string& openid, const std::string& value) {
        if (!db_ || !db_->getStorage()) return;
        try {
            const auto publicId = identity::BindingStore::instance().observeOfficial(*db_, app, openid, identity::Kind::User);
            auto* st = db_->getStorage();
            auto rows = st->get_all<PlayerProfileRow>(orm::where(
                orm::c(&PlayerProfileRow::platform) == std::string("qq_official") and orm::c(&PlayerProfileRow::userId) == publicId), orm::limit(1));
            if (rows.empty()) {
                PlayerProfileRow row; row.platform = "qq_official"; row.userId = publicId; row.nickname = value;
                row.createdAt = utils::nowIso8601(); st->insert(row);
            } else if (rows.front().nickname != value) {
                st->update_all(orm::set(orm::assign(&PlayerProfileRow::nickname, value)), orm::where(orm::c(&PlayerProfileRow::id) == rows.front().id));
            }
        } catch (...) {}
    }
    mutable std::mutex mu_;
    mutable std::unordered_map<std::string, Record> records_;
    Database* db_ = nullptr;
    uint64_t revision_ = 0;
};

} // namespace dice
