#pragma once

#include <string>
#include <string_view>

namespace dice::reply_channel_scope {

inline std::string validate(std::string_view scope, std::string_view target) {
    if (scope == "global") return target.empty() ? "" : "global reply scope must not have a target";
    if (scope != "adapter" && scope != "account") return "invalid reply channel scope";
    return target.empty() ? "reply channel target required" : "";
}

inline bool allows(std::string_view scope, std::string_view target,
                   std::string_view platform, std::string_view adapterId) {
    if (scope == "global") return target.empty();
    if (target.empty()) return false;
    if (scope == "adapter") return target == platform;
    if (scope == "account") return !adapterId.empty() && target == adapterId;
    return false; // Never widen an invalid scope to global.
}

inline int rank(std::string_view scope) {
    return scope == "account" ? 0 : scope == "adapter" ? 1 : 2;
}

} // namespace dice::reply_channel_scope
