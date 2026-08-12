#include "test_framework.h"
#include "../src/service/heart_debounce.h"

TEST(HeartDebounce, RequiresContinuousOfflineWindow) {
    long long observedAt = 0;
    ASSERT_FALSE(dice::heart::offlineTransitionReady("online", false, 100, observedAt));
    ASSERT_EQ(observedAt, 100LL);
    ASSERT_FALSE(dice::heart::offlineTransitionReady("online", false, 159, observedAt));
    ASSERT_TRUE(dice::heart::offlineTransitionReady("online", false, 160, observedAt));
}

TEST(HeartDebounce, ReconnectionCancelsPendingOffline) {
    long long observedAt = 100;
    ASSERT_FALSE(dice::heart::offlineTransitionReady("online", true, 130, observedAt));
    ASSERT_EQ(observedAt, 0LL);
    ASSERT_FALSE(dice::heart::offlineTransitionReady("online", false, 140, observedAt));
    ASSERT_EQ(observedAt, 140LL);
}

TEST(HeartDebounce, NeverReportsInitialOfflineState) {
    long long observedAt = 0;
    ASSERT_FALSE(dice::heart::offlineTransitionReady("unknown", false, 100, observedAt));
    ASSERT_EQ(observedAt, 0LL);
}
