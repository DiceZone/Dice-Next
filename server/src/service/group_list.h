#pragma once

#include "group_management.h"
#include "../adapter/adapter_manager.h"
#include "../adapter/qq_official_adapter.h"
#include "../config/config_manager.h"

namespace dice::group_management {

inline void discoverRecords(Database& db, AdapterManager& adapterMgr) {
    auto* st = db.getStorage();
    // 每次打开页面最多补查这么多个群的机器人身份，大量群时分批完成，避免一次性压给协议端。
    constexpr int kRoleLookupsPerPass = 100;
    for (const auto& adapter : adapterMgr.allAdapters()) {
        if (!adapter->isConnected()) continue;
        int roleLookups = 0;
        const auto platform = adapter->platform(), adapterId = adapter->id();
        std::map<std::string, std::string> live;
        for (const auto& [endpoint, name] : adapter->getGroupList()) {
            std::string group = endpoint;
            if (platform == "onebot_v11" || platform == "milky")
                group = identity::BindingStore::instance().observeQQ(db, platform, adapterId, endpoint, identity::Kind::Group);
            else if (platform == "discord" || platform == "kook")
                group = identity::BindingStore::instance().observeVirtual(db, platform, adapterId, endpoint, identity::Kind::Group);
            if (group.empty()) group = endpoint;
            std::lock_guard lock(groupPresenceMutex());
            if (groupDiscoveryBlocked(*st, adapterId, platform, group)) continue;
            live[group] = endpoint;
            setAccountGroupSetting(*st, adapterId, platform, group, endpoint, "__removed", "0");
            setAccountGroupSetting(*st, adapterId, platform, group, endpoint, "left", "0");
            if (accountGroupSetting(*st, adapterId, platform, group, "enabled").empty())
                setAccountGroupSetting(*st, adapterId, platform, group, endpoint, "enabled", "1");
            if (!name.empty() && name != endpoint) setSharedSetting(*st, platform, group, "name", name);
            if (roleLookups < kRoleLookupsPerPass && adapter->getSelfRole(endpoint).empty()) {
                adapter->refreshSelfRole(endpoint);
                ++roleLookups;
            }
        }
        adapter->refreshGroupList();
        std::lock_guard lock(groupPresenceMutex());
        syncAccountGroupPresence(*st, adapterId, platform, live);
    }
}

// The API and regression tests use the same account/legacy visibility rules.
inline nlohmann::json listRecords(Database& db, AdapterManager& adapterMgr, ConfigManager& cfg) {
    std::lock_guard lock(groupPresenceMutex());
    namespace orm = sqlite_orm;
    using J = nlohmann::json;
    auto* st = db.getStorage();
    auto rows = st->get_all<GroupSettingRow>();
    std::map<std::string, std::map<std::string, std::string>> groups; // "plat\x1fgid" → kv
    for (auto& r : rows) groups[r.platform + "\x1f" + r.groupId][r.key] = r.value;
    auto accountRows = st->get_all<GroupAccountSettingRow>();
    std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> accountGroups;
    std::map<std::string, std::pair<std::string, std::string>> accountMeta; // gid/aid -> platform, endpoint
    for (auto& r : accountRows) {
        accountGroups[r.groupId][r.adapterId][r.key] = r.value;
        accountMeta[r.groupId + "\x1f" + r.adapterId] = {r.platform, r.endpointId};
        // Ensure a group with no shared metadata is still represented.
        groups[r.platform + "\x1f" + r.groupId];
    }
    // 预载各群语言覆盖（locale_settings scope=group，key "<plat>:<gid>"）。
    std::map<std::string, std::string> groupLocales;
    for (auto& lr : st->get_all<LocaleSettingRow>(
             orm::where(orm::c(&LocaleSettingRow::scope) == std::string("group"))))
        groupLocales[lr.scopeKey] = lr.locale;
    // A real QQ group can be reached through more than one adapter.  The
    // management view represents that group once, and keeps every actual
    // adapter endpoint as metadata instead of rendering duplicate cards.
    std::map<std::string, std::string> selectedByGroup;
    for (auto& [pg, kv] : groups) {
        if (kv.count("__removed") && kv["__removed"] == "1") continue;
        auto sep = pg.find('\x1f');
        std::string plat = pg.substr(0, sep), gid = pg.substr(sep + 1);
        auto it = selectedByGroup.find(gid);
        if (it == selectedByGroup.end()
            || (it->second.rfind("qq_official\x1f", 0) == 0 && plat != "qq_official")) {
            selectedByGroup[gid] = pg;
        }
    }

    std::map<int64_t, IdentityRow> identities;
    std::map<std::string, J> groupBindings;
    try {
        for (const auto& row : st->get_all<IdentityRow>()) identities[row.id] = row;
        for (const auto& ep : st->get_all<IdentityEndpointRow>()) {
            auto it = identities.find(ep.identityId);
            if (it == identities.end() || it->second.kind != "group") continue;
            auto& list = groupBindings[it->second.publicId];
            if (!list.is_array()) list = J::array();
            list.push_back(J{{"adapterType", ep.adapterType}, {"adapterAccount", ep.adapterAccount},
                             {"endpointId", ep.endpointId}});
        }
    } catch (...) {}

    J arr = J::array();
    for (auto& [gid, pg] : selectedByGroup) {
        auto& kv = groups[pg];
        if (kv.count("__removed") && kv["__removed"] == "1") continue;   // 隐藏已移除的群
        auto sep = pg.find('\x1f');
        std::string plat = pg.substr(0, sep);
        std::string name = (kv.count("name") && !kv["name"].empty()) ? kv["name"] : gid;
        std::string lastCommandAt = group_management::commandTime(kv);
        J accounts = J::array();
        if (accountGroups.count(gid)) {
            for (auto& [aid, akv] : accountGroups[gid]) {
                if (akv.count("__removed") && akv["__removed"] == "1") continue;
                auto meta = accountMeta[gid + "\x1f" + aid];
                const std::string aplat = meta.first.empty() ? plat : meta.first;
                const std::string endpoint = meta.second.empty() ? gid : meta.second;
                auto value = [&](const std::string& key) -> std::string {
                    auto it = akv.find(key); if (it != akv.end()) return it->second;
                    auto old = kv.find(key); return old == kv.end() ? std::string() : old->second;
                };
                auto a = adapterMgr.getAdapter(aid);
                std::string accountName = aid, loginId, loginName, appId, botRole;
                int memberCount = 0;
                bool connected = a && a->isConnected();
                if (a) {
                    accountName = a->name(); loginId = a->getLoginId(); loginName = a->getLoginName();
                    if (auto off = std::dynamic_pointer_cast<QQOfficialAdapter>(a)) appId = off->appId();
                    std::string gn = a->getGroupName(endpoint);
                    if (name == gid && !gn.empty() && gn != endpoint) name = gn;
                    botRole = a->getSelfRole(endpoint);
                    memberCount = a->getGroupMemberCount(endpoint);
                }
                int observers = 0;
                try { auto o = J::parse(value("observers")); if (o.is_array()) observers = (int)o.size(); } catch (...) {}
                auto glIt = groupLocales.find(aplat + ":" + gid);
                const auto accountCommandAt = group_management::commandTime(akv);
                if (accountCommandAt > lastCommandAt) lastCommandAt = accountCommandAt;
                accounts.push_back(J{
                    {"adapterId", aid}, {"adapterName", accountName}, {"loginId", loginId},
                    {"loginName", loginName}, {"appId", appId},
                    {"platform", aplat}, {"endpointId", endpoint}, {"connected", connected},
                    {"canLeaveGroup", connected && a->supportsGroupLeave()}, {"lastCommandAt", accountCommandAt},
                    {"enabled", value("enabled") != "0"}, {"ai_enabled", value("aiEnabled") != "0"},
                    {"log_enabled", value("logEnabled") != "0"}, {"reply_enabled", value("replyDisabled") != "1"},
                    {"roll_enabled", value("rollEnabled") != "0"}, {"plugin_enabled", value("pluginEnabled") != "0"},
                    {"locked", value("locked") == "1"}, {"card", value("card")},
                    {"activeLog", !value("activeLog").empty()}, {"activeLogId", value("activeLog")},
                    {"activeLogName", value("activeLogName")}, {"observers", observers},
                    {"botRole", botRole}, {"memberCount", memberCount}, {"inviter", value("inviter")},
                    {"locale", value("locale").empty() ? (glIt != groupLocales.end() ? glIt->second : std::string()) : value("locale")},
                    {"left", value("left") == "1"}, {"welcome", value("welcome")},
                    {"welcome_delay", value("welcome_delay")}, {"welcome_cooldown", value("welcome_cooldown")}
                });
            }
        }

        // Compatibility for databases that have not observed an adapter
        // since upgrading: expose the old shared state as one legacy item.
        if (accounts.empty() && accountGroups.count(gid)) continue;
        if (accounts.empty()) {
            int observers = 0;
            if (kv.count("observers")) { try { auto o = J::parse(kv["observers"]); if (o.is_array()) observers = (int)o.size(); } catch (...) {} }
            auto glIt = groupLocales.find(plat + ":" + gid);
            accounts.push_back(J{{"adapterId", ""}, {"adapterName", ""}, {"loginId", ""},
                {"platform", plat}, {"endpointId", gid}, {"connected", false},
                {"canLeaveGroup", false}, {"lastCommandAt", group_management::commandTime(kv)},
                {"enabled", !kv.count("enabled") || kv["enabled"] != "0"},
                {"log_enabled", kv["logEnabled"] != "0"}, {"reply_enabled", kv["replyDisabled"] != "1"},
                {"roll_enabled", kv["rollEnabled"] != "0"}, {"plugin_enabled", kv["pluginEnabled"] != "0"},
                {"ai_enabled", !kv.count("aiEnabled") || kv["aiEnabled"] != "0"},
                {"locked", kv.count("locked") && kv["locked"] == "1"}, {"card", kv.count("card") ? kv["card"] : ""},
                {"activeLog", kv.count("activeLog") && !kv["activeLog"].empty()},
                {"activeLogId", kv.count("activeLog") ? kv["activeLog"] : ""}, {"activeLogName", kv.count("activeLogName") ? kv["activeLogName"] : ""},
                {"observers", observers}, {"botRole", ""}, {"memberCount", 0}, {"inviter", kv.count("inviter") ? kv["inviter"] : ""},
                {"locale", glIt != groupLocales.end() ? glIt->second : std::string()},
                {"left", kv.count("left") && kv["left"] == "1"}, {"welcome", kv.count("welcome") ? kv["welcome"] : ""},
                {"welcome_delay", kv.count("welcome_delay") ? kv["welcome_delay"] : ""},
                {"welcome_cooldown", kv.count("welcome_cooldown") ? kv["welcome_cooldown"] : ""}});
        }
        // Prefer a connected, still-joined account for the card summary.
        J selected = accounts.front();
        for (const auto& account : accounts)
            if (account.value("connected", false) && !account.value("left", false)) { selected = account; break; }
        arr.push_back(J{
            {"platform", selected.value("platform", plat)}, {"groupId", gid}, {"name", name},
            {"lastCommandAt", lastCommandAt}, {"canLeaveGroup", selected.value("canLeaveGroup", false)},
            {"enabled", selected.value("enabled", true)}, {"ai_enabled", selected.value("ai_enabled", true)},
            {"log_enabled", selected.value("log_enabled", true)}, {"reply_enabled", selected.value("reply_enabled", true)},
            {"roll_enabled", selected.value("roll_enabled", true)}, {"plugin_enabled", selected.value("plugin_enabled", true)},
            {"locked", selected.value("locked", false)}, {"card", selected.value("card", std::string())},
            {"remark", kv.count("remark") ? kv["remark"] : ""},
            {"activeLog", selected.value("activeLog", false)}, {"observers", selected.value("observers", 0)},
            {"botRole", selected.value("botRole", std::string())}, {"memberCount", selected.value("memberCount", 0)},
            {"inviter", selected.value("inviter", std::string())}, {"locale", selected.value("locale", std::string())},
            {"left", std::all_of(accounts.begin(), accounts.end(), [](const J& x){ return x.value("left", false); })},
            {"welcome", selected.value("welcome", std::string())},
            {"welcome_delay", selected.value("welcome_delay", std::string())},
            {"welcome_cooldown", selected.value("welcome_cooldown", std::string())},
            {"welcome_min_delay", cfg.get<int>("events/welcome_min_delay", 0)},
            {"welcome_min_cooldown", cfg.get<int>("events/welcome_min_cooldown", 0)},
            {"bindings", groupBindings.count(gid) ? groupBindings[gid] : J::array()}, {"accounts", accounts}
        });
    }
    return arr;
}

} // namespace dice::group_management
