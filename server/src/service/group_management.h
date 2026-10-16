#pragma once

#include "../storage/group_account_settings.h"
#include <map>
#include <stdexcept>
#include <string>

namespace dice::group_management {

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

} // namespace dice::group_management
