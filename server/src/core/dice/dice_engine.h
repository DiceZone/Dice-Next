#pragma once

#include "dice_result.h"
#include "dice_rules.h"
#include "dice_expression.h"
#include "../../config/config_manager.h"

#include <random>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace dice {

/**
 * @brief Core dice-rolling engine.
 *
 * Manages:
 *   - Random number generation (Mersenne Twister via std::mt19937)
 *   - Dice expression parsing and evaluation (via DiceExpression)
 *   - RPG system-specific rolling rules (COC, DnD, Fate, L5R)
 *   - Critical/fumble detection based on current DiceRules
 *   - Hot-reload: registers a ConfigManager callback to refresh
 *     DiceRules when the "dice/rules" config subtree changes.
 *
 * Thread-safety: the `roll*` methods are NOT internally
 * synchronized; it is the caller's responsibility to ensure
 * that only one thread calls the engine at a time, or to wrap
 * the engine in a mutex.
 */
class DiceEngine {
public:
    /**
     * @brief Construct the engine and register a hot-reload
     *        callback on the given ConfigManager.
     *
     * @param configMgr  The application ConfigManager.
     */
    explicit DiceEngine(ConfigManager& configMgr);

    // ─── Primary API ──────────────────────────────────────────

    /**
     * @brief Parse and roll a dice expression.
     *
     * Supports: XdY, XdY+N, XdY-N, (expr)dY, N, etc.
     *
     * @param expression  Raw expression string (e.g., "2d6+3").
     * @return DiceResult with full roll details.
     */
    DiceResult roll(const std::string& expression);

    /**
     * @brief Roll X dice with Y sides each (low-level).
     *
     * @param x  Number of dice.
     * @param y  Number of sides per die.
     * @return Vector of individual results, each in [1, y].
     */
    std::vector<int> rollXdY(int x, int y);

    // ─── System-specific rolls ────────────────────────────────

    /**
     * @brief Roll a Call of Cthulhu (D100) check.
     *
     * The result is a 5-95 range result from two D10s,
     * plus critical/fumble flags based on the configured
     * cocCriticalRange and cocFumbleRange.
     *
     * @param bonus  Optional bonus die (roll extra tens die, keep worst/best).
     * @return DiceResult
     */
    DiceResult rollCOC(int bonus = 0);

    /**
     * @brief Roll a D&D advantage/disadvantage check (D20).
     * @param advantage  If true, roll with advantage (best of 2d20).
     *                    If false, use standard dndAdvantage setting.
     * @return DiceResult
     */
    DiceResult rollDND(bool advantage);

    /**
     * @brief Roll Fate/Fudge dice (4dF → [-4, +4]).
     * @return DiceResult
     */
    DiceResult rollFate();

    /**
     * @brief Roll Legend of the Five Rings (L5R) dice pool.
     *
     * Roll X k Y: roll X D10s, keep the best Y.
     *
     * @param roll  Number of dice to roll.
     * @param keep  Number of dice to keep.
     * @return DiceResult
     */
    DiceResult rollL5R(int roll, int keep);

    // ─── Rule management ─────────────────────────────────────

    /**
     * @brief Replace the current DiceRules.
     * @param rules  New rules structure.
     */
    void setRules(const DiceRules& rules);

    /**
     * @brief Get a const reference to the current rules.
     */
    const DiceRules& getRules() const noexcept { return rules_; }

    /**
     * @brief Reload rules from the ConfigManager's current state.
     */
    void reloadRules();

    // ─── Critical / Fumble detection ──────────────────────────

    /**
     * @brief Check if a COC-style result is critical.
     * @param result  The D100 roll result.
     * @return true if critical success.
     */
    bool checkCritical(int result) const;

    /**
     * @brief Check if a COC-style result is a fumble.
     * @param result  The D100 roll result.
     * @return true if fumble / critical failure.
     */
    bool checkFumble(int result) const;

private:
    // ─── RNG ─────────────────────────────────────────────────
    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist100_{1, 100};
    std::uniform_int_distribution<int> dist10_{0, 9};

    // Returns a random int in [1, sides]
    int rollOneDie(int sides);

    // ─── Rules ───────────────────────────────────────────────
    DiceRules rules_;

    // ─── Expression engine ───────────────────────────────────
    DiceExpression expression_;

    // ─── Config reference ────────────────────────────────────
    ConfigManager& configMgr_;
};

}  // namespace dice
