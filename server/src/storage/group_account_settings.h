#pragma once
// ─── Issue #7: per-adapter-account group settings ─────────────
// group_account_settings holds switches/runtime state per concrete adapter
// account in a shared group (public group id), while group_settings remains
// the shared/public layer (name, remark, ...).  These helpers keep the WebUI
// group discovery and the chat pipeline consistent, and are unit-tested
// against the exact double-account overwrite regression.

#include "../storage/database.h"

#include <ctime>
#include <map>
#include <mutex>
#include <string>

namespace dice {

// Serialize only group lifecycle transitions, not ordinary command processing.
// A delayed inbound message must not interleave with deleting the account rows.
inline std::recursive_mutex& groupPresenceMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

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
        auto r = rows.front();
        // 群发现每次打开页面都会对所有群重写相同的值；值未变就不写，避免大量写事务逐个落盘。
        if (r.platform == platform && r.value == value && (endpointId.empty() || r.endpointId == endpointId))
            return;
        r.platform = platform;
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

/// 骰娘刚提出退群时，平台的群列表缓存里这个群还会留一阵（OneBot 退群本身延迟执行，列表
/// 还要再刷新一次）。这段时间里自动发现不能把它当成「仍在群里」——那会清掉刚写下的已退群、
/// 已删除标记，群又回到活跃列表（issue #54）。真重新入群时由入群事件清掉这个标记。
inline constexpr const char* kLeaveRequestedAt = "__leaveRequestedAt";
inline constexpr long long kLeaveGraceSeconds = 600;

// An explicit leave intent is durable until a real join event or manual restore.
// A cache entry (even after ten minutes) is not evidence of joining again.
template <typename Storage>
bool groupLeaveRequested(Storage& st, const std::string& adapterId,
                         const std::string& platform, const std::string& groupId) {
    return !accountGroupSetting(st, adapterId, platform, groupId, kLeaveRequestedAt).empty();
}

template <typename Storage>
bool groupDiscoveryBlocked(Storage& st, const std::string& adapterId,
                           const std::string& platform, const std::string& groupId) {
    return groupLeaveRequested(st, adapterId, platform, groupId)
        || accountGroupSetting(st, adapterId, platform, groupId, "__removed") == "1";
}

inline std::string leaveRequestStamp() { return std::to_string(static_cast<long long>(std::time(nullptr))); }

inline bool leaveRecentlyRequested(const std::string& stamp,
                                   long long now = static_cast<long long>(std::time(nullptr))) {
    if (stamp.empty()) return false;
    try {
        const long long at = std::stoll(stamp);
        return at <= now && now - at < kLeaveGraceSeconds;
    } catch (...) {
        return false;
    }
}

}  // namespace dice
