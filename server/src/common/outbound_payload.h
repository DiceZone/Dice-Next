#pragma once
#include "markdown.h"
#include "content_format.h"
#include "qq_rich_reply.h"
#include <nlohmann/json.hpp>

namespace dice::outbound {
using Json = nlohmann::json;

inline Json qqText(const std::string& text, ContentFormat format, bool useMarkdown,
                   bool channel, const qq_rich::Card* card = nullptr,
                   const std::vector<presentation::Action>& actions = {},
                   const std::string& richStyle = "off", const std::string& interactions = "off",
                   const std::string& nativeUser = "", bool verifyImages = false, int fallbackLevel = 0) {
    const bool rich = useMarkdown && format == ContentFormat::kPlainText && richStyle != "off"
        && card && card->original == text;
    const bool interactive = useMarkdown && fallbackLevel == 0 && richStyle != "off" && !actions.empty();
    const auto commands = qq_rich::mergeCommands(rich ? card : nullptr, actions);
    std::string wire = useMarkdown
        ? (format == ContentFormat::kMarkdown ? text : markdown::escapeQQMarkdownLiteral(text))
        : (format == ContentFormat::kMarkdown ? markdown::toPlainText(text) : text);
    if (rich) wire = qq_rich::render(*card, fallbackLevel == 0 && richStyle == "math", false);
    if ((rich || interactive) && fallbackLevel == 0 && interactions == "links") wire += qq_rich::commandLinks(commands);
    if (rich && wire.size() > 3500)
        return qqText(text, format, false, channel, nullptr, {}, "off", "off", "", verifyImages, 2);
    Json body = useMarkdown
        ? Json{{"content", " "}, {"msg_type", 2}, {"markdown", {{"content", wire}, {"force_verify_image_resource", verifyImages}}}}
        : Json{{"content", wire.empty() && !text.empty() ? text : wire}};
    if (!useMarkdown && !channel) body["msg_type"] = 0;
    if ((rich || interactive) && fallbackLevel == 0 && interactions == "buttons") {
        auto keys = qq_rich::keyboard(commands, nativeUser);
        if (!keys.is_null()) body["keyboard"] = std::move(keys);
    }
    return body;
}

inline Json kookText(const std::string& target, const std::string& content,
                     ContentFormat format, bool cardMode) {
    if (!cardMode || content.size() > 5000)
        return Json{{"type", 1}, {"target_id", target},
            {"content", format == ContentFormat::kMarkdown ? markdown::toPlainText(content) : content}};
    const auto wire = format == ContentFormat::kMarkdown ? content : markdown::escapeLiteral(content);
    const auto card = Json::array({{{"type", "card"}, {"theme", "primary"}, {"size", "sm"},
        {"modules", Json::array({{{"type", "section"}, {"text", {{"type", "kmarkdown"}, {"content", wire}}}}})}}});
    return Json{{"type", 10}, {"target_id", target}, {"content", card.dump()}};
}

// Discord always interprets Markdown, including ordinary content messages.
inline std::string discordText(const std::string& text, ContentFormat format, bool rich) {
    if (format == ContentFormat::kMarkdown && rich) return text;
    return markdown::escapeLiteral(format == ContentFormat::kMarkdown ? markdown::toPlainText(text) : text);
}
inline Json discordPayload(const std::string& native, bool cardMode) {
    if (cardMode && native.size() <= 4096)
        return Json{{"embeds", Json::array({{{"description", native}, {"color", 0x5865F2}}})}};
    return Json{{"content", native}};
}
} // namespace dice::outbound
