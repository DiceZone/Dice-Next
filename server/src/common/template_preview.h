#pragma once
#include "weighted_templates.h"
#include "reply_preview.h"
#include "../i18n/i18n.h"

namespace dice::outbound {
// Credential-free rendering shared by the HTTP endpoint and the local UI driver.
inline json templatePreview(const json& body) {
    std::string text = body.contains("variants") ? weighted_templates::encode(body["variants"])
                                               : body.value("text", "");
    if (weighted_templates::decode(text).empty() && text.size() > 65536)
        throw std::invalid_argument("preview text too long");
    const auto formatName = body.value("format", std::string("plain"));
    if (formatName != "markdown" && formatName != "plain") throw std::invalid_argument("invalid format");
    const auto format = contentFormatFromString(formatName);
    const auto styleName = body.value("style", std::string("visual"));
    if (styleName != "traditional" && styleName != "standard" && styleName != "visual")
        throw std::invalid_argument("invalid presentation style");
    const auto style = presentationStyleFromString(styleName);
    I18n::Args args;
    if (body.contains("args")) {
        if (!body["args"].is_object() || body["args"].size() > 256)
            throw std::invalid_argument("invalid preview arguments");
        for (const auto& [key, value] : body["args"].items()) {
            if (!value.is_string() || value.get_ref<const std::string&>().size() > 4096)
                throw std::invalid_argument("invalid preview argument");
            args[key] = value.get<std::string>();
        }
    }
    // One draw shared by all views. Substituted user data never becomes code.
    text = I18n::previewTemplate(text, args, format);
    return json{{"templateVersion", 1}, {"preview", replyPreview(text, format,
            body.value("platform", std::string("qq_group")), style, body.value("forcePlain", false))},
        {"markdown", replyPreview(text, format, "qq_group", style)["text"]},
        {"onebot", replyPreview(text, format, "plain", style)["text"]}};
}
}
