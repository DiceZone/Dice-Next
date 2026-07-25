#pragma once
// ─── Dice!Next v3.0.0 — Persona Manager (C#28-B) ──────────────
// Manages named persona templates and their i18n override entries.
// Personas are DB-driven overlays that sit between user overrides and
// the default i18n bundle in the lookup chain:
//   override → persona(若激活) → bundle → key
//
// Supports per-group persona switching:
//   global  → dice_config key "persona/global"
//   group   → group_settings key "persona"

#include "../../storage/database.h"
#include "../../config/config_manager.h"
#include "../../i18n/i18n.h"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

class PersonaManager {
public:
    PersonaManager(Database& db, I18n& i18n, ConfigManager& cfg);

    // ─── Startup ──────────────────────────────────────────────

    /// Load the global persona at startup (if configured).
    void loadStartupPersona();

    // ─── Active persona resolution ────────────────────────────

    /// Get the active persona ID for a group (per-group > global > 0).
    /// @p groupId empty → returns global persona.
    int getActivePersona(const std::string& groupId = "") const;

    /// Set the active persona for a group (or global if groupId is empty).
    void setActivePersona(int personaId, const std::string& groupId = "");

    // ─── Template CRUD ────────────────────────────────────────

    /// List all persona templates.
    std::vector<PersonaTemplateRow> listTemplates() const;

    /// Get a template by ID. Returns empty-named row if not found.
    PersonaTemplateRow getTemplateById(int id) const;

    /// Get a template by name. Returns empty-named row if not found.
    PersonaTemplateRow getTemplateByName(const std::string& name) const;

    /// Create a new persona template. Returns the new ID (or -1 on failure).
    int createTemplate(const std::string& name, const std::string& description);

    /// Copy a persona template (including entries). Returns the new ID (or -1).
    int copyTemplate(int srcId, const std::string& newName);

    /// Delete a persona template (and its entries). Built-in cannot be deleted.
    bool deleteTemplate(int id);

    /// Update template metadata (name, description).
    bool updateTemplateMeta(int id, const std::string& name, const std::string& description);

    // ─── Entry CRUD ───────────────────────────────────────────

    /// List all entries for a persona.
    std::vector<PersonaEntryRow> listEntries(int personaId) const;

    /// Set or update an entry (upsert by persona_id + locale + key).
    bool setEntry(int personaId, const std::string& locale,
                  const std::string& key, const std::string& value);

    /// Delete an entry.
    bool deleteEntry(int personaId, const std::string& locale, const std::string& key);

    /// Count entries for a persona.
    int getEntryCount(int personaId) const;

    /// Get all distinct keys for a persona.
    std::vector<std::string> getEntryKeys(int personaId) const;

    // ─── I18n injection ───────────────────────────────────────

    /// Load persona entries from DB into I18n's personaBundles_ layer.
    /// If personaId is 0, clears the persona layer.
    void loadIntoI18n(int personaId);

    // ─── Import / Export ──────────────────────────────────────

    /// Export a persona template + entries as JSON.
    json exportTemplate(int id) const;

    /// Import a persona from JSON (expects exportTemplate format).
    /// Returns the new persona ID (or -1 on failure).
    int importTemplate(const json& data);

private:
    Database& db_;
    I18n& i18n_;
    ConfigManager& cfg_;

    /// Get current ISO 8601 timestamp.
    static std::string nowIso();
};

}  // namespace dice
