#pragma once

#include <string>

namespace dice {

enum class FriendRequestDecision { kPending, kApprove, kReject };

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
