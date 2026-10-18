#pragma once

#include "../storage/group_account_settings.h"
#include <map>
#include <stdexcept>
#include <string>

namespace dice::group_management {

template <typename Storage>
void setSharedSetting(Storage& st, const std::string& platform, const std::string& groupId,
                       const std::string& key, const std::string& value) {
    namespace orm = sqlite_orm;
    const auto rows = st.template get_all<GroupSettingRow>(orm::where(
        orm::c(&GroupSettingRow::platform) == platform and
        orm::c(&GroupSettingRow::groupId) == groupId and orm::c(&GroupSettingRow::key) == key));
    if (rows.empty()) {
        GroupSettingRow row; row.platform = platform; row.groupId = groupId;
        row.key = key; row.value = value; st.insert(row);
    } else { auto row = rows.front(); row.value = value; st.update(row); }
}

// Only a genuine join or an explicit administrator restore clears a leave intent.
// Other bot accounts in the same public group retain their own tombstones.
template <typename Storage>
void restoreRecord(Storage& st, const std::string& adapterId, const std::string& platform,
                    const std::string& groupId, const std::string& endpointId = {}) {
    std::lock_guard lock(groupPresenceMutex());
    namespace orm = sqlite_orm;
    for (const auto* key : {"__removed", "left", "leaving", kLeaveRequestedAt})
        st.template remove_all<GroupSettingRow>(orm::where(
            orm::c(&GroupSettingRow::platform) == platform and
            orm::c(&GroupSettingRow::groupId) == groupId and
            orm::c(&GroupSettingRow::key) == std::string(key)));
    if (!adapterId.empty()) {
        for (const auto* key : {"__removed", "left", "leaving", kLeaveRequestedAt})
            setAccountGroupSetting(st, adapterId, platform, groupId, endpointId, key,
                key == kLeaveRequestedAt ? "" : "0");
    } else {
        auto rows = st.template get_all<GroupAccountSettingRow>(orm::where(
            orm::c(&GroupAccountSettingRow::platform) == platform and
            orm::c(&GroupAccountSettingRow::groupId) == groupId));
        for (auto& row : rows)
            if (row.key == "__removed" || row.key == "left" || row.key == "leaving" || row.key == kLeaveRequestedAt) {
                row.value = row.key == kLeaveRequestedAt ? "" : "0"; st.update(row);
            }
    }
}

// Clear management settings only: never cards, transcripts, identities or another
// account's rows. A retired record keeps a leave intent against delayed messages.
template <typename Storage>
void removeRecord(Storage& st, const std::string& adapterId, const std::string& platform,
                   const std::string& groupId, const std::string& requestedEndpoint = {},
                   bool onlyIfLeft = false) {
    std::lock_guard lock(groupPresenceMutex());
    namespace orm = sqlite_orm;
    const auto accounts = st.template get_all<GroupAccountSettingRow>(orm::where(
        orm::c(&GroupAccountSettingRow::groupId) == groupId));
    const auto left = accountGroupSetting(st, adapterId, platform, groupId, "left");
    if (onlyIfLeft && left != "1")
        throw std::invalid_argument("The group is no longer departed; refresh before removing records");
    std::string stamp = accountGroupSetting(st, adapterId, platform, groupId, kLeaveRequestedAt);
    if (stamp.empty() && (left == "1" || accountGroupSetting(st, adapterId, platform, groupId, "leaving") == "1"))
        stamp = leaveRequestStamp();
    if (!adapterId.empty()) {
        std::string endpoint;
        bool found = false;
        for (const auto& row : accounts) if (row.adapterId == adapterId) {
            found = true;
            if (row.platform != platform) throw std::invalid_argument("Group platform does not match the bot account");
            if (!row.endpointId.empty()) {
                if (!endpoint.empty() && endpoint != row.endpointId) throw std::invalid_argument("Inconsistent group binding");
                endpoint = row.endpointId;
            }
        }
        if (!found) throw std::invalid_argument("No group record for the selected bot account");
        if (!requestedEndpoint.empty() && !endpoint.empty() && requestedEndpoint != endpoint)
            throw std::invalid_argument("Group endpoint changed; refresh before removing records");
        st.template remove_all<GroupAccountSettingRow>(orm::where(
            orm::c(&GroupAccountSettingRow::adapterId) == adapterId and
            orm::c(&GroupAccountSettingRow::groupId) == groupId));
        setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, "__removed", "1");
        if (!stamp.empty()) {
            setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, kLeaveRequestedAt, stamp);
            setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, "left", "1");
        }
    } else {
        // A legacy item has no bot binding. Never let that fallback delete all
        // accounts when the caller really needs to select one specific bot.
        for (const auto& row : accounts)
            if (accountGroupSetting(st, row.adapterId, row.platform, groupId, "__removed") != "1")
                throw std::invalid_argument("Select a specific bot account before removing group records");
        st.template remove_all<GroupSettingRow>(orm::where(
            orm::c(&GroupSettingRow::platform) == platform and orm::c(&GroupSettingRow::groupId) == groupId));
        st.template remove_all<LocaleSettingRow>(orm::where(
            orm::c(&LocaleSettingRow::scope) == std::string("group") and
            orm::c(&LocaleSettingRow::scopeKey) == platform + ":" + groupId));
        setSharedSetting(st, platform, groupId, "__removed", "1");
        if (!stamp.empty()) setSharedSetting(st, platform, groupId, kLeaveRequestedAt, stamp);
    }
}

// Historical installations only recorded a day. Return it unchanged rather
// than pretending that a command occurred at midnight or at upgrade time.
inline std::string commandTime(const std::map<std::string, std::string>& settings) {
    for (const char* key : {"lastCommandAt", "lastActiveAt"}) {
        auto found = settings.find(key);
        if (found != settings.end() && !found->second.empty()) return found->second;
    }
    return {};
}

// A destructive action must name an actual account/group binding. Never
// silently fall back to another bot or trust a client-supplied native endpoint.
template <typename Storage>
std::string requireLeaveEndpoint(Storage& st, const std::string& adapterId,
                                 const std::string& platform, const std::string& groupId,
                                 const std::string& requestedEndpoint) {
    if (adapterId.empty()) throw std::invalid_argument("Select a specific bot account before leaving a group");
    namespace orm = sqlite_orm;
    const auto rows = st.template get_all<GroupAccountSettingRow>(orm::where(
        orm::c(&GroupAccountSettingRow::adapterId) == adapterId and
        orm::c(&GroupAccountSettingRow::groupId) == groupId));
    std::string endpoint;
    for (const auto& row : rows) {
        if (row.platform != platform) throw std::invalid_argument("Group platform does not match the bot account");
        if ((row.key == "left" || row.key == "__removed" || row.key == "leaving") && row.value == "1")
            throw std::invalid_argument("This bot has already left or is leaving the group; refresh the group list");
        if (row.endpointId.empty()) continue;
        if (!endpoint.empty() && endpoint != row.endpointId)
            throw std::invalid_argument("Inconsistent group binding; refresh the group list");
        endpoint = row.endpointId;
    }
    if (endpoint.empty()) throw std::invalid_argument("No group binding for the selected bot account");
    if (!requestedEndpoint.empty() && requestedEndpoint != endpoint)
        throw std::invalid_argument("Group endpoint changed; refresh the group list before leaving");
    return endpoint;
}

// Queue the leave and optionally remove its record in one lifecycle operation.
// Mark intent before invoking the adapter: queued inbound traffic cannot undo it.
template <typename Storage, typename Leave>
void requestLeave(Storage& st, const std::string& adapterId, const std::string& platform,
                   const std::string& groupId, const std::string& requestedEndpoint,
                   bool remove, Leave&& leave) {
    std::lock_guard lock(groupPresenceMutex());
    const auto endpoint = requireLeaveEndpoint(st, adapterId, platform, groupId, requestedEndpoint);
    const auto oldLeft = accountGroupSetting(st, adapterId, platform, groupId, "left");
    const auto oldStamp = accountGroupSetting(st, adapterId, platform, groupId, kLeaveRequestedAt);
    setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, kLeaveRequestedAt, leaveRequestStamp());
    setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, "left", "1");
    try { leave(endpoint); }
    catch (...) {
        setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, "left", oldLeft);
        setAccountGroupSetting(st, adapterId, platform, groupId, endpoint, kLeaveRequestedAt, oldStamp);
        throw;
    }
    if (remove) removeRecord(st, adapterId, platform, groupId, endpoint);
}

} // namespace dice::group_management
