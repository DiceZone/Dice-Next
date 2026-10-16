#include "test_framework.h"
#include "src/core/friend_approval_policy.h"

using dice::FriendRequestDecision;
using dice::evaluateFriendRequest;

TEST(FriendApproval, LegacyHookCannotGrantApprovalBeyondHostPolicy) {
    using dice::legacyRequestAction;
    using A = dice::LegacyRequestAction;
    ASSERT_TRUE(legacyRequestAction(false, true, false) == A::kContinueHost);
    ASSERT_TRUE(legacyRequestAction(true, true, false) == A::kLeavePending);
    ASSERT_TRUE(legacyRequestAction(true, std::nullopt, true) == A::kLeavePending);
    ASSERT_TRUE(legacyRequestAction(true, false, true) == A::kReject);
    ASSERT_TRUE(legacyRequestAction(true, false, false) == A::kReject);
    ASSERT_TRUE(legacyRequestAction(true, true, true) == A::kContinueHost);
}

TEST(FriendApproval, HandlesAllPolicies) {
    ASSERT_TRUE(evaluateFriendRequest("manual", "", "", true) == FriendRequestDecision::kPending);
    ASSERT_TRUE(evaluateFriendRequest("all", "", "", false) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("reject", "", "", true) == FriendRequestDecision::kReject);

    ASSERT_TRUE(evaluateFriendRequest("keyword", "hello dice", "dice", false) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("keyword", "hello", "dice", true) == FriendRequestDecision::kPending);
    ASSERT_TRUE(evaluateFriendRequest("keyword", "hello", "", true) == FriendRequestDecision::kPending);

    ASSERT_TRUE(evaluateFriendRequest("group_used", "", "", true) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("group_used", "", "", false) == FriendRequestDecision::kPending);

    ASSERT_TRUE(evaluateFriendRequest("whitelist", "", "", false, true, false) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("whitelist", "", "", true, false, false) == FriendRequestDecision::kPending);
    ASSERT_TRUE(evaluateFriendRequest("nonblacklist", "", "", false, false, false) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("nonblacklist", "", "", true, true, true) == FriendRequestDecision::kPending);
}
