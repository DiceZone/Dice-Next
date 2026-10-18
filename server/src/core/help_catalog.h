#pragma once
#include "help_entry_filter.h"
#include <algorithm>
#include <map>
#include <vector>

namespace dice {
inline std::string helpSourceKind(const std::string& source) {
    if (source == "builtin") return "builtin";
    if (source == "custom" || source.rfind("file:", 0) == 0) return "custom";
    if (source == "plugin" || source.rfind("plugin:", 0) == 0 || source.rfind("lua:", 0) == 0) return "plugin";
    if (source.rfind("rule:", 0) == 0) return "rule";
    if (source.rfind("helpdoc:", 0) == 0) return "helpdoc";
    return "other";
}

struct HelpDocumentEntry {
    std::string key, content, source, i18nKey, locale, format = "plain";
    bool editable = false, customized = false, shadowed = false;
    std::string kind; // Explicit origin for document packs embedded in a rule pack.
};

/// Shared by the paged library and source counters. Visibility is decided
/// before filtering, so filtering a shadowed source never makes it effective.
class HelpCatalog {
public:
    explicit HelpCatalog(bool management) : visibility_(management) {}
    static std::string kindOf(const HelpDocumentEntry& entry) {
        return entry.kind.empty() ? helpSourceKind(entry.source) : entry.kind;
    }
    void add(HelpDocumentEntry entry) {
        const auto decision = visibility_.add(entry.key, entry.content);
        if (!decision.include) return;
        entry.shadowed = decision.shadowed;
        entries_.push_back(std::move(entry));
    }
    std::vector<const HelpDocumentEntry*> select(const std::string& query = "",
            const std::string& kind = "", const std::string& source = "") const {
        std::vector<const HelpDocumentEntry*> result;
        for (const auto& entry : entries_) {
            if ((!kind.empty() && kindOf(entry) != kind) ||
                (!source.empty() && entry.source != source)) continue;
            if (contains(entry.key, query) || contains(entry.content, query)) result.push_back(&entry);
        }
        return result;
    }
private:
    static char lower(unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? char(ch + 32) : char(ch); }
    static bool contains(const std::string& text, const std::string& query) {
        if (query.empty()) return true;
        return std::search(text.begin(), text.end(), query.begin(), query.end(),
            [](char a, char b) { return lower(a) == lower(b); }) != text.end();
    }
    HelpEntryFilter visibility_;
    std::vector<HelpDocumentEntry> entries_;
};
}
