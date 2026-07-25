#pragma once

#include <string>
#include <vector>
#include <map>

namespace dice {

/**
 * @brief CQ code type enumeration.
 *
 * CQ codes are a plain-text markup format used by QQ bots.
 * Examples: [CQ:at,qq=123456], [CQ:image,file=abc.jpg]
 */
enum class CQType {
    AT      = 0,
    IMAGE   = 1,
    FACE    = 2,
    RECORD  = 3,
    VIDEO   = 4,
    REPLY   = 5,
    FORWARD = 6,
    UNKNOWN = 99,
};

inline const char* cqTypeToString(CQType type) {
    switch (type) {
        case CQType::AT:      return "at";
        case CQType::IMAGE:   return "image";
        case CQType::FACE:    return "face";
        case CQType::RECORD:  return "record";
        case CQType::VIDEO:   return "video";
        case CQType::REPLY:   return "reply";
        case CQType::FORWARD: return "forward";
        default:              return "unknown";
    }
}

inline CQType cqTypeFromString(const std::string& s) {
    if (s == "at")      return CQType::AT;
    if (s == "image")   return CQType::IMAGE;
    if (s == "face")    return CQType::FACE;
    if (s == "record")  return CQType::RECORD;
    if (s == "video")   return CQType::VIDEO;
    if (s == "reply")   return CQType::REPLY;
    if (s == "forward") return CQType::FORWARD;
    return CQType::UNKNOWN;
}

/**
 * @brief Represents a single CQ code element.
 *
 * Format: [CQ:type,key1=val1,key2=val2,...]
 *
 * Example:
 *   CQCode{type=CQType::AT, params={{"qq", "123456"}}}
 *   → toString() → "[CQ:at,qq=123456]"
 */
struct CQCode {
    CQType type = CQType::UNKNOWN;
    std::map<std::string, std::string> params;

    CQCode() = default;

    explicit CQCode(CQType t) : type(t) {}

    CQCode(CQType t, std::map<std::string, std::string> p)
        : type(t), params(std::move(p)) {}

    /**
     * @brief Convert to CQ code string format.
     * @return String like "[CQ:at,qq=123456]".
     */
    std::string toString() const;

    /**
     * @brief Parse a single CQ code from a raw string.
     * @param cq  Raw string like "[CQ:at,qq=123456]".
     * @return Parsed CQCode. Type is UNKNOWN if parsing fails.
     */
    static CQCode fromString(const std::string& cq);

    /**
     * @brief Extract all CQ codes from a raw message string.
     * @param raw  Raw message text possibly containing multiple CQ codes.
     * @return Vector of parsed CQCode objects in order of appearance.
     */
    static std::vector<CQCode> parseMultiple(const std::string& raw);
};

}  // namespace dice
