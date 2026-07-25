#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

/**
 * @brief Dice rule configuration for all supported RPG systems.
 *
 * DiceRules holds the global dice-rolling parameters that affect
 * critical/fumble detection and system-specific rule variants.
 * The struct is designed to be loaded from JSON (e.g., the
 * "dice/rules" section of the server configuration) and supports
 * hot-reloading via the ConfigManager callback mechanism.
 *
 * Defaults:
 *   - COC: critical on result = 1, fumble on result >= 96 (100-die)
 *   - DnD: advantage off by default
 *   - Standard dice sides: 100
 *   - Fate and L5R off by default
 */
struct DiceRules {
    // ─── Fields (one-to-one with JSON keys) ───────────────────

    bool cocEnabled     = true;
    int  cocCriticalRange = 1;     // Result value considered critical
    int  cocFumbleRange   = 96;    // Result value >= this is a fumble (100-die)

    bool dndEnabled     = true;
    bool dndAdvantage   = false;

    bool fateEnabled    = false;
    bool l5rEnabled     = false;

    int  defaultDiceSides = 100;

    std::map<std::string, bool> customRules;  // Extensible rule flags

    // ─── JSON Serialization ───────────────────────────────────

    /**
     * @brief Populate this struct from a JSON object (typically
     *        the "dice/rules" subtree of server config).
     * @param j  JSON object containing the dice rules subtree.
     */
    void fromJSON(const json& j);

    /**
     * @brief Serialize this struct to a JSON object.
     * @return  A nlohmann::json representation.
     */
    json toJSON() const;

    /**
     * @brief Reset all fields to their default values.
     */
    void resetDefaults();
};

// ─── Inline JSON helpers ─────────────────────────────────────

inline void DiceRules::fromJSON(const json& j) {
    resetDefaults();

    if (j.is_null()) return;

    // COC
    if (j.contains("coc_enabled"))       cocEnabled       = j["coc_enabled"].get<bool>();
    if (j.contains("coc_critical_range")) cocCriticalRange = j["coc_critical_range"].get<int>();
    if (j.contains("coc_fumble_range"))   cocFumbleRange   = j["coc_fumble_range"].get<int>();

    // DnD
    if (j.contains("dnd_enabled"))  dndEnabled  = j["dnd_enabled"].get<bool>();
    if (j.contains("dnd_advantage")) dndAdvantage = j["dnd_advantage"].get<bool>();

    // Other systems
    if (j.contains("fate_enabled")) fateEnabled = j["fate_enabled"].get<bool>();
    if (j.contains("l5r_enabled"))  l5rEnabled  = j["l5r_enabled"].get<bool>();

    // Default dice sides
    if (j.contains("default_dice_sides")) defaultDiceSides = j["default_dice_sides"].get<int>();

    // Custom rules
    if (j.contains("custom_rules") && j["custom_rules"].is_object()) {
        for (auto& [key, val] : j["custom_rules"].items()) {
            customRules[key] = val.get<bool>();
        }
    }

    // Sanity checks
    if (cocCriticalRange < 1)  cocCriticalRange = 1;
    if (cocFumbleRange < 1)    cocFumbleRange   = 96;
    if (defaultDiceSides < 2)  defaultDiceSides = 2;
}

inline json DiceRules::toJSON() const {
    json j;
    j["coc_enabled"]        = cocEnabled;
    j["coc_critical_range"]  = cocCriticalRange;
    j["coc_fumble_range"]    = cocFumbleRange;
    j["dnd_enabled"]         = dndEnabled;
    j["dnd_advantage"]       = dndAdvantage;
    j["fate_enabled"]        = fateEnabled;
    j["l5r_enabled"]         = l5rEnabled;
    j["default_dice_sides"]  = defaultDiceSides;

    if (!customRules.empty()) {
        json cr = json::object();
        for (const auto& [k, v] : customRules) {
            cr[k] = v;
        }
        j["custom_rules"] = cr;
    }

    return j;
}

inline void DiceRules::resetDefaults() {
    cocEnabled       = true;
    cocCriticalRange = 1;
    cocFumbleRange   = 96;
    dndEnabled       = true;
    dndAdvantage     = false;
    fateEnabled      = false;
    l5rEnabled       = false;
    defaultDiceSides = 100;
    customRules.clear();
}

}  // namespace dice
