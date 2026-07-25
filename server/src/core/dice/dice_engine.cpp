#include "dice_engine.h"
#include "../../common/logger.h"
#include "../../common/errors.h"

#include <sstream>
#include <algorithm>
#include <random>
#include <chrono>

namespace dice {

// ═══════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════

DiceEngine::DiceEngine(ConfigManager& configMgr)
    : configMgr_(configMgr)
    , expression_([this](int sides) { return this->rollOneDie(sides); }) {

    // Seed the Mersenne Twister
    std::random_device rd;
    // Combine random_device with high-res clock for extra entropy
    auto seed = rd() ^
        static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
    rng_.seed(static_cast<std::mt19937::result_type>(seed));

    // Load initial rules from config
    reloadRules();

    // Register hot-reload callback
    configMgr_.onConfigChanged([this]() {
        DICE_LOG_INFO("DiceEngine: config changed, reloading dice rules");
        reloadRules();
    });

    DICE_LOG_INFO("DiceEngine: initialized (coc critical={}, fumble>={})",
        rules_.cocCriticalRange, rules_.cocFumbleRange);
}

// ═══════════════════════════════════════════════════════════════
// Primary API
// ═══════════════════════════════════════════════════════════════

DiceResult DiceEngine::roll(const std::string& expression) {
    DICE_LOG_DEBUG("DiceEngine: rolling '{}'", expression);

    DiceResult result = expression_.evaluate(expression);

    if (!result.ok()) {
        // DEBUG, not WARN: the caller routinely retries via the OneDice fallback
        // engine (kh/kl, 骰池, bare "d", etc.), so a first-try miss is expected,
        // not an error. The real user-facing failure is reported by the caller.
        DICE_LOG_DEBUG("DiceEngine: roll failed (will try OneDice fallback): {}", result.error);
        return result;
    }

    // For COC d100, apply critical/fumble detection
    if (rules_.cocEnabled && !result.individualResults.empty()) {
        // Detect COC-style rolls: single 100-sided die or single result in D100 range
        bool isD100 = false;
        if (result.individualResults.size() == 1) {
            isD100 = (result.expression.find("d100") != std::string::npos ||
                      result.expression.find("D100") != std::string::npos);
        } else if (result.individualResults.size() == 2) {
            // Two D10 for COC-style D100: tens×10 + ones → [0,99]
            // If both dice are in [0,9] range, treat as D100
            isD100 = (result.individualResults[0] >= 0 && result.individualResults[0] <= 9 &&
                      result.individualResults[1] >= 0 && result.individualResults[1] <= 9);
        }

        // Apply critical/fumble based on the modified total for COC-style rolls.
        // NOTE: the human-readable "[Critical]/[Fumble]" label is intentionally
        // NOT appended here — it is language-specific and gets localized in the
        // command router via I18n (dice.crit / dice.fumble). The engine only
        // reports the boolean flags so the presentation layer stays i18n-aware.
        if (result.modifiedTotal >= 1 && result.modifiedTotal <= 100) {
            result.isCritical = checkCritical(result.modifiedTotal);
            result.isFumble = checkFumble(result.modifiedTotal);
        }
    }

    return result;
}

std::vector<int> DiceEngine::rollXdY(int x, int y) {
    std::vector<int> results;
    results.reserve(static_cast<size_t>(x));
    for (int i = 0; i < x; ++i) {
        results.push_back(rollOneDie(y));
    }
    return results;
}

int DiceEngine::rollOneDie(int sides) {
    if (sides < 1) sides = 1;
    std::uniform_int_distribution<int> dist(1, sides);
    return dist(rng_);
}

// ═══════════════════════════════════════════════════════════════
// System-specific rolls
// ═══════════════════════════════════════════════════════════════

DiceResult DiceEngine::rollCOC(int bonus) {
    // COC D100: tens die (0-9) × 10 + ones die (0-9), range [0, 99]
    // 0 0 → 100, otherwise tens×10 + ones
    if (bonus < 0) bonus = 0;

    // Roll base: tens die + ones die
    int tensDie = rollOneDie(10) - 1;   // 0-9
    int onesDie = rollOneDie(10) - 1;   // 0-9

    // If bonus dice, roll extra tens dice and keep best/worst
    std::vector<int> allTens;
    allTens.push_back(tensDie);
    for (int i = 0; i < bonus; ++i) {
        allTens.push_back(rollOneDie(10) - 1);
    }

    // For bonus dice in COC, keep the best (lowest tens digit) for advantage
    tensDie = *std::min_element(allTens.begin(), allTens.end());

    int result = (tensDie * 10) + onesDie;
    if (result == 0) result = 100;  // 00 → 100

    std::vector<int> individualResults = { result };

    bool crit = checkCritical(result);
    bool fumble = checkFumble(result);

    std::ostringstream detail, formatted;
    detail << "COC D100 = " << result;
    formatted << "COC D100 = " << result;

    if (crit) { detail << " 【暴击】"; formatted << " 【暴击】"; }
    if (fumble) { detail << " 【大失败】"; formatted << " 【大失败】"; }

    return DiceResult::success("coc", individualResults, result, crit, fumble,
                                detail.str(), formatted.str());
}

DiceResult DiceEngine::rollDND(bool advantage) {
    // D20 roll
    int roll1 = rollOneDie(20);
    int roll2 = rollOneDie(20);

    int finalRoll = 0;
    std::vector<int> individualResults;
    std::string mode;

    if (advantage) {
        finalRoll = std::max(roll1, roll2);
        individualResults = { roll1, roll2 };
        mode = "优势";
    } else {
        finalRoll = roll1;
        individualResults = { roll1 };
        mode = "标准";
    }

    std::ostringstream detail, formatted;
    detail << "DND D20 (" << mode << ") = " << finalRoll;
    formatted << "DND D20 (" << mode << ") = " << finalRoll;
    if (advantage) {
        detail << " (取优: " << roll1 << ", " << roll2 << ")";
        formatted << " (取优: " << roll1 << ", " << roll2 << ")";
    }

    return DiceResult::success("dnd", individualResults, finalRoll, false, false,
                                detail.str(), formatted.str());
}

DiceResult DiceEngine::rollFate() {
    // 4dF: each die is {-1, 0, +1}
    std::uniform_int_distribution<int> fateDist(0, 2); // 0→-1, 1→0, 2→+1
    std::vector<int> results;
    int total = 0;

    for (int i = 0; i < 4; ++i) {
        int val = fateDist(rng_) - 1; // -1, 0, or +1
        results.push_back(val);
        total += val;
    }

    std::ostringstream detail, formatted;
    detail << "4dF = [";
    formatted << "4dF = [";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) { detail << ", "; formatted << ", "; }
        char sign = (results[i] > 0) ? '+' : (results[i] < 0) ? '-' : ' ';
        detail << sign << std::abs(results[i]);
        formatted << sign << std::abs(results[i]);
    }
    detail << "] = " << total;
    formatted << "] = " << total;

    return DiceResult::success("4dF", results, total, false, false,
                                detail.str(), formatted.str());
}

DiceResult DiceEngine::rollL5R(int roll, int keep) {
    if (roll < 1) roll = 1;
    if (keep < 1) keep = 1;
    if (keep > roll) keep = roll;

    std::vector<int> allRolls = rollXdY(roll, 10);

    // Sort descending and keep the best
    std::vector<int> sorted = allRolls;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());

    std::vector<int> kept(keep);
    int total = 0;
    for (int i = 0; i < keep; ++i) {
        kept[i] = sorted[i];
        total += sorted[i];
    }

    std::ostringstream detail, formatted;
    detail << roll << "d10k" << keep << " = [";
    formatted << roll << "d10k" << keep << " = [";
    for (size_t i = 0; i < allRolls.size(); ++i) {
        if (i > 0) { detail << ", "; formatted << ", "; }
        detail << allRolls[i];
        formatted << allRolls[i];
    }
    detail << "] = " << total;
    formatted << "] = " << total;

    // Only include kept dice in individualResults for L5R
    return DiceResult::success(roll + "d10k" + std::to_string(keep),
                                allRolls, total, false, false,
                                detail.str(), formatted.str());
}

// ═══════════════════════════════════════════════════════════════
// Rule management
// ═══════════════════════════════════════════════════════════════

void DiceEngine::setRules(const DiceRules& rules) {
    rules_ = rules;
    DICE_LOG_INFO("DiceEngine: rules updated (coc critical={}, fumble>={})",
        rules_.cocCriticalRange, rules_.cocFumbleRange);
}

void DiceEngine::reloadRules() {
    try {
        json rulesJson = configMgr_.getAll();
        // Navigate to dice → rules
        if (rulesJson.contains("dice") && rulesJson["dice"].contains("rules")) {
            rules_.fromJSON(rulesJson["dice"]["rules"]);
        } else if (rulesJson.contains("dice")) {
            // Top-level dice section might contain rules inline
            rules_.fromJSON(rulesJson["dice"]);
        }

        DICE_LOG_INFO("DiceEngine: rules reloaded from config");
    } catch (const std::exception& e) {
        DICE_LOG_WARN("DiceEngine: failed to reload rules: {}, using previous", e.what());
    }
}

// ═══════════════════════════════════════════════════════════════
// Critical / Fumble detection
// ═══════════════════════════════════════════════════════════════

bool DiceEngine::checkCritical(int result) const {
    if (!rules_.cocEnabled) return false;
    return result <= rules_.cocCriticalRange;
}

bool DiceEngine::checkFumble(int result) const {
    if (!rules_.cocEnabled) return false;
    return result >= rules_.cocFumbleRange;
}

}  // namespace dice
