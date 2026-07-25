#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

/**
 * @brief Result of a dice roll operation.
 *
 * Carries every detail needed for formatted output and rule
 * detection (critical/fumble flags). Conforms to the 8.5
 * cross-file conventions.
 *
 * Fields:
 *   - expression:       The original dice expression string
 *   - individualResults: Each die's individual face value
 *   - total:            Sum of all individual results
 *   - modifiedTotal:    Total after applying modifiers (e.g., ±N)
 *   - isCritical:       Whether the roll is a critical success
 *   - isFumble:         Whether the roll is a fumble / critical failure
 *   - detail:           Human-readable breakdown (e.g. "2d6+3 = [4, 2] + 3 = 9")
 *   - formattedOutput:  Final formatted string for chat output
 *   - error:            Non-empty if the roll failed
 *   - errorCode:        Numeric API error code (0 = success)
 */
struct DiceResult {
    std::string expression;
    std::vector<int> individualResults;
    int total = 0;
    int modifiedTotal = 0;
    bool isCritical = false;
    bool isFumble = false;
    std::string detail;
    std::string formattedOutput;
    std::string error;
    int errorCode = 0;

    // ─── Convenience ──────────────────────────────────────────

    bool ok() const noexcept { return error.empty(); }
    bool hasError() const noexcept { return !error.empty(); }

    // ─── Static factory methods ───────────────────────────────

    /**
     * @brief Create a successful result from raw data.
     */
    static DiceResult success(
        const std::string& expr,
        const std::vector<int>& results,
        int modTotal,
        bool crit,
        bool fumble,
        const std::string& detailStr,
        const std::string& formattedStr);

    /**
     * @brief Create an error result.
     * @param expr       The expression that caused the error.
     * @param errorMsg   Human-readable error description.
     * @param code       Numeric error code (e.g., 2001 for invalid expression).
     */
    static DiceResult failure(
        const std::string& expr,
        const std::string& errorMsg,
        int code = 2001);

    // ─── JSON Serialization ───────────────────────────────────

    /**
     * @brief Serialize to JSON for REST API responses.
     */
    json toJSON() const;
};

// ─── Inline implementation ───────────────────────────────────

inline DiceResult DiceResult::success(
    const std::string& expr,
    const std::vector<int>& results,
    int modTotal,
    bool crit,
    bool fumble,
    const std::string& detailStr,
    const std::string& formattedStr) {

    DiceResult r;
    r.expression = expr;
    r.individualResults = results;

    // Compute total from individual results
    r.total = 0;
    for (int v : results) r.total += v;

    r.modifiedTotal = modTotal;
    r.isCritical = crit;
    r.isFumble = fumble;
    r.detail = detailStr;
    r.formattedOutput = formattedStr;
    return r;
}

inline DiceResult DiceResult::failure(
    const std::string& expr,
    const std::string& errorMsg,
    int code) {

    DiceResult r;
    r.expression = expr;
    r.error = errorMsg;
    r.errorCode = code;
    return r;
}

inline json DiceResult::toJSON() const {
    json j;
    j["expression"]         = expression;
    j["individual_results"]  = individualResults;
    j["total"]              = total;
    j["modified_total"]     = modifiedTotal;
    j["is_critical"]        = isCritical;
    j["is_fumble"]          = isFumble;
    j["detail"]             = detail;
    j["formatted_output"]   = formattedOutput;

    if (!ok()) {
        j["error"]      = error;
        j["error_code"] = errorCode;
    }

    return j;
}

}  // namespace dice
