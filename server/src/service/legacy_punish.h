#pragma once
// Original Dice kick / mute punishment and cleanup-leave copy.
// Callers must pass events that have already gone through reconcileOwnLeaveEcho:
// an unattributed kick after our own leave request is a confirmation, not a kick.

#include "notice_event_text.h"

#include <cstdint>
#include <limits>
#include <string>

namespace dice::legacy_punish {

struct Flags {
    bool listen = false;
    bool banInviter = false;
};

struct Parties {
    bool groupExempt = false;
    bool operatorExempt = false;
    bool operatorIsDiceBot = false;
    bool inviterExempt = true;
};

struct KickDecision {
    bool banOperator = false;
    bool banInviter = false;
    bool clearCleanupExempt = false;
};

struct MuteDecision {
    bool banOperator = false;
    bool banInviter = false;
    bool leaveGroup = false;
    bool clearCleanupExempt = false;
};

enum class KickSubject { None, Self, OtherDice };

enum class CleanupLeave { Idle, BlackMember, BlackAdmin, Oversize };

inline KickSubject kickSubject(const BotEvent& observed, bool kickedUserIsDiceBot) {
    if (observed.type != EventType::kGroupDecrease) return KickSubject::None;
    if (!notice::reportedAsKick(observed.subType)) return KickSubject::None;
    if (observed.userId.empty()) return KickSubject::None;
    if (!observed.selfId.empty() && observed.userId == observed.selfId) return KickSubject::Self;
    if (kickedUserIsDiceBot) return KickSubject::OtherDice;
    return KickSubject::None;
}

inline int64_t muteDurationSeconds(const BotEvent& event) {
    if (!event.extra.is_object() || !event.extra.contains("duration")) return 0;
    const auto& value = event.extra["duration"];
    if (value.is_number_integer()) {
        const auto n = value.get<int64_t>();
        return n > 0 ? n : 0;
    }
    if (value.is_number_unsigned()) {
        const auto n = value.get<uint64_t>();
        if (n == 0 || n > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return 0;
        return static_cast<int64_t>(n);
    }
    if (value.is_string()) {
        try {
            const auto n = std::stoll(value.get<std::string>());
            return n > 0 ? n : 0;
        } catch (...) { return 0; }
    }
    return 0;
}

inline bool wholeGroupMute(const BotEvent& event) {
    return event.userId == "0" || event.subType == "whole_ban" || event.subType == "whole_lift_ban";
}

inline bool muteLifted(const BotEvent& event) {
    return event.subType == "lift_ban" || event.subType == "whole_lift_ban";
}

inline bool shouldConsiderMute(const BotEvent& event, bool targetIsSelf) {
    if (!targetIsSelf || event.type != EventType::kGroupBan) return false;
    if (wholeGroupMute(event) || muteLifted(event)) return false;
    return muteDurationSeconds(event) > 0;
}

inline KickDecision decideKick(const BotEvent& observed, KickSubject subject,
                               const Flags& flags, const Parties& parties) {
    KickDecision out;
    if (!flags.listen || subject == KickSubject::None) return out;
    if (parties.groupExempt) return out;
    if (!notice::namesAnotherOperator(observed)) return out;
    if (parties.operatorExempt) return out;
    out.clearCleanupExempt = true;
    out.banOperator = !parties.operatorIsDiceBot;
    out.banInviter = flags.banInviter && subject == KickSubject::Self && !parties.inviterExempt;
    return out;
}

inline MuteDecision decideMute(const BotEvent& event, bool targetIsSelf,
                               const Flags& flags, const Parties& parties) {
    MuteDecision out;
    if (!flags.listen || !shouldConsiderMute(event, targetIsSelf)) return out;
    if (parties.groupExempt) return out;
    if (!notice::namesAnotherOperator(event)) return out;
    if (parties.operatorExempt) return out;
    out.leaveGroup = true;
    out.clearCleanupExempt = true;
    out.banOperator = !parties.operatorIsDiceBot;
    out.banInviter = flags.banInviter && !parties.inviterExempt;
    return out;
}

inline const char* cleanupLeaveKey(CleanupLeave kind) {
    switch (kind) {
        case CleanupLeave::Idle: return "event.leave_unused";
        case CleanupLeave::BlackMember: return "event.blacklist_member_leave";
        case CleanupLeave::BlackAdmin: return "event.blacklist_admin_leave";
        case CleanupLeave::Oversize: return "event.leave_oversize";
    }
    return "event.leave_unused";
}

}  // namespace dice::legacy_punish
