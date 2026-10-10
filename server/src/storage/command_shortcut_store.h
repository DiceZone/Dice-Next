#pragma once

#include "database.h"
#include "../adapter/adapter_interface.h"
#include "../core/command_shortcut.h"
#include <map>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace dice::shortcut {

// Reuse the existing settings tables: backups and verified .bind migrations
// already include them. Personal shortcuts are global to the canonical user;
// group shortcuts follow Next's per-adapter-account group isolation.
inline constexpr const char* kSettingPrefix = "shortcut:v1:";
inline std::mutex& storeMutex() { static std::mutex value; return value; }
using Entries = std::map<std::string, std::string>;
using Store = std::remove_pointer_t<decltype(std::declval<Database&>().getStorage())>;
enum class WriteResult { Added, Replaced, Removed, Missing, Limit, Error };

template<class Row> inline auto scopeRows(Store* st, const Message& msg) {
    if constexpr (std::is_same_v<Row, UserSettingRow>)
        return st->get_all<Row>(orm::where(orm::c(&Row::userId) == msg.senderId && orm::c(&Row::groupId) == std::string()));
    else if constexpr (std::is_same_v<Row, GroupAccountSettingRow>)
        return st->get_all<Row>(orm::where(orm::c(&Row::adapterId) == msg.adapterId && orm::c(&Row::groupId) == msg.targetId));
    else
        return st->get_all<Row>(orm::where(orm::c(&Row::platform) == msg.platform && orm::c(&Row::groupId) == msg.targetId));
}
template<class Rows> inline Entries entries(const Rows& rows) {
    Entries out;
    const std::string prefix = kSettingPrefix;
    for (const auto& row : rows) if (row.key.rfind(prefix, 0) == 0)
        out[row.key.substr(prefix.size())] = row.value;
    return out;
}
inline Entries list(Database& db, const Message& msg, bool personal) {
    std::lock_guard lock(storeMutex());
    auto* st = db.getStorage();
    if (!st) throw std::runtime_error("shortcut storage unavailable");
    if (personal) return entries(scopeRows<UserSettingRow>(st, msg));
    if (!msg.adapterId.empty()) return entries(scopeRows<GroupAccountSettingRow>(st, msg));
    return entries(scopeRows<GroupSettingRow>(st, msg));
}
template<class Row> inline WriteResult modify(Store* st, const Message& msg,
                                              const std::string& name, const std::optional<std::string>& target) {
    auto rows = scopeRows<Row>(st, msg);
    const std::string key = std::string(kSettingPrefix) + name;
    bool existed = false;
    for (auto& row : rows) if (row.key == key) {
        existed = true;
        if (target) { row.value = *target; st->update(row); }
        else st->remove<Row>(row.id);
    }
    if (existed) return target ? WriteResult::Replaced : WriteResult::Removed;
    if (!target) return WriteResult::Missing;
    if (entries(rows).size() >= kMaxEntries) return WriteResult::Limit;
    Row row;
    if constexpr (std::is_same_v<Row, UserSettingRow>) row.userId = msg.senderId;
    else {
        row.groupId = msg.targetId; row.platform = msg.platform;
        if constexpr (std::is_same_v<Row, GroupAccountSettingRow>) {
            row.adapterId = msg.adapterId;
            row.endpointId = msg.extra.is_object() ? msg.extra.value("__identity_native_target", msg.targetId) : msg.targetId;
        }
    }
    row.key = key; row.value = *target; st->insert(row);
    return WriteResult::Added;
}
inline WriteResult write(Database& db, const Message& msg, bool personal,
                         const std::string& name, const std::optional<std::string>& target) {
    std::lock_guard lock(storeMutex());
    auto* st = db.getStorage(); if (!st) return WriteResult::Error;
    try {
        if (personal) return modify<UserSettingRow>(st, msg, name, target);
        if (!msg.adapterId.empty()) return modify<GroupAccountSettingRow>(st, msg, name, target);
        return modify<GroupSettingRow>(st, msg, name, target);
    } catch (...) { return WriteResult::Error; }
}

} // namespace dice::shortcut
