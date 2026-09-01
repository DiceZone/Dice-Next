#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace dice::plugin_command_priority {

// Exact core names stay owned by Dice!Next. A plugin may still own a longer
// word (for example ram) even when a compact core parser recognizes its prefix
// (ra). This list intentionally contains command names, never aliases inferred
// from similarity.
inline bool isReservedCoreCommand(std::string word) {
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::unordered_set<std::string> names = {
        "r", "h", "rh", "rs", "rsh", "rhs", "rdc", "rdx", "rav", "rcv", "rx", "ra", "rah", "rc", "rch",
        "rb", "rp", "ba", "bav", "rpmode", "ss", "cast", "longrest", "ds",
        "game", "coc", "cocs", "coc6", "coc6s", "coc7", "coc7s",
        "cocd", "cocds", "coc6d", "coc6ds", "coc7d", "coc7ds", "dnd", "bot",
        "boton", "botoff", "blackqq", "blackgroup", "whitegroup", "whiteqq", "blackfriend",
        "strselfname", "strselfcall", "st", "sc",
        "ww", "dx", "favor", "jrrp", "mrrp", "zrrp", "ti", "li", "name", "gn", "en", "me",
        "ak", "nnn", "nn", "setcoc", "plugin", "system", "setsn", "setdnd",
        "set", "sleep", "draw", "drawh", "deck", "gacha", "helpdoc", "welcome", "dismiss",
        "ruleset", "notice", "alias", "trust", "admin", "rules", "group", "reply",
        "cloud", "user", "bind", "info", "buff", "send", "help", "text", "link",
        "init", "lang", "rule", "npc", "log", "hiy", "mod", "ai", "ob", "ri",
        "sn", "pc",
        "长休", "死亡豁免", "好感", "群内好感", "强制好感"
    };
    return names.count(word) != 0;
}

}  // namespace dice::plugin_command_priority
