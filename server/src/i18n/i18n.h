#pragma once
// ─── Dice!Next v3.0.0 — i18n Translation Engine ──────────────
// Loads per-locale JSON resource bundles and resolves message
// keys into localized strings with {placeholder} interpolation.
//
// Design goals:
//   - One place to define text, translated everywhere ("一处定义，处处翻译")
//   - Graceful fallback: requested locale → default locale → key itself
//   - Thread-safe (replies are produced on adapter/network threads)
//   - Hot-reloadable (resource files can be edited at runtime)
//
// Resource layout (one file per locale, BCP-47 code as filename):
//   <resourceDir>/zh-Hant.json
//   <resourceDir>/zh-Hans.json
//   <resourceDir>/en.json
//
// Keys are nested and addressed with '.' as separator, e.g.
//   tr(Locale::kEn, "dice.error.roll", {{"error", "bad expr"}})
// looks up  { "dice": { "error": { "roll": "Roll error: {error}" } } }.

#include "../common/content_format.h"
#include "../common/types.h"

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <optional>
#include <utility>
#include <nlohmann/json.hpp>

namespace dice {

using json = nlohmann::json;

class I18n {
public:
    /// Interpolation arguments: {placeholder name → value}.
    using Args = std::map<std::string, std::string>;

    /// @param resourceDir  Directory containing the <locale>.json files.
    /// @param defaultLocale Fallback locale when a key/locale is missing.
    explicit I18n(std::string resourceDir,
                  Locale defaultLocale = Locale::kZhHans);

    // ─── Lifecycle ───────────────────────────────────────────

    /// Load every supported locale bundle from disk.
    /// Missing files are skipped with a warning (not fatal).
    /// Returns true if at least one bundle loaded.
    bool load();

    /// Re-read all bundles from disk (hot-reload entry point).
    bool reload();

    // ─── Translation ─────────────────────────────────────────

    /// Translate @p key for @p loc, substituting {placeholders}
    /// from @p args. Falls back to the default locale, then to the
    /// raw key if no translation exists.
    std::string tr(Locale loc,
                   const std::string& key,
                   const Args& args = {}) const;

    /// Convenience overload accepting a locale code string.
    std::string tr(const std::string& localeCode,
                   const std::string& key,
                   const Args& args = {}) const {
        return tr(localeFromString(localeCode), key, args);
    }

    // ─── Configuration ───────────────────────────────────────

    void setDefaultLocale(Locale loc);
    Locale defaultLocale() const;

    /// Locales that successfully loaded a bundle.
    std::vector<Locale> availableLocales() const;

    // ─── 自定义翻译文件 ─────────────────────────────────
    // i18n/ 目录下所有 <code>.json 都会被加载（内置四语之外的注册为动态
    // Locale）。文件可带 `_meta` 块自定义显示名与 .lang 切换关键词：
    //   { "_meta": { "name": "한국어", "keywords": ["ko","korean","韩语"] }, ... }

    /// Match a .lang keyword (from any bundle's _meta.keywords, case-insensitive
    /// for ASCII) to its locale. std::nullopt if nothing matches.
    std::optional<Locale> localeForKeyword(const std::string& input) const;

    /// All loaded locales as (code, displayName). displayName = _meta.name
    /// → bundle's own lang.name → code.
    std::vector<std::pair<std::string, std::string>> listLocales() const;

    /// Display name for one locale (same fallback chain as listLocales).
    std::string localeDisplayName(Locale loc) const;

    /// Replace every {name} in @p tmpl with args.at("name"). Public so callers
    /// can interpolate text that isn't itself a translation key (e.g. data tables).
    static std::string interpolate(const std::string& tmpl, const Args& args);

    // ─── User overrides (editable reply templates) ───────────
    // An override replaces the bundle value for one (locale, key). The bundle
    // default always remains, so a reset = clear the override. Persisted by the
    // caller (DB); load them back via setOverride at startup.

    /// Set/replace the override for (loc, key).
    void setOverride(Locale loc, const std::string& key, const std::string& value,
                     ContentFormat format = ContentFormat::kPlainText);
    /// Remove the override for (loc, key) → falls back to the bundle default.
    void clearOverride(Locale loc, const std::string& key);
    /// Whether an override exists for (loc, key).
    bool hasOverride(Locale loc, const std::string& key) const;
    /// Explicit format of an override; absent overrides return plain text.
    ContentFormat getOverrideFormat(Locale loc, const std::string& key) const;
    /// The bundle DEFAULT for (loc, key), ignoring overrides (for edit/reset UI).
    std::string getDefault(Locale loc, const std::string& key) const;
    /// Format of a trusted built-in template. It is derived only from bundled
    /// project resources, never from user input or a rendered reply.
    ContentFormat getDefaultFormat(Locale loc, const std::string& key) const;

    /// Capture the richest template format used while one command builds a
    /// reply, without changing the command router's string-returning API.
    static void beginOutboundCapture(
        ContentFormat preferredOutput = ContentFormat::kMarkdown);
    static ContentFormat endOutboundCapture();

    /// Flatten the whole bundle for @p loc into {dotted-key → default value} for
    /// every string leaf (powers the "全部文本可自定义" editor). Overrides are NOT
    /// applied — the caller overlays them.
    std::map<std::string, std::string> flatten(Locale loc) const;

    // ─── Persona layer (骰娘人格切换) ─────────────────
    // A "persona" is an overlay bundle that sits between user overrides and the
    // default bundle in the lookup chain: override → persona(若启用) → bundle → key.
    // Persona data is injected by PersonaManager from the database (not files).

    /// Select a persona only for the current thread and lexical scope.  Message
    /// handling uses this instead of mutating the process-wide default, so two
    /// groups can render replies concurrently without leaking personas.
    class PersonaScope {
    public:
        explicit PersonaScope(int personaId);
        ~PersonaScope();
        PersonaScope(const PersonaScope&) = delete;
        PersonaScope& operator=(const PersonaScope&) = delete;

    private:
        std::optional<int> previous_;
    };

    [[nodiscard]] PersonaScope scopedPersona(int personaId) const {
        return PersonaScope(personaId);
    }

    /// Switch to a named persona (0 = default/disabled).
    void setPersona(int personaId);

    /// Current active persona ID (0 = no persona layer active).
    int getActivePersonaId() const;

    /// Load (or clear) the persona bundle. With DB-driven personas, this is
    /// a no-op — data is injected via setPersonaBundles() by PersonaManager.
    bool loadPersona();

    /// Cache one persona's bundles for a locale (called by PersonaManager).
    /// @p formats maps the same keys to "plain" or "markdown". Missing
    /// formats remain plain so existing personas keep their literal semantics.
    void setPersonaBundles(int personaId, Locale loc, const json& bundles,
                           const json& formats = json::object());

    /// Evict one persona from the cache (used before a reload or after deletion).
    void clearPersonaBundles(int personaId);

private:
    /// Look up a key in a single bundle. Returns nullptr if absent.
    /// Caller must hold mutex_.
    const json* lookupNode(Locale loc, const std::string& key) const;

    /// Look up a flat key in the persona bundle for @p loc.
    /// Persona bundles are flat {dotted-key → string}, so no nested traversal.
    /// Caller must hold mutex_.
    const json* lookupPersonaNode(Locale loc, const std::string& key) const;
    ContentFormat lookupPersonaFormat(Locale loc, const std::string& key) const;
    static ContentFormat trustedTemplateFormat(const std::string& value);
    static void noteOutboundFormat(ContentFormat format);
    struct TemplateValue {
        std::string value;
        std::string plainValue;
        ContentFormat format = ContentFormat::kPlainText;
    };
    static TemplateValue prepareTemplate(const std::string& value,
                                         ContentFormat format);
    static std::string renderTemplate(const TemplateValue& value,
                                      const Args& args);

    /// All locales this engine knows how to load.
    static std::vector<Locale> supportedLocales();

    std::string resourceDir_;
    Locale defaultLocale_;
    std::map<Locale, json> bundles_;
    std::map<Locale, std::map<std::string, TemplateValue>> preparedBundles_;
    std::map<Locale, std::map<std::string, TemplateValue>> overrides_;
    std::map<int, std::map<Locale, json>> personaBundles_;   // persona → locale → flat key/value
    std::map<int, std::map<Locale, json>> personaFormats_;   // persona → locale → flat key/format
    std::map<int, std::map<Locale, std::map<std::string, TemplateValue>>> preparedPersonaBundles_;
    int activePersonaId_ = 0;                   // current persona (0=default/off)
    std::map<std::string, Locale> keywordMap_;  // _meta.keywords → locale（键已转小写）
    mutable std::mutex mutex_;
    static thread_local bool outboundCaptureActive_;
    static thread_local bool outboundCaptureMarkdown_;
    static thread_local ContentFormat outboundPreferredOutput_;
    static thread_local std::optional<int> scopedPersonaId_;
};

}  // namespace dice
