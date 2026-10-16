#pragma once

#include <string>
#include <optional>

namespace dice {

enum class FriendRequestDecision { kPending, kApprove, kReject };
enum class LegacyRequestAction { kContinueHost, kLeavePending, kReject };
// Shared by friend/group hooks. A plugin may make a decision stricter, never
// turn an application the owner left for manual review into an automatic approval.
inline LegacyRequestAction legacyRequestAction(bool blocked, std::optional<bool> approval, bool hostAllowsApproval) {
    if (!blocked) return LegacyRequestAction::kContinueHost;
    if (approval && !*approval) return LegacyRequestAction::kReject;
    if (approval && *approval && hostAllowsApproval) return LegacyRequestAction::kContinueHost;
    return LegacyRequestAction::kLeavePending;
}

inline FriendRequestDecision evaluateFriendRequest(
        const std::string& policy,
        const std::string& comment,
        const std::string& keyword,
        bool hasGroupCommandHistory,
        bool isWhitelisted = false,
        bool isBlacklisted = false) {
    if (policy == "all") return FriendRequestDecision::kApprove;
    if (policy == "keyword") {
        return !keyword.empty() && comment.find(keyword) != std::string::npos
            ? FriendRequestDecision::kApprove : FriendRequestDecision::kPending;
    }
    if (policy == "group_used") {
        return hasGroupCommandHistory
            ? FriendRequestDecision::kApprove : FriendRequestDecision::kPending;
    }
    if (policy == "whitelist") {
        return isWhitelisted ? FriendRequestDecision::kApprove : FriendRequestDecision::kPending;
    }
    if (policy == "nonblacklist") {
        return isBlacklisted ? FriendRequestDecision::kPending : FriendRequestDecision::kApprove;
    }
    if (policy == "reject") return FriendRequestDecision::kReject;
    return FriendRequestDecision::kPending;
}

} // namespace dice
