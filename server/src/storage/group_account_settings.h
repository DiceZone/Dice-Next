#pragma once
// ─── Issue #7: per-adapter-account group settings ─────────────
// group_account_settings holds switches/runtime state per concrete adapter
// account in a shared group (public group id), while group_settings remains
// the shared/public layer (name, remark, ...).  These helpers keep the WebUI
// group discovery and the chat pipeline consistent, and are unit-tested
// against the exact double-account overwrite regression.

#include "../storage/database.h"

#include <map>
#include <string>

namespace dice {

/// Read an account-scoped setting.  Falls back to the shared legacy row when
/// this adapter has not observed the group yet, so pre-issue#7 data keeps
/// working unchanged.
template <typename Storage>
std::string accountGroupSetting(Storage& st, const std::string& adapterId,
                                const std::string& platform, const std::string& groupId,
                                const std::string& key) {
    namespace orm = sqlite_orm;
    if (!adapterId.empty()) {
        auto rows = st.template get_all<GroupAccountSettingRow>(orm::where(
            orm::c(&GroupAccountSettingRow::adapterId) == adapterId and
            orm::c(&GroupAccountSettingRow::groupId) == groupId and
            orm::c(&GroupAccountSettingRow::key) == key));
        if (!rows.empty()) return rows.front().value;
    }
    auto shared = st.template get_all<GroupSettingRow>(orm::where(
        orm::c(&GroupSettingRow::platform) == platform and
        orm::c(&GroupSettingRow::groupId) == groupId and
        orm::c(&GroupSettingRow::key) == key));
    return shared.empty() ? std::string() : shared.front().value;
}

/// Upsert an account-scoped setting (creates the per-adapter row on first
/// write; never touches another adapter's rows).
template <typename Storage>
void setAccountGroupSetting(Storage& st, const std::string& adapterId,
                            const std::string& platform, const std::string& groupId,
                            const std::string& endpointId, const std::string& key,
                            const std::string& value) {
    namespace orm = sqlite_orm;
    auto rows = st.template get_all<GroupAccountSettingRow>(orm::where(
        orm::c(&GroupAccountSettingRow::adapterId) == adapterId and
        orm::c(&GroupAccountSettingRow::groupId) == groupId and
        orm::c(&GroupAccountSettingRow::key) == key));
    if (rows.empty()) {
        GroupAccountSettingRow r;
        r.adapterId = adapterId; r.platform = platform; r.groupId = groupId;
        r.endpointId = endpointId; r.key = key; r.value = value;
        st.insert(r);
    } else {
        auto r = rows.front(); r.platform = platform;
        if (!endpointId.empty()) r.endpointId = endpointId;
        r.value = value; st.update(r);
    }
}

/// Issue #7 regression: mark ONLY this adapter's still-recorded groups as
/// "left" when they disappear from its joined-group list.  Previously the
/// whole platform was scanned, so logging in a second OneBot account moved the
/// first account's groups into "left".  An empty live list (async list not
/// arrived yet) is deliberately treated as a no-op so a cold start can never
/// archive every group.
template <typename Storage>
void syncAccountGroupPresence(Storage& st, const std::string& adapterId,
                              const std::string& platform,
                              const std::map<std::string, std::string>& livePublicGroups) {
    if (adapterId.empty() || livePublicGroups.empty()) return;
    namespace orm = sqlite_orm;
    auto accountRows = st.template get_all<GroupAccountSettingRow>(orm::where(
        orm::c(&GroupAccountSettingRow::adapterId) == adapterId and
        orm::c(&GroupAccountSettingRow::key) == std::string("enabled")));
    for (auto& r : accountRows) {
        if (livePublicGroups.count(r.groupId)) continue;
        const std::string left = accountGroupSetting(st, adapterId, platform, r.groupId, "left");
        const std::string removed = accountGroupSetting(st, adapterId, platform, r.groupId, "__removed");
        const std::string leaving = accountGroupSetting(st, adapterId, platform, r.groupId, "leaving");
        if (left == "1" || removed == "1" || leaving == "1") continue;
        setAccountGroupSetting(st, adapterId, platform, r.groupId, r.endpointId, "left", "1");
    }
}

}  // namespace dice
