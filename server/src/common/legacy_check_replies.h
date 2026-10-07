#pragma once
#include <string>
#include <vector>

namespace dice::legacy_check_replies {
inline bool isKey(const std::string& key) { return key.rfind("dice.compat.", 0) == 0; }
inline std::string single(const std::string& grade) { return "dice.compat.check.single." + grade; }
inline std::vector<std::string> variables(const std::string& key) {
    if (!isKey(key)) return {};
    std::vector<std::string> vars{"nick", "attr", "reason", "res", "roll", "rate", "level", "result"};
    if (key.rfind("dice.compat.sanity.", 0) == 0)
        vars.insert(vars.end(), {"san", "rank", "loss", "change", "final"});
    if (key.rfind("dice.compat.growth.", 0) == 0)
        vars.insert(vars.end(), {"change", "final"});
    return vars;
}
inline std::vector<std::string> keys() {
    std::vector<std::string> result{"dice.compat.check.prefix", "dice.compat.check.prefix_reason", "dice.compat.sanity.result",
        "dice.compat.growth.base", "dice.compat.growth.unchanged", "dice.compat.growth.failure", "dice.compat.growth.success"};
    for (const auto* grade : {"critical", "extreme", "hard", "regular", "failure", "fumble"}) result.push_back(single(grade));
    return result;
}
} // namespace dice::legacy_check_replies
