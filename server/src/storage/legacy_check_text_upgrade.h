#pragma once
#include "database.h"
#include "legacy_message_keys.h"
#include "../common/legacy_check_replies.h"
#include "../common/safe_template.h"
#include "../common/weighted_templates.h"
#include "../common/utils.h"
#include "../i18n/i18n.h"
#include <nlohmann/json.hpp>
#include <map>
#include <set>

namespace dice::legacyv2 {
inline constexpr const char* checkTextUpgradeMarker = "migration:legacy-check-texts:v1";

inline nlohmann::json checkTextDetail(const std::string& source, const std::string& target,
                                    const std::string& text) {
    std::vector<std::string> issues;
    const auto variables = legacy_check_replies::variables(target);
    std::set<std::string> allowed(variables.begin(), variables.end());
    if (target == "dice.check.hidden") allowed.insert("attr");
    std::set<std::string> knownReferences;
    for (const auto& [original, mapped] : msgKeyMap()) knownReferences.insert(mapped);
    // These are resolved by the normal reply formatter after interpolation.
    allowed.insert({"self", "nick", "name", "qqnick", "card", "pcname", "qqnickw", "cardw", "pcnamew", "user", "group", "date", "time"});
    bool preserved = false;
    std::vector<std::string> texts{text};
    const auto variants = weighted_templates::decode(text);
    if (!variants.empty()) {
        texts.clear();
        for (const auto& variant : variants) texts.push_back(variant["text"].get<std::string>());
    }
    for (const auto& variant : texts) {
        const auto program = safe_template::compile(normalizeLegacyTemplate(source, variant));
        preserved = preserved || !program.issues.empty();
        issues.insert(issues.end(), program.issues.begin(), program.issues.end());
        for (const auto& field : program.selectors)
            if (std::find(variables.begin(), variables.end(), field) == variables.end())
                issues.push_back("unavailable condition field: " + field);
        for (const auto& field : program.variables)
            if (!allowed.count(field)) issues.push_back("unavailable variable: " + field);
        for (const auto& reference : program.references) {
            if (reference == target) { preserved = true; issues.push_back("cyclic text reference: " + reference); }
            else if (!knownReferences.count(reference)) issues.push_back("unverified text reference: " + reference);
        }
    }
    std::sort(issues.begin(), issues.end());
    issues.erase(std::unique(issues.begin(), issues.end()), issues.end());
    return {{"source", source}, {"target", target},
        {"status", preserved ? "preserved" : issues.empty() ? "active" : "partial"}, {"issues", issues}};
}

// Upgrade old orphan/incorrect mappings once, in a transaction. Never remove
// originals or replace an existing target (including an intentional empty one).
// A persistent marker prevents an explicit reset from resurrecting on restart.
inline nlohmann::json upgradeLegacyCheckTexts(Database& db) {
    using J = nlohmann::json;
    auto* storage = db.getStorage();
    J report{{"items", J::array()}, {"restored", 0}, {"conflicts", 0}, {"preserved", 0}};
    if (!storage) return report;
    const auto existing = storage->get_all<DiceConfigRow>(orm::where(orm::c(&DiceConfigRow::key) == std::string(checkTextUpgradeMarker)));
    if (!existing.empty()) { auto saved = J::parse(existing.front().value); saved["alreadyApplied"] = true; return saved; }
    report["appliedAt"] = utils::nowIso8601();
    std::map<std::string, std::string> sources;
    for (const auto& [original, target] : msgKeyMap())
        if (legacy_check_replies::isKey(target) || original == "strRollSkillHidden" || original == "strEnDefaultName")
            sources["legacy." + original] = target;
    sources["dice.crit"] = "dice.compat.check.single.critical";
    sources["dice.fumble"] = "dice.compat.check.single.fumble";
    storage->transaction([&] {
        const auto originals = storage->get_all<I18nOverrideRow>();
        // Saved original aliases take precedence over historical generic slots.
        for (const auto& [source, target] : sources) if (source.rfind("legacy.", 0) == 0)
            for (const auto& row : originals) if (row.key == source) {
                auto detail = checkTextDetail(source, target, row.value);
                const auto matches = storage->get_all<I18nOverrideRow>(orm::where(
                    orm::c(&I18nOverrideRow::locale) == row.locale and orm::c(&I18nOverrideRow::key) == target));
                if (!matches.empty()) { detail["status"] = "conflict"; report["conflicts"] = report["conflicts"].get<int>() + 1; }
                else if (detail["status"] == "preserved") report["preserved"] = report["preserved"].get<int>() + 1;
                else {
                    auto copy = row; copy.id = 0; copy.key = target;
                    copy.value = normalizeLegacyTemplate(source.substr(7), copy.value); storage->insert(copy);
                    report["restored"] = report["restored"].get<int>() + 1;
                }
                detail["locale"] = row.locale; report["items"].push_back(detail);
            }
        for (const auto* source : {"dice.crit", "dice.fumble"})
            for (const auto& row : originals) if (row.key == source) {
                const auto& target = sources.at(source);
                if (!storage->get_all<I18nOverrideRow>(orm::where(orm::c(&I18nOverrideRow::locale) == row.locale and
                    orm::c(&I18nOverrideRow::key) == target)).empty()) continue;
                auto detail = checkTextDetail(source, target, row.value);
                if (detail["status"] == "preserved") report["preserved"] = report["preserved"].get<int>() + 1;
                else { auto copy = row; copy.id = 0; copy.key = target; copy.value = normalizeLegacyTemplate(source, row.value); storage->insert(copy);
                    report["restored"] = report["restored"].get<int>() + 1; }
                detail["locale"] = row.locale; report["items"].push_back(detail);
            }
        // Personas get the same non-destructive correction before cache loading.
        const auto entries = storage->get_all<PersonaEntryRow>();
        // Visit aliases first here too, independently of database insertion order.
        for (const bool aliases : {true, false})
            for (const auto& row : entries) if (sources.count(row.key) && (row.key.rfind("legacy.", 0) == 0) == aliases) {
                const auto& target = sources.at(row.key);
                auto detail = checkTextDetail(row.key, target, row.value);
                const auto matches = storage->get_all<PersonaEntryRow>(orm::where(orm::c(&PersonaEntryRow::personaId) == row.personaId and
                    orm::c(&PersonaEntryRow::locale) == row.locale and orm::c(&PersonaEntryRow::key) == target));
                if (!matches.empty()) { detail["status"] = "conflict"; report["conflicts"] = report["conflicts"].get<int>() + 1; }
                else if (detail["status"] == "preserved") report["preserved"] = report["preserved"].get<int>() + 1;
                else {
                    auto copy = row; copy.id = 0; copy.key = target;
                    copy.value = normalizeLegacyTemplate(row.key, row.value);
                    storage->insert(copy); report["restored"] = report["restored"].get<int>() + 1;
                }
                detail["locale"] = row.locale; detail["personaId"] = row.personaId; report["items"].push_back(detail);
            }
        DiceConfigRow marker; marker.key = checkTextUpgradeMarker; marker.value = report.dump(); storage->insert(marker);
        return true;
    });
    return report;
}
} // namespace dice::legacyv2
