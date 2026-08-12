#include "test_framework.h"
#include "src/core/friend_approval_policy.h"

using dice::FriendRequestDecision;
using dice::evaluateFriendRequest;

TEST(FriendApproval, HandlesAllPolicies) {
    ASSERT_TRUE(evaluateFriendRequest("manual", "", "", true) == FriendRequestDecision::kPending);
    ASSERT_TRUE(evaluateFriendRequest("all", "", "", false) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("reject", "", "", true) == FriendRequestDecision::kReject);

    ASSERT_TRUE(evaluateFriendRequest("keyword", "hello dice", "dice", false) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("keyword", "hello", "dice", true) == FriendRequestDecision::kPending);
    ASSERT_TRUE(evaluateFriendRequest("keyword", "hello", "", true) == FriendRequestDecision::kPending);

    ASSERT_TRUE(evaluateFriendRequest("group_used", "", "", true) == FriendRequestDecision::kApprove);
    ASSERT_TRUE(evaluateFriendRequest("group_used", "", "", false) == FriendRequestDecision::kPending);
}
