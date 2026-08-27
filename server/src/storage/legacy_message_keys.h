#pragma once

// Shared compatibility map for original Dice!V2 GlobalMsg keys (strXXX).
// Migration and live command compatibility must use the same audited map.

#include <cstring>
#include <map>
#include <string>

namespace dice::legacyv2 {

inline const std::map<std::string, std::string>& msgKeyMap() {
    static const std::map<std::string, std::string> M = {
        {"strAddFriend", "event.friend_welcome"}, {"strAddGroup", "event.group_joined"},
        {"strBotOn", "bot.on"}, {"strBotOff", "bot.off"},
        {"strBotOnAlready", "bot.already_on"}, {"strBotOffAlready", "bot.already_off"},
        {"strHlpMsg", "help.main"}, {"strHlpNotFound", "help.unknown"},
        {"strJrrp", "fun.jrrp"},
        {"strRollCriticalSuccess", "dice.crit"}, {"strRollFumble", "dice.fumble"},
        {"strCriticalSuccess", "dice.level.critical"}, {"strExtremeSuccess", "dice.level.extreme"},
        {"strHardSuccess", "dice.level.hard"}, {"strSuccess", "dice.level.regular"},
        {"strFailure", "dice.level.failure"}, {"strFumble", "dice.level.fumble"},
        {"strRollDice", "dice.roll.result"}, {"strRollDiceReason", "dice.roll.result_reason"},
        {"strDeckEmpty", "deck.empty"}, {"strDeckNotFound", "deck.no_deck"},
        {"strDeckNameEmpty", "deck.usage"}, {"strDeckProSet", "deck.default_set"},
        {"strSanInvalid", "card.sc.invalid_san"},
        {"strNameTooLongErr", "fun.nn.too_long"}, {"strNameNumTooBig", "fun.name.too_many"},
        {"strNameNumCannotBeZero", "fun.name.too_many"},
        {"strSetTooBig", "fun.set.invalid"}, {"strSetCannotBeZero", "fun.set.invalid"},
        {"strWelcomeMsgUpdateNotice", "welcome.set"}, {"strWelcomeMsgClearNotice", "welcome.off"},
        {"strWelcomeMsgClearErr", "welcome.none"}, {"strDismiss", "dismiss.leaving"},
        // New off/end hooks no longer receive the legacy log-name argument.
        {"strLogNew", "log.new"}, {"strLogOn", "log.on"},
        {"strGlobalOff", "gate.global_silent"},
        {"strDisabledJrrpGlobal", "gate.jrrp_global"}, {"strDisabledMeGlobal", "gate.me_global"},
        {"strDismissPrivate", "dismiss.private"}, {"strWelcomePrivate", "welcome.private"},
        {"strBlackGroup", "event.blacklist_group"}, {"strSummonEmpty", "fun.summon_empty"},
        {"strLeaveUnused", "event.leave_unused"}, {"strMEDisabledErr", "me.disabled"},
        {"strNotMaster", "gate.not_master"}, {"strNotAdmin", "gate.not_admin"},
        {"strPermissionDeniedErr", "gate.perm_denied"}, {"strSelfPermissionErr", "gate.self_perm"},
        {"strRuleNotFound", "rule.not_found"}, {"strPropNotFound", "card.attr_missing"},
        {"strGameNew", "game.new"}, {"strGameAreaOpen", "game.area_open"},
        {"strGameAreaClosed", "game.area_closed"}, {"strGameMasterDenied", "game.master_denied"},
        {"strGameMastered", "game.mastered"}, {"strGameMasterList", "game.master_list"},
        {"strGameNotExist", "game.not_exist"}, {"strGameVoidHere", "game.void_here"},
        {"strGameNotMaster", "game.not_master"}, {"strGameItemSet", "game.item_set"},
        {"strGameItemShow", "game.item_show"}, {"strGameItemEmpty", "game.item_empty"},
        {"strGameJoined", "game.joined"}, {"strGamePlayerAlready", "game.player_already"},
        {"strGameExited", "game.exited"}, {"strGameNotJoined", "game.not_joined"},
        {"strGamePlayerEmpty", "game.player_empty"}, {"strGameKicked", "game.kicked"},
        {"strGameKickNotPlayer", "game.kick_not_player"}, {"strGameOver", "game.over"},
        {"strGameRouletteSet", "game.roulette_set"}, {"strGameRouletteHistory", "game.roulette_hist"},
        {"strGameRouletteEmpty", "game.roulette_empty"}, {"strGameRouletteReset", "game.roulette_reset"},
        {"strGameRouletteClear", "game.roulette_clear"}, {"strGameRouletteTooBig", "game.roulette_too_big"},
        {"strObEnter", "ob.joined"}, {"strObEnterAlready", "ob.already"},
        {"strObExit", "ob.exit"}, {"strObExitAlready", "ob.not_in"},
        // strObList intentionally has no mapping because the new slot owns the list too.
        {"strObListEmpty", "ob.empty"}, {"strObListClr", "ob.cleared"},
        {"strObOn", "ob.on"}, {"strObOff", "ob.off"},
        {"strObOnAlready", "ob.on_already"}, {"strObOffAlready", "ob.off_already"},
    };
    return M;
}

// Only normalize placeholders whose destination runtime arguments were audited.
inline std::string normalizeLegacyTemplate(const std::string&, std::string text) {
    const std::pair<const char*, const char*> generic[] = {
        {"{pc}", "{nick}"}, {"{strSelfName}", "{self}"},
        {"{deck_name}", "{name}"}, {"{game.log_name}", "{name}"},
    };
    for (const auto& [from, to] : generic) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return text;
}

inline std::string v2KeyFor(const std::string& ourKey) {
    for (const auto& [orig, our] : msgKeyMap()) if (our == ourKey) return orig;
    return "";
}

}  // namespace dice::legacyv2
