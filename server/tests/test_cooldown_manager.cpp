// ─── C#29: CooldownManager Unit Tests ─────────────────────────
// Tests the in-memory cooldown tracking (no DB required).
// Covers: trigger, isCooling, remainingMs, clear, clearAll,
//         per-user/per-group/global key isolation.

#include "test_framework.h"
#include "../src/core/causal/cooldown_manager.h"

using namespace dice;

TEST(Cooldown, NotCoolingBeforeTrigger) {
    CooldownManager mgr;
    ASSERT_FALSE(mgr.isCooling("1:per-user:12345", 5000));
}

TEST(Cooldown, CoolingAfterTrigger) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    ASSERT_TRUE(mgr.isCooling("1:per-user:12345", 5000));
}

TEST(Cooldown, ZeroCooldownNeverCools) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    ASSERT_FALSE(mgr.isCooling("1:per-user:12345", 0));
}

TEST(Cooldown, NegativeCooldownNeverCools) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    ASSERT_FALSE(mgr.isCooling("1:per-user:12345", -100));
}

TEST(Cooldown, DifferentKeysAreIndependent) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:111");
    ASSERT_TRUE(mgr.isCooling("1:per-user:111", 5000));
    ASSERT_FALSE(mgr.isCooling("1:per-user:222", 5000));
}

TEST(Cooldown, PerUserAndPerGroupIsolated) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    ASSERT_TRUE(mgr.isCooling("1:per-user:12345", 5000));
    ASSERT_FALSE(mgr.isCooling("1:per-group:67890", 5000));
}

TEST(Cooldown, GlobalKeyIsolated) {
    CooldownManager mgr;
    mgr.trigger("1:global:");
    ASSERT_TRUE(mgr.isCooling("1:global:", 5000));
    ASSERT_FALSE(mgr.isCooling("1:per-user:12345", 5000));
}

TEST(Cooldown, ClearRemovesCooldown) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    ASSERT_TRUE(mgr.isCooling("1:per-user:12345", 5000));
    mgr.clear("1:per-user:12345");
    ASSERT_FALSE(mgr.isCooling("1:per-user:12345", 5000));
}

TEST(Cooldown, ClearAllRemovesEverything) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:111");
    mgr.trigger("2:per-group:222");
    mgr.trigger("3:global:");
    ASSERT_TRUE(mgr.isCooling("1:per-user:111", 5000));
    ASSERT_TRUE(mgr.isCooling("2:per-group:222", 5000));
    ASSERT_TRUE(mgr.isCooling("3:global:", 5000));
    mgr.clearAll();
    ASSERT_FALSE(mgr.isCooling("1:per-user:111", 5000));
    ASSERT_FALSE(mgr.isCooling("2:per-group:222", 5000));
    ASSERT_FALSE(mgr.isCooling("3:global:", 5000));
}

TEST(Cooldown, RemainingMsIsZeroBeforeTrigger) {
    CooldownManager mgr;
    ASSERT_EQ(mgr.remainingMs("1:per-user:12345", 5000), 0);
}

TEST(Cooldown, RemainingMsIsPositiveAfterTrigger) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    auto remaining = mgr.remainingMs("1:per-user:12345", 5000);
    EXPECT_TRUE(remaining > 0 && remaining <= 5000);
}

TEST(Cooldown, RemainingMsIsZeroForZeroCooldown) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    ASSERT_EQ(mgr.remainingMs("1:per-user:12345", 0), 0);
}

TEST(Cooldown, ReTriggerResetsTimer) {
    CooldownManager mgr;
    mgr.trigger("1:per-user:12345");
    // Simulate time passing (we can't actually sleep, but we can verify
    // that re-triggering doesn't cause issues)
    mgr.trigger("1:per-user:12345");
    ASSERT_TRUE(mgr.isCooling("1:per-user:12345", 5000));
}
