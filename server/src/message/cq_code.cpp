#include "cq_code.h"
#include "../common/logger.h"

#include <regex>
#include <sstream>

namespace dice {

// ─── Regex for CQ codes ─────────────────────────────────────
// Matches [CQ:type,key=val,key=val,...]
// Captures: type (alphanumeric), then the entire params tail
static const std::regex kCQRegex(
    R"(\[CQ:([a-zA-Z0-9_]+),([^\[\]]*)\])",
    std::regex::ECMAScript
);

// Matches a single key=value pair in params
static const std::regex kParamRegex(
    R"(([a-zA-Z0-9_]+)=([^,]+))",
    std::regex::ECMAScript
);

// ═══════════════════════════════════════════════════════════════
// CQCode::toString
// ═══════════════════════════════════════════════════════════════

std::string CQCode::toString() const {
    std::ostringstream oss;
    oss << "[CQ:" << cqTypeToString(type);

    for (const auto& [key, val] : params) {
        oss << "," << key << "=" << val;
    }

    oss << "]";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// CQCode::fromString
// ═══════════════════════════════════════════════════════════════

CQCode CQCode::fromString(const std::string& cq) {
    CQCode result;

    std::smatch match;
    if (!std::regex_match(cq, match, kCQRegex)) {
        DICE_LOG_DEBUG("CQCode: failed to parse '{}'", cq);
        return result;
    }

    // match[1] = type string
    // match[2] = params tail (e.g., "qq=123456,text=hello")
    std::string typeStr = match[1].str();
    std::string paramsTail = match[2].str();

    result.type = cqTypeFromString(typeStr);

    // Parse key=value pairs
    auto paramsBegin = std::sregex_iterator(paramsTail.begin(), paramsTail.end(), kParamRegex);
    auto paramsEnd   = std::sregex_iterator();

    for (auto it = paramsBegin; it != paramsEnd; ++it) {
        std::string key = (*it)[1].str();
        std::string val = (*it)[2].str();

        // Trim trailing whitespace from val
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) {
            val.pop_back();
        }

        result.params[key] = val;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// CQCode::parseMultiple
// ═══════════════════════════════════════════════════════════════

std::vector<CQCode> CQCode::parseMultiple(const std::string& raw) {
    std::vector<CQCode> result;

    auto begin = std::sregex_iterator(raw.begin(), raw.end(), kCQRegex);
    auto end   = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        CQCode code;
        code.type = cqTypeFromString((*it)[1].str());

        std::string paramsTail = (*it)[2].str();

        auto pBegin = std::sregex_iterator(paramsTail.begin(), paramsTail.end(), kParamRegex);
        auto pEnd   = std::sregex_iterator();

        for (auto pIt = pBegin; pIt != pEnd; ++pIt) {
            std::string key = (*pIt)[1].str();
            std::string val = (*pIt)[2].str();

            while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) {
                val.pop_back();
            }

            code.params[key] = val;
        }

        result.push_back(std::move(code));
    }

    return result;
}

}  // namespace dice
