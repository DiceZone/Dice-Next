#pragma once
#include "weighted_templates.h"
#include "reply_preview.h"
#include "template_preview_context.h"
#include "../storage/legacy_message_keys.h"
#include "../i18n/i18n.h"

namespace dice::outbound {
// Credential-free rendering shared by the HTTP endpoint and the local UI driver.
inline json templatePreview(const json& body, const I18n* i18n = nullptr) {
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
    // Persona-specific grade/receipt texts must apply while computing the
    // example as well as while rendering the final template.
    const I18n::PersonaScope personaScope(body.value("personaId", 0));
    std::optional<PreviewContext> simulation;
    if (body.contains("simulation")) {
        if (body.contains("args")) throw std::invalid_argument("simulation arguments cannot be overridden");
        auto request = body["simulation"];
        if (!request.is_object()) throw std::invalid_argument("invalid preview simulation");
        request["locale"] = body.value("locale", std::string("zh-Hans"));
        simulation = templatePreviewContext(request, i18n);
    }
    sample_template::RandomScope randomScope(simulation ? std::optional(simulation->seed) : std::nullopt);
    I18n::Args args;
    if (simulation) args = simulation->args;
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
    if (i18n) {
        const auto locale = localeFromString(body.value("locale", std::string("zh-Hans")));
        text = i18n->previewWithReferences(locale, text, args, format);
    } else text = I18n::previewTemplate(text, args, format);
    json result{{"templateVersion", 1}, {"preview", replyPreview(text, format,
            body.value("platform", std::string("qq_group")), style, body.value("forcePlain", false))},
        {"markdown", replyPreview(text, format, "qq_group", style)["text"]},
        {"onebot", replyPreview(text, format, "plain", style)["text"]}};
    if (simulation) {
        // Inspect authored variables, never re-parse inserted user data. Unknown
        // fields remain visible and are reported instead of inventing values.
        json unresolved = json::array();
        json issues = json::array();
        const auto authored = body.contains("variants") ? weighted_templates::encode(body["variants"]) : body.value("text", "");
        auto variants = weighted_templates::decode(authored);
        if (variants.empty()) variants = json::array({{{"text", authored}, {"weight", 1}}});
        for (const auto& variant : variants) {
            const auto program = safe_template::compile(legacyv2::normalizeLegacyReferences(variant["text"].get<std::string>()));
            for (const auto& variable : program.variables)
                if (!args.count(variable) && std::find(program.locals.begin(), program.locals.end(), variable) == program.locals.end() &&
                    (!i18n || i18n->getDefault(localeFromString(body.value("locale", std::string("zh-Hans"))), variable).empty()) &&
                    std::find(unresolved.begin(), unresolved.end(), json(variable)) == unresolved.end()) unresolved.push_back(variable);
            for (const auto& issue : program.issues) issues.push_back(issue);
        }
        result["simulation"] = {{"version", 1}, {"seed", simulation->seed}, {"command", simulation->command},
            {"family", simulation->family}, {"grade", simulation->grade}, {"args", args}, {"facts", simulation->facts},
            {"unresolved", unresolved}, {"issues", issues}};
    }
    return result;
}
}
