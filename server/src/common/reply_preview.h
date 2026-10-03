#pragma once
#include "outbound_payload.h"
#include <stdexcept>

namespace dice::outbound {
// No adapters, credentials, identity lookups or network calls: preview only
// exercises the production text/card serializers, before media processing.
inline Json replyPreview(const std::string& text, ContentFormat format,
                         const std::string& platform, PresentationStyle style, bool forcePlain = false) {
    const bool qq = platform == "qq_group" || platform == "qq_private" || platform == "qq_channel";
    const bool channel = platform == "qq_channel";
    if (!qq && platform != "kook" && platform != "discord" && platform != "plain")
        throw std::invalid_argument("unsupported preview platform");
    const bool rich = !forcePlain && style != PresentationStyle::kTraditional && !channel && platform != "plain";
    auto expanded = presentation::expandComponents(text, style, rich ? ContentFormat::kMarkdown : ContentFormat::kPlainText);
    if (expanded.usedMarkdown) format = ContentFormat::kMarkdown;
    const auto plain = format == ContentFormat::kMarkdown ? markdown::toPlainText(expanded.text) : expanded.text;
    Json payload;
    if (qq) payload = qqText(expanded.text, format, rich, channel, nullptr, expanded.actions,
        "markdown", "buttons", "preview-user");
    else if (platform == "kook") payload = kookText("preview-target", expanded.text, format, rich);
    else if (platform == "discord") payload = discordPayload(discordText(expanded.text, format, rich), rich);
    // Plain adapters also parse CQ/media and map transport IDs; don't invent
    // fake OneBot/Milky segments which would claim to be final wire data.
    Json actions = Json::array();
    if (qq && rich)
        for (const auto& action : expanded.actions) actions.push_back({{"label", action.label}, {"text", action.text}});
    return {{"text", rich ? expanded.text : plain}, {"plain", plain},
        {"markdown", rich && format == ContentFormat::kMarkdown}, {"payload", payload}, {"actions", actions}};
}
} // namespace dice::outbound
