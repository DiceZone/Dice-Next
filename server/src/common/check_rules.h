#pragma once

namespace dice::check_rules {
enum class SuccessLevel { kFumble, kFailure, kRegular, kHard, kExtreme, kCritical };

// Used by both real checks and the isolated reply preview. Keep rule decisions
// here, rather than maintaining a second, subtly different frontend rule engine.
inline SuccessLevel coc(int res, int rate, int rule = 0) {
    using S = SuccessLevel;
    switch (rule) {
    case 1:
        if (res == 100)
            return S::kFumble;
        if (res == 1 || (res <= 5 && rate >= 50))
            return S::kCritical;
        break;
    case 2:
        if (res == 100)
            return S::kFumble;
        if (res <= 5 && res <= rate)
            return S::kCritical;
        if (res <= rate / 5)
            return S::kExtreme;
        if (res <= rate / 2)
            return S::kHard;
        if (res <= rate)
            return S::kRegular;
        return res < 96 ? S::kFailure : S::kFumble;
    case 3:
        if (res >= 96)
            return S::kFumble;
        if (res <= 5)
            return S::kCritical;
        if (res <= rate / 5)
            return S::kExtreme;
        if (res <= rate / 2)
            return S::kHard;
        return res <= rate ? S::kRegular : S::kFailure;
    case 4:
        if (res == 100)
            return S::kFumble;
        if (res <= 5 && res <= rate / 10)
            return S::kCritical;
        if (res <= rate / 5)
            return S::kExtreme;
        if (res <= rate / 2)
            return S::kHard;
        if (res <= rate)
            return S::kRegular;
        return rate >= 50 || res < 96 + rate / 10 ? S::kFailure : S::kFumble;
    case 5:
        if (res >= 99)
            return S::kFumble;
        if (res <= 2 && res < rate / 10)
            return S::kCritical;
        break;
    case 6:
        if (res > rate)
            return res == 100 || res % 11 == 0 ? S::kFumble : S::kFailure;
        return res == 1 || res % 11 == 0 ? S::kCritical : S::kRegular;
    case 7:
        if (res >= 100)
            return S::kFumble;
        if (res >= 96)
            return (90 - rate) / 20 + res >= 100 ? S::kFumble : S::kFailure;
        if (res == 1 || res <= rate / 5)
            return S::kExtreme;
        return res <= rate ? S::kRegular : S::kFailure;
    default:
        if (res == 100)
            return S::kFumble;
        if (res == 1)
            return S::kCritical;
        break;
    }
    if (res <= rate / 5)
        return S::kExtreme;
    if (res <= rate / 2)
        return S::kHard;
    if (res <= rate)
        return S::kRegular;
    return rate >= 50 || res < 96 ? S::kFailure : S::kFumble;
}

inline SuccessLevel brp(int roll, int skill) {
    using S = SuccessLevel;
    if (skill < 1)
        skill = 1;
    if (roll >= 100)
        return S::kFumble;
    if (roll > skill)
        return roll == 99 ? S::kFumble : S::kFailure;
    const int critical = skill / 20 < 1 ? 1 : skill / 20;
    if (roll <= critical)
        return S::kCritical;
    if (roll <= skill / 5)
        return S::kExtreme;
    return S::kRegular;
}

inline const char *grade(SuccessLevel level, bool brp = false) {
    switch (level) {
    case SuccessLevel::kCritical:
        return "critical";
    case SuccessLevel::kExtreme:
        return brp ? "special" : "extreme";
    case SuccessLevel::kHard:
        return "hard";
    case SuccessLevel::kRegular:
        return "regular";
    case SuccessLevel::kFailure:
        return "failure";
    case SuccessLevel::kFumble:
        return "fumble";
    }
    return "failure";
}
} // namespace dice::check_rules
