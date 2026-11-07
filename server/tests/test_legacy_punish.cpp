#include "test_framework.h"
#include "service/legacy_punish.h"
#include "common/auto_cleanup.h"
#include "service/notice_event_text.h"
#include <string>

namespace {
dice::BotEvent kickEvent() {
    dice::BotEvent event;
    event.type = dice::EventType::kGroupDecrease;
    event.subType = "kick_me";
    event.groupId = "90001";
    event.selfId = "10001";
    event.userId = "10001";
    event.operatorId = "20002";
    event.platform = "onebot_v11";
    event.adapterId = "onebot:10001";
    return event;
}
dice::BotEvent muteEvent() {
    dice::BotEvent event;
    event.type = dice::EventType::kGroupBan;
    event.subType = "ban";
    event.groupId = "90001";
    event.selfId = "10001";
    event.userId = "10001";
    event.operatorId = "20002";
    event.extra = {{"duration", 60}};
    return event;
}
dice::legacy_punish::Flags on() {
    return {true, false};
}
dice::legacy_punish::Parties ordinary() {
    return {};
}
}

TEST(LegacyPunish, NamedKickerIsBannedWhenListening) {
    using namespace dice::legacy_punish;
    const auto event = kickEvent();
    const auto decision = decideKick(event, KickSubject::Self, on(), ordinary());
    ASSERT_TRUE(decision.banOperator);
    ASSERT_TRUE(decision.clearCleanupExempt);
    ASSERT_FALSE(decision.banInviter);
}

TEST(LegacyPunish, LeaveEchoAfterOwnRequestIsNotAKick) {
    using namespace dice::legacy_punish;
    auto event = kickEvent();
    event.operatorId.clear();
    dice::notice::LeaveIntent intent;
    intent.operation = "dismiss";
    intent.cause = "指令 .dismiss";
    const auto id = dice::notice::LeaveIntents::instance().remember(
        event.adapterId, event.platform, event.groupId, intent);
    struct Guard { uint64_t id; ~Guard() { dice::notice::LeaveIntents::instance().cancel(id); } } guard{id};
    dice::notice::reconcileOwnLeaveEcho(event);
    ASSERT_EQ(event.subType, std::string("leave"));
    ASSERT_TRUE(kickSubject(event, false) == KickSubject::None);
    const auto decision = decideKick(event, kickSubject(event, false), on(), ordinary());
    ASSERT_FALSE(decision.banOperator);
    ASSERT_FALSE(decision.clearCleanupExempt);
}

TEST(LegacyPunish, EmptyZeroOrSelfOperatorNeverBanned) {
    using namespace dice::legacy_punish;
    for (const auto* operatorId : {"", "0", "10001"}) {
        auto event = kickEvent();
        event.operatorId = operatorId;
        const auto decision = decideKick(event, KickSubject::Self, on(), ordinary());
        ASSERT_FALSE(decision.banOperator);
        ASSERT_FALSE(decision.banInviter);
    }
}

TEST(LegacyPunish, TrustWhitelistExemptGroupAndDisabledSwitchSkipBan) {
    using namespace dice::legacy_punish;
    const auto event = kickEvent();
    Parties exemptOperator = ordinary();
    exemptOperator.operatorExempt = true;
    ASSERT_FALSE(decideKick(event, KickSubject::Self, on(), exemptOperator).banOperator);
    Parties exemptGroup = ordinary();
    exemptGroup.groupExempt = true;
    ASSERT_FALSE(decideKick(event, KickSubject::Self, on(), exemptGroup).banOperator);
    ASSERT_FALSE(decideKick(event, KickSubject::Self, {false, true}, ordinary()).banOperator);
}

TEST(LegacyPunish, InviterChainOnlyForSelfKickWhenEnabled) {
    using namespace dice::legacy_punish;
    Flags flags{true, true};
    Parties parties = ordinary();
    parties.inviterExempt = false;
    const auto self = decideKick(kickEvent(), KickSubject::Self, flags, parties);
    ASSERT_TRUE(self.banOperator);
    ASSERT_TRUE(self.banInviter);
    const auto witness = decideKick(kickEvent(), KickSubject::OtherDice, flags, parties);
    ASSERT_TRUE(witness.banOperator);
    ASSERT_FALSE(witness.banInviter);
    parties.inviterExempt = true;
    ASSERT_FALSE(decideKick(kickEvent(), KickSubject::Self, flags, parties).banInviter);
}

TEST(LegacyPunish, OtherDiceKickCanBanOperator) {
    using namespace dice::legacy_punish;
    auto event = kickEvent();
    event.userId = "40004";
    ASSERT_TRUE(kickSubject(event, true) == KickSubject::OtherDice);
    const auto decision = decideKick(event, KickSubject::OtherDice, on(), ordinary());
    ASSERT_TRUE(decision.banOperator);
    ASSERT_TRUE(decision.clearCleanupExempt);
}

TEST(LegacyPunish, MuteSelfLeavesAndBansNamedOperator) {
    using namespace dice::legacy_punish;
    const auto event = muteEvent();
    const auto decision = decideMute(event, true, on(), ordinary());
    ASSERT_TRUE(decision.banOperator);
    ASSERT_TRUE(decision.leaveGroup);
    ASSERT_TRUE(decision.clearCleanupExempt);
}

TEST(LegacyPunish, MuteLiftWholeGroupAndForeignTargetAreIgnored) {
    using namespace dice::legacy_punish;
    auto lift = muteEvent();
    lift.subType = "lift_ban";
    ASSERT_FALSE(decideMute(lift, true, on(), ordinary()).leaveGroup);
    auto whole = muteEvent();
    whole.subType = "whole_ban";
    whole.userId = "0";
    ASSERT_FALSE(decideMute(whole, true, on(), ordinary()).leaveGroup);
    ASSERT_FALSE(decideMute(muteEvent(), false, on(), ordinary()).leaveGroup);
    auto zero = muteEvent();
    zero.extra = {{"duration", 0}};
    ASSERT_FALSE(decideMute(zero, true, on(), ordinary()).banOperator);
}

TEST(LegacyPunish, MuteDoesNotBanOtherDiceOperator) {
    using namespace dice::legacy_punish;
    Parties parties = ordinary();
    parties.operatorIsDiceBot = true;
    const auto decision = decideMute(muteEvent(), true, {true, true}, parties);
    ASSERT_FALSE(decision.banOperator);
    ASSERT_TRUE(decision.leaveGroup);
}

TEST(LegacyPunish, CleanupCopyKeysMatchOriginalPaths) {
    using namespace dice::legacy_punish;
    ASSERT_EQ(std::string(cleanupLeaveKey(CleanupLeave::Idle)), std::string("event.leave_unused"));
    ASSERT_EQ(std::string(cleanupLeaveKey(CleanupLeave::BlackMember)), std::string("event.blacklist_member_leave"));
    ASSERT_EQ(std::string(cleanupLeaveKey(CleanupLeave::BlackAdmin)), std::string("event.blacklist_admin_leave"));
    ASSERT_EQ(std::string(cleanupLeaveKey(CleanupLeave::Oversize)), std::string("event.leave_oversize"));
}

TEST(AutoCleanup, IdleDaysUsesWholeUtcDays) {
    constexpr std::time_t now = 1762300800; // 2025-11-05 00:00:00 UTC
    ASSERT_EQ(dice::auto_cleanup::idleDays("2025-11-04T00:00:00Z", now), 1);
    ASSERT_EQ(dice::auto_cleanup::idleDays("2025-10-06T00:00:00Z", now), 30);
    ASSERT_EQ(dice::auto_cleanup::idleDays("", now), 0);
    ASSERT_EQ(dice::auto_cleanup::idleDays("2025-11-04", now), 0);
}
