#pragma once

#include <string>
#include <vector>

namespace dice::check_reply {

// Optional overlays: empty entries inherit and rule engines still decide grades.
// Shared by the router and editable-text API to keep keys/variables in sync.
struct Family {
    std::string name;
    std::vector<std::string> grades;
    std::string legacyKey;
    std::vector<std::string> extraVars;
};

inline const std::vector<Family>& families() {
    static const std::vector<std::string> coc{
        "critical", "extreme", "hard", "regular", "failure", "fumble"};
    static const std::vector<Family> entries{
        {"standard", {"critical", "extreme", "hard", "regular", "failure", "fumble", "special", "tie"}, "dice.check.result", {}},
        {"bonus", coc, "dice.check.result", {}},
        {"penalty", coc, "dice.check.result", {}},
        {"brp", {"critical", "special", "regular", "failure", "fumble"}, "dice.brp.result", {}},
        {"sanity", coc, "card.sc.result", {"san", "grade", "loss", "final"}},
        {"growth", {"regular", "failure"}, "card.en.success", {"change", "final"}},
        {"psychology", coc, "dice.rx.private", {"target", "gid", "receipt"}},
        {"opposed", {"regular", "failure", "tie"}, "dice.rav.result", {"la", "lb", "ra", "rb", "va", "vb", "lva", "lvb"}},
        {"resist", {"regular", "failure"}, "dice.brp.resist", {"la", "lb", "va", "vb", "target"}},
        {"dnd_check", {"regular", "failure"}, "dnd.rdc.result", {"detail", "total", "mod", "threshold", "turn"}},
        {"death_save", {"critical", "regular", "failure", "fumble"}, "dnd.ds.result", {"total", "s", "f"}},
    };
    return entries;
}

inline std::string key(const std::string& family, const std::string& grade) {
    return "dice.outcome." + family + "." + grade;
}

inline const Family* familyForKey(const std::string& textKey) {
    for (const auto& family : families())
        for (const auto& grade : family.grades)
            if (textKey == key(family.name, grade)) return &family;
    return nullptr;
}

inline std::string gradeForKey(const std::string& textKey) {
    return textKey.substr(textKey.find_last_of('.') + 1);
}

// The original template is selected by the caller (reason/loss/growth variants).
inline std::vector<std::string> candidates(const std::string& family, const std::string& grade) {
    std::vector<std::string> keys{key(family, grade)};
    if (family != "standard") keys.push_back(key("standard", grade));
    return keys;
}

inline std::vector<std::string> fallbackKeys(const std::string& textKey) {
    const auto* family = familyForKey(textKey);
    if (!family) return {};
    auto keys = candidates(family->name, gradeForKey(textKey));
    keys.erase(keys.begin());
    // Representative defaults for editing; runtime uses the exact old variant.
    keys.push_back(family->name == "growth" && gradeForKey(textKey) == "failure"
        ? "card.en.fail" : family->legacyKey);
    return keys;
}

inline std::vector<std::string> variables(const std::string& textKey) {
    const auto* family = familyForKey(textKey);
    if (!family) return {};
    std::vector<std::string> vars{"nick", "attr", "reason", "roll", "rate", "level", "res", "result", "outcome"};
    vars.insert(vars.end(), family->extraVars.begin(), family->extraVars.end());
    return vars;
}

} // namespace dice::check_reply
