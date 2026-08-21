// ─── Dice!Next v3.0.0 — Persona Manager ──────────────
#include "persona_manager.h"
#include "../../common/logger.h"
#include "../../common/utils.h"

#include <sqlite_orm/sqlite_orm.h>
#include <algorithm>

namespace dice {

namespace orm = sqlite_orm;

PersonaManager::PersonaManager(Database& db, I18n& i18n, ConfigManager& cfg)
    : db_(db), i18n_(i18n), cfg_(cfg) {}

std::string PersonaManager::nowIso() {
    return utils::nowIso8601();
}

// ═══════════════════════════════════════════════════════════════
// Startup
// ═══════════════════════════════════════════════════════════════

void PersonaManager::loadStartupPersona() {
    int globalId = cfg_.get<int>("persona/global", 0);
    if (globalId > 0) {
        // Verify the persona exists
        auto tmpl = getTemplateById(globalId);
        if (tmpl.id > 0) {
            loadIntoI18n(globalId);
            i18n_.setPersona(globalId);
            DICE_LOG_INFO("PersonaManager: startup persona '{}' (id={}) loaded", tmpl.name, globalId);
        } else {
            DICE_LOG_WARN("PersonaManager: configured global persona id={} not found, using default", globalId);
            i18n_.setPersona(0);
        }
    } else {
        DICE_LOG_INFO("PersonaManager: no global persona configured (default)");
        i18n_.setPersona(0);
    }
}

// ═══════════════════════════════════════════════════════════════
// Active persona resolution
// ═══════════════════════════════════════════════════════════════

int PersonaManager::getActivePersona(const std::string& groupId) const {
    // Per-group override first
    if (!groupId.empty()) {
        auto* st = db_.getStorage();
        if (st) {
            try {
                // group_settings stores persona as key="persona" per group
                // We search across all platforms for this groupId
                auto rows = st->get_all<GroupSettingRow>(
                    orm::where(orm::c(&GroupSettingRow::groupId) == groupId
                               and orm::c(&GroupSettingRow::key) == std::string("persona")),
                    orm::limit(1));
                if (!rows.empty()) {
                    return std::stoi(rows.front().value);
                }
            } catch (...) {}
        }
    }
    // Fall back to global
    return cfg_.get<int>("persona/global", 0);
}

void PersonaManager::setActivePersona(int personaId, const std::string& groupId) {
    if (groupId.empty()) {
        // Global persona
        cfg_.set<int>("persona/global", personaId);
        cfg_.save();
        DICE_LOG_INFO("PersonaManager: global persona set to {}", personaId);
    } else {
        // Per-group persona — store in group_settings
        auto* st = db_.getStorage();
        if (st) {
            try {
                auto rows = st->get_all<GroupSettingRow>(
                    orm::where(orm::c(&GroupSettingRow::groupId) == groupId
                               and orm::c(&GroupSettingRow::key) == std::string("persona")),
                    orm::limit(1));
                if (!rows.empty()) {
                    auto row = rows.front();
                    row.value = std::to_string(personaId);
                    st->update(row);
                } else {
                    GroupSettingRow row;
                    row.platform = "onebot_v11";
                    row.groupId = groupId;
                    row.key = "persona";
                    row.value = std::to_string(personaId);
                    st->insert(row);
                }
                DICE_LOG_INFO("PersonaManager: group '{}' persona set to {}", groupId, personaId);
            } catch (const std::exception& e) {
                DICE_LOG_ERROR("PersonaManager: failed to set group persona: {}", e.what());
            }
        }
    }

    // If this is the global persona or the current context, reload I18n
    if (groupId.empty()) {
        loadIntoI18n(personaId);
        i18n_.setPersona(personaId);
    }
}

// ═══════════════════════════════════════════════════════════════
// Template CRUD
// ═══════════════════════════════════════════════════════════════

std::vector<PersonaTemplateRow> PersonaManager::listTemplates() const {
    auto* st = db_.getStorage();
    if (!st) return {};
    try {
        return st->get_all<PersonaTemplateRow>(
            orm::order_by(&PersonaTemplateRow::id));
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("PersonaManager: listTemplates failed: {}", e.what());
        return {};
    }
}

PersonaTemplateRow PersonaManager::getTemplateById(int id) const {
    auto* st = db_.getStorage();
    if (!st) return {};
    try {
        return st->get<PersonaTemplateRow>(id);
    } catch (...) {
        return {};
    }
}

PersonaTemplateRow PersonaManager::getTemplateByName(const std::string& name) const {
    auto* st = db_.getStorage();
    if (!st) return {};
    try {
        auto rows = st->get_all<PersonaTemplateRow>(
            orm::where(orm::c(&PersonaTemplateRow::name) == name),
            orm::limit(1));
        if (!rows.empty()) return rows.front();
    } catch (...) {}
    return {};
}

int PersonaManager::createTemplate(const std::string& name, const std::string& description) {
    if (name.empty()) return -1;
    auto* st = db_.getStorage();
    if (!st) return -1;

    // Check for duplicate name
    auto existing = getTemplateByName(name);
    if (existing.id > 0) return -1;

    try {
        PersonaTemplateRow row;
        row.name = name;
        row.description = description;
        row.isBuiltin = false;
        row.createdAt = nowIso();
        row.updatedAt = nowIso();
        return st->insert(row);
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("PersonaManager: createTemplate failed: {}", e.what());
        return -1;
    }
}

int PersonaManager::copyTemplate(int srcId, const std::string& newName) {
    if (newName.empty()) return -1;
    auto* st = db_.getStorage();
    if (!st) return -1;

    auto src = getTemplateById(srcId);
    if (src.id <= 0) return -1;

    // Check for duplicate name
    auto existing = getTemplateByName(newName);
    if (existing.id > 0) return -1;

    try {
        // Create the new template
        PersonaTemplateRow row;
        row.name = newName;
        row.description = src.description;
        row.isBuiltin = false;
        row.createdAt = nowIso();
        row.updatedAt = nowIso();
        int newId = st->insert(row);

        // Copy all entries
        auto entries = listEntries(srcId);
        for (const auto& e : entries) {
            PersonaEntryRow er;
            er.personaId = newId;
            er.locale = e.locale;
            er.key = e.key;
            er.value = e.value;
            er.format = e.format;
            st->insert(er);
        }

        DICE_LOG_INFO("PersonaManager: copied persona '{}' (id={}) → '{}' (id={}), {} entries",
                      src.name, srcId, newName, newId, entries.size());
        return newId;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("PersonaManager: copyTemplate failed: {}", e.what());
        return -1;
    }
}

bool PersonaManager::deleteTemplate(int id) {
    auto* st = db_.getStorage();
    if (!st) return false;

    auto tmpl = getTemplateById(id);
    if (tmpl.id <= 0) return false;
    if (tmpl.isBuiltin) return false;  // built-in cannot be deleted

    try {
        // Delete all entries first
        st->remove_all<PersonaEntryRow>(
            orm::where(orm::c(&PersonaEntryRow::personaId) == id));
        // Delete the template
        st->remove<PersonaTemplateRow>(id);
        DICE_LOG_INFO("PersonaManager: deleted persona '{}' (id={})", tmpl.name, id);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("PersonaManager: deleteTemplate failed: {}", e.what());
        return false;
    }
}

bool PersonaManager::updateTemplateMeta(int id, const std::string& name, const std::string& description) {
    auto* st = db_.getStorage();
    if (!st) return false;

    auto tmpl = getTemplateById(id);
    if (tmpl.id <= 0) return false;

    // Check name uniqueness if changing
    if (name != tmpl.name) {
        auto existing = getTemplateByName(name);
        if (existing.id > 0) return false;
    }

    try {
        tmpl.name = name.empty() ? tmpl.name : name;
        tmpl.description = description;
        tmpl.updatedAt = nowIso();
        st->update(tmpl);
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("PersonaManager: updateTemplateMeta failed: {}", e.what());
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════
// Entry CRUD
// ═══════════════════════════════════════════════════════════════

std::vector<PersonaEntryRow> PersonaManager::listEntries(int personaId) const {
    auto* st = db_.getStorage();
    if (!st) return {};
    try {
        return st->get_all<PersonaEntryRow>(
            orm::where(orm::c(&PersonaEntryRow::personaId) == personaId),
            orm::order_by(&PersonaEntryRow::key));
    } catch (...) {
        return {};
    }
}

bool PersonaManager::setEntry(int personaId, const std::string& locale,
                               const std::string& key, const std::string& value,
                               const std::string& format) {
    auto* st = db_.getStorage();
    if (!st) return false;

    try {
        // Check if entry exists (upsert)
        auto rows = st->get_all<PersonaEntryRow>(
            orm::where(orm::c(&PersonaEntryRow::personaId) == personaId
                       and orm::c(&PersonaEntryRow::locale) == locale
                       and orm::c(&PersonaEntryRow::key) == key),
            orm::limit(1));
        if (!rows.empty()) {
            auto row = rows.front();
            row.value = value;
            row.format = format == "markdown" ? "markdown" : "plain";
            st->update(row);
        } else {
            PersonaEntryRow row;
            row.personaId = personaId;
            row.locale = locale;
            row.key = key;
            row.value = value;
            row.format = format == "markdown" ? "markdown" : "plain";
            st->insert(row);
        }
        return true;
    } catch (const std::exception& e) {
        DICE_LOG_ERROR("PersonaManager: setEntry failed: {}", e.what());
        return false;
    }
}

bool PersonaManager::deleteEntry(int personaId, const std::string& locale, const std::string& key) {
    auto* st = db_.getStorage();
    if (!st) return false;
    try {
        st->remove_all<PersonaEntryRow>(
            orm::where(orm::c(&PersonaEntryRow::personaId) == personaId
                       and orm::c(&PersonaEntryRow::locale) == locale
                       and orm::c(&PersonaEntryRow::key) == key));
        return true;
    } catch (...) {
        return false;
    }
}

int PersonaManager::getEntryCount(int personaId) const {
    auto* st = db_.getStorage();
    if (!st) return 0;
    try {
        return (int)st->count<PersonaEntryRow>(
            orm::where(orm::c(&PersonaEntryRow::personaId) == personaId));
    } catch (...) {
        return 0;
    }
}

std::vector<std::string> PersonaManager::getEntryKeys(int personaId) const {
    auto entries = listEntries(personaId);
    std::vector<std::string> keys;
    keys.reserve(entries.size());
    for (const auto& e : entries) keys.push_back(e.key);
    return keys;
}

// ═══════════════════════════════════════════════════════════════
// I18n injection
// ═══════════════════════════════════════════════════════════════

void PersonaManager::loadIntoI18n(int personaId) {
    if (personaId <= 0) {
        i18n_.clearPersonaBundles();
        return;
    }

    auto entries = listEntries(personaId);
    if (entries.empty()) {
        i18n_.clearPersonaBundles();
        return;
    }

    // Group entries by locale and build flat JSON objects
    std::map<Locale, json> bundlesByLocale;
    std::map<Locale, json> formatsByLocale;
    for (const auto& e : entries) {
        Locale loc = localeFromString(e.locale);
        bundlesByLocale[loc][e.key] = e.value;
        formatsByLocale[loc][e.key] = e.format;
    }

    // Clear existing persona bundles first
    i18n_.clearPersonaBundles();

    // Inject each locale's bundle
    for (const auto& [loc, bundle] : bundlesByLocale) {
        i18n_.setPersonaBundles(loc, bundle, formatsByLocale[loc]);
    }

    DICE_LOG_INFO("PersonaManager: loaded {} entries ({} locales) into I18n for persona {}",
                  entries.size(), bundlesByLocale.size(), personaId);
}

// ═══════════════════════════════════════════════════════════════
// Import / Export
// ═══════════════════════════════════════════════════════════════

json PersonaManager::exportTemplate(int id) const {
    auto tmpl = getTemplateById(id);
    if (tmpl.id <= 0) return nullptr;

    json j;
    j["name"] = tmpl.name;
    j["description"] = tmpl.description;
    j["isBuiltin"] = tmpl.isBuiltin;

    auto entries = listEntries(id);
    json entriesArr = json::array();
    for (const auto& e : entries) {
        entriesArr.push_back(json{
            {"locale", e.locale},
            {"key", e.key},
            {"value", e.value},
            {"format", e.format}
        });
    }
    j["entries"] = entriesArr;
    return j;
}

int PersonaManager::importTemplate(const json& data) {
    if (!data.is_object()) return -1;
    std::string name = data.value("name", "");
    if (name.empty()) return -1;

    std::string description = data.value("description", "");

    int newId = createTemplate(name, description);
    if (newId < 0) return -1;

    // Import entries
    if (data.contains("entries") && data["entries"].is_array()) {
        for (const auto& e : data["entries"]) {
            std::string locale = e.value("locale", "zh-Hans");
            std::string key = e.value("key", "");
            std::string value = e.value("value", "");
            std::string format = e.value("format", "plain");
            if (!key.empty()) {
                setEntry(newId, locale, key, value, format);
            }
        }
    }

    DICE_LOG_INFO("PersonaManager: imported persona '{}' (id={})", name, newId);
    return newId;
}

}  // namespace dice
