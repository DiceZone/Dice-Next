#include "test_framework.h"
#include "common/auto_cleanup.h"
#include "common/check_rules.h"

TEST(AutoCleanup, InactivityRequiresValidKnownUtcTime) {
    constexpr std::time_t now = 1762300800; // 2025-11-05 00:00:00 UTC
    ASSERT_TRUE(dice::auto_cleanup::inactive("2025-11-04T00:00:00.123Z", 1, now));
    ASSERT_FALSE(dice::auto_cleanup::inactive("2025-11-04T00:00:01Z", 1, now));
    ASSERT_FALSE(dice::auto_cleanup::inactive("2025-11-04T00:00:00Z", 0, now));
    for (const auto* value : {"", "2025-11-04", "2025-02-31T00:00:00Z", "2025-11-04T24:00:00Z",
            "2025-11-04T00:00:00+08:00", "2025-11-04T00:00:00.Z", "2025-11-04T00:00:00Zjunk",
            "2025-13-01T00:00:00Z", "2099-01-01T00:00:00Z"})
        ASSERT_FALSE(dice::auto_cleanup::inactive(value, 1, now));
}

TEST(AutoCleanup, SettingsAreValidatedBeforeAnyMutation) {
    using J = nlohmann::json;
    const J original{{"friend_clean_days", 0}, {"masters", J::array({"owner"})}};
    const auto next = dice::auto_cleanup::patched(original, J{{"days", 30}, {"groupDays", 90},
        {"userRecordDays", 180}, {"groupRecordDays", 365}, {"groupLimit", 0}, {"maxGroupSize", 3000}});
    ASSERT_EQ(next["friend_clean_days"].get<int>(), 30);
    ASSERT_EQ(next["group_clean_days"].get<int>(), 90);
    ASSERT_EQ(next["user_record_days"].get<int>(), 180);
    ASSERT_EQ(next["group_record_days"].get<int>(), 365);
    ASSERT_EQ(next["group_clear_limit"].get<int>(), 0);
    ASSERT_TRUE(next["masters"] == original["masters"]);
    for (const auto& invalid : {J{{"days", 90}, {"groupDays", -1}}, J{{"days", 1.5}}, J{{"days", "2"}},
            J{{"userRecordDays", 3651}}, J{{"groupLimit", true}}, J{{"groupLimit", 10001}},
            J{{"groupRecordDays", 100000000000ULL}}, J::array()}) {
        bool rejected = false;
        try { dice::auto_cleanup::patched(original, invalid); } catch (const std::invalid_argument&) { rejected = true; }
        ASSERT_TRUE(rejected);
        ASSERT_EQ(original["friend_clean_days"].get<int>(), 0);
    }
}

TEST(CheckRules, DocumentedHouseRuleBoundaries) {
    using S = dice::check_rules::SuccessLevel;
    struct Example { int rule; int skill; int roll; S expected; };
    for (const auto& e : {Example{0,49,96,S::kFumble}, {0,50,96,S::kFailure}, {0,50,1,S::kCritical},
        {1,49,5,S::kExtreme}, {1,50,5,S::kCritical}, {2,3,4,S::kFailure}, {2,99,99,S::kRegular},
        {2,100,100,S::kFumble}, {3,99,96,S::kFumble}, {4,10,1,S::kCritical}, {4,10,2,S::kExtreme},
        {4,30,98,S::kFailure}, {4,30,99,S::kFumble}, {5,20,2,S::kExtreme}, {5,30,2,S::kCritical},
        {5,49,96,S::kFumble}, {5,50,98,S::kFailure}, {5,50,99,S::kFumble}, {6,50,44,S::kCritical},
        {6,50,55,S::kFumble}, {6,100,100,S::kRegular}, {6,50,10,S::kRegular}, {7,50,1,S::kExtreme},
        {7,50,25,S::kRegular}, {7,50,97,S::kFailure}, {7,50,98,S::kFumble}, {7,100,96,S::kFailure}})
        ASSERT_TRUE(dice::check_rules::coc(e.roll,e.skill,e.rule) == e.expected);
}
