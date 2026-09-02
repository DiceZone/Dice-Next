#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace dice::master_delivery {

/// One configured master delivery endpoint. canonicalId is populated only when
/// the identity binding store can prove that this endpoint belongs to a shared
/// public identity (normally a real QQ number).
struct Recipient {
    std::string platform;
    std::string adapterId;
    std::string id;
    std::string canonicalId;
};

inline std::string identityKey(const Recipient& recipient) {
    if (!recipient.canonicalId.empty()) return "identity\x1f" + recipient.canonicalId;
    return "endpoint\x1f" + recipient.platform + "\x1f" + recipient.adapterId + "\x1f" + recipient.id;
}

/// Preserve configured order while grouping only endpoints that are either an
/// exact duplicate or explicitly joined by the identity binding store.
inline std::vector<std::vector<Recipient>> groupRecipients(const std::vector<Recipient>& recipients) {
    std::vector<std::vector<Recipient>> groups;
    std::unordered_map<std::string, std::size_t> positions;
    for (const auto& recipient : recipients) {
        const std::string key = identityKey(recipient);
        auto [it, inserted] = positions.emplace(key, groups.size());
        if (inserted) groups.push_back({});
        groups[it->second].push_back(recipient);
    }
    return groups;
}

/// Prefer a real QQ/OneBot route when several live transports represent the
/// same person; QQ Official remains the fallback when OneBot is unavailable.
inline int transportPriority(const Recipient& recipient) {
    if (recipient.platform == "onebot_v11" || recipient.platform == "milky" ||
        (recipient.platform.empty() && !recipient.canonicalId.empty())) return 0;
    if (recipient.platform == "qq_official") return 1;
    return 2;
}

} // namespace dice::master_delivery
