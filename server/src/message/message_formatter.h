#pragma once

#include "cq_code.h"
#include <string>
#include <vector>

namespace dice {

/**
 * @brief Internal message representation used by the server.
 *
 * InternalMessage is the canonical message format within the
 * Dice! server. All adapters convert their native message
 * formats to/from InternalMessage.
 *
 * Fields:
 *   - plainText:     Text content with CQ codes stripped.
 *   - richElements:  Parsed CQ code elements in order.
 *   - senderId:      Sender identifier (QQ number, etc.).
 *   - groupId:       Group identifier; empty for private messages.
 *   - time:          Message timestamp in ISO 8601 format.
 */
struct InternalMessage {
    std::string plainText;
    std::vector<CQCode> richElements;
    std::string senderId;
    std::string groupId;
    std::string time;

    InternalMessage() = default;

    bool isGroupMessage() const noexcept { return !groupId.empty(); }
    bool isPrivateMessage() const noexcept { return groupId.empty(); }
};

/**
 * @brief Message formatting utilities.
 *
 * Provides conversions between:
 *   - CQ code strings (the QQ bot wire format)
 *   - InternalMessage (the server's canonical format)
 *   - Plain text (CQ codes removed)
 */
class MessageFormatter {
public:
    MessageFormatter() = default;

    // ─── CQ Code Helpers ─────────────────────────────────────

    /**
     * @brief Format a single CQCode object into its string representation.
     */
    static std::string formatCQCode(const CQCode& code);

    /**
     * @brief Extract all CQ codes from raw text.
     */
    static std::vector<CQCode> parseCQCodes(const std::string& text);

    // ─── Internal ↔ CQ Conversions ──────────────────────────

    /**
     * @brief Convert an InternalMessage to a CQ code string
     *        suitable for sending via a QQ bot adapter.
     *
     * Reconstructs the original CQ-encoded string by interleaving
     * plain text with CQ code elements at their original positions.
     *
     * @param msg  The internal message.
     * @return CQ-encoded string.
     */
    static std::string internalToCQ(const InternalMessage& msg);

    /**
     * @brief Parse a raw CQ-encoded string into an InternalMessage.
     *
     * @param cqString  The raw QQ message string.
     * @param senderId  The sender identifier.
     * @param groupId   Optional group identifier.
     * @return InternalMessage with plain text and rich elements parsed.
     */
    static InternalMessage cqToInternal(const std::string& cqString,
                                         const std::string& senderId = "",
                                         const std::string& groupId = "");

    // ─── Text Utilities ─────────────────────────────────────

    /**
     * @brief Strip all CQ codes from a string, returning only
     *        the human-readable plain text.
     */
    static std::string plainText(const std::string& cqString);

    /**
     * @brief Escape a text string so it won't be misinterpreted
     *        as CQ codes (replaces '[' and ',' with full-width
     *        equivalents).
     */
    static std::string escapeCQ(const std::string& text);
};

}  // namespace dice
