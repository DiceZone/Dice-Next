#include "message_formatter.h"
#include "../common/logger.h"
#include "../common/utils.h"

#include <regex>
#include <sstream>

namespace dice {

// ─── Regex for CQ codes ─────────────────────────────────────
static const std::regex kCQCodeRegex(
    R"(\[CQ:[a-zA-Z0-9_]+,[^\[\]]*\])",
    std::regex::ECMAScript
);

// ═══════════════════════════════════════════════════════════════
// CQ Code Helpers
// ═══════════════════════════════════════════════════════════════

std::string MessageFormatter::formatCQCode(const CQCode& code) {
    return code.toString();
}

std::vector<CQCode> MessageFormatter::parseCQCodes(const std::string& text) {
    return CQCode::parseMultiple(text);
}

// ═══════════════════════════════════════════════════════════════
// Internal ↔ CQ
// ═══════════════════════════════════════════════════════════════

std::string MessageFormatter::internalToCQ(const InternalMessage& msg) {
    // For now, reconstruct by placing CQ codes in sequence
    // A more sophisticated implementation would track positions
    if (msg.richElements.empty()) {
        return msg.plainText;
    }

    std::ostringstream oss;

    // If we have rich elements, they were extracted from positions
    // in the original message. For simplicity, append them after the
    // plain text (adapters can customize this behavior).
    if (!msg.plainText.empty()) {
        oss << msg.plainText;
    }

    for (const auto& elem : msg.richElements) {
        oss << elem.toString();
    }

    return oss.str();
}

InternalMessage MessageFormatter::cqToInternal(
    const std::string& cqString,
    const std::string& senderId,
    const std::string& groupId) {

    InternalMessage msg;
    msg.senderId = senderId;
    msg.groupId = groupId;
    msg.time = utils::nowIso8601();

    // Parse all CQ codes
    msg.richElements = CQCode::parseMultiple(cqString);

    // Extract plain text by removing all CQ code blocks
    msg.plainText = std::regex_replace(cqString, kCQCodeRegex, "");

    // Trim excess whitespace from the plain text
    msg.plainText = utils::trim(msg.plainText);

    return msg;
}

// ═══════════════════════════════════════════════════════════════
// Text Utilities
// ═══════════════════════════════════════════════════════════════

std::string MessageFormatter::plainText(const std::string& cqString) {
    // Remove all CQ codes, return the remaining text
    std::string result = std::regex_replace(cqString, kCQCodeRegex, "");
    return utils::trim(result);
}

std::string MessageFormatter::escapeCQ(const std::string& text) {
    // Replace '[' and ',' with full-width equivalents to
    // prevent accidental CQ code interpretation.
    std::string result = text;
    result = utils::replaceAll(result, "[", "［");
    result = utils::replaceAll(result, "]", "］");
    result = utils::replaceAll(result, ",", "，");
    return result;
}

}  // namespace dice
