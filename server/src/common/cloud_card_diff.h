#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dice::cloud_cards {
using DiffJson = nlohmann::json;
struct Difference {
    std::string path;
    std::optional<DiffJson> base, local, remote;
    enum class Kind { Local, Remote, Same, Conflict } kind;
};
inline void compareDocumentValues(const std::optional<DiffJson>& base, const std::optional<DiffJson>& local,
                                  const std::optional<DiffJson>& remote, const std::string& path,
                                  std::vector<Difference>& out) {
    if (base == local && base == remote) return;
    // Descend only when all three versions retain the object. Deleting or
    // replacing a parent while the other side edits a child is one conflict,
    // not two falsely independent field edits. Arrays remain atomic.
    if (base && local && remote && base->is_object() && local->is_object() && remote->is_object()) {
        std::set<std::string> keys;
        for (const auto* object : {&*base, &*local, &*remote})
            for (auto it = object->begin(); it != object->end(); ++it) keys.insert(it.key());
        const auto get = [](const DiffJson& object, const std::string& key) -> std::optional<DiffJson> {
            const auto it = object.find(key); return it == object.end() ? std::nullopt : std::optional<DiffJson>(*it);
        };
        for (const auto& key : keys) {
            if (path.empty() && (key == "schema_version" || key == "card_id" || key == "rev")) continue;
            std::string escaped;
            for (char ch : key) escaped += ch == '~' ? "~0" : ch == '/' ? "~1" : std::string(1, ch);
            compareDocumentValues(get(*base, key), get(*local, key), get(*remote, key), path + "/" + escaped, out);
        }
        return;
    }
    const auto kind = local == remote ? Difference::Kind::Same : base == local ? Difference::Kind::Remote
        : base == remote ? Difference::Kind::Local : Difference::Kind::Conflict;
    out.push_back({path, base, local, remote, kind});
}
inline std::vector<Difference> compareDocuments(const DiffJson& base, const DiffJson& local, const DiffJson& remote) {
    std::vector<Difference> out;
    compareDocumentValues(base, local, remote, "", out);
    return out;
}
inline std::string diffValue(const std::optional<DiffJson>& value) {
    if (!value) return "(missing)";
    auto text = value->dump();
    if (text.size() > 256) return "(long value; compare a separate copy)";
    // A card value must not become an executable CQ/media segment in a receipt.
    std::string safe;
    for (char ch : text) safe += ch == '[' ? "［" : ch == ']' ? "］" : std::string(1, ch);
    return safe;
}
inline std::string differenceText(const std::vector<Difference>& differences) {
    std::string text;
    size_t count = 0;
    for (const auto& entry : differences) {
        if (++count > 30) { text += "…\n"; break; }
        const char* kind = entry.kind == Difference::Kind::Conflict ? "conflict" : entry.kind == Difference::Kind::Local ? "local"
            : entry.kind == Difference::Kind::Remote ? "remote" : "same";
        auto path = entry.path;
        if (path.size() > 256) path = "(long field path)";
        // Paths are user-defined too; avoid media code injection in them.
        path = diffValue(DiffJson(path));
        text += path + " [" + kind + "]\n  " + diffValue(entry.base) + " | " + diffValue(entry.local) + " | " + diffValue(entry.remote) + "\n";
    }
    return text.empty() ? "(0)" : text;
}
} // namespace dice::cloud_cards
