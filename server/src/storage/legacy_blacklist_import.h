#pragma once
#include "database.h"
#include "legacy_reply_import_safety.h"
#include "../common/utils.h"

namespace dice::legacyv2 {
inline std::string utf8Truncate(const std::string&, size_t);
inline std::string sanitizeJsonControls(const std::string&);

inline int importBlacklistSafe(Database& db, const std::filesystem::path& confDir,
                               nlohmann::json* diagnostics = nullptr,
                               const std::set<std::string>& localSourceIds = {}) {
    using json = nlohmann::json;
    using reply_import_safety::diagnostic;
    const auto body = reply_import_safety::readSource(confDir / "BlackList.json");
    if (body.empty()) return 0;
    auto* st = db.getStorage();
    if (!st) throw std::runtime_error("blacklist database is not open");
    const auto arr = json::parse(sanitizeJsonControls(body));
    if (!arr.is_array()) throw std::runtime_error("BlackList.json must be an array");
    struct Mark {
        std::string type, time, source, note;
        std::string ids[4]; bool active[4]{};
        int danger = 0; long long wid = 0; bool local = false;
        json original;
    };
    auto id = [](const json& entry, const char* field, const char* alias = nullptr) -> std::string {
        const auto it = entry.find(field);
        const auto value = it != entry.end() ? it : alias ? entry.find(alias) : entry.end();
        if (value == entry.end() || value->is_null()) return {};
        std::string text;
        if (value->is_number_integer()) { const auto n = value->get<long long>(); if (n <= 0) return {}; text = std::to_string(n); }
        else if (value->is_string()) text = reply_import_safety::trim(value->get<std::string>());
        else throw std::runtime_error(std::string("invalid blacklist identifier: ") + field);
        if (text.empty()) return {};
        if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            throw std::runtime_error(std::string("invalid blacklist identifier: ") + field);
        const auto first = text.find_first_not_of('0');
        return first == std::string::npos ? std::string() : text.substr(first);
    };
    auto same = [](const Mark& a, const Mark& b) {
        if (a.wid && b.wid) return a.wid == b.wid;
        if (a.type != b.type && a.type != "null" && b.type != "null") return false;
        bool overlap = false;
        if (!a.time.empty() && !b.time.empty()) { if (a.time != b.time) return false; overlap = true; }
        for (int i = 0; i < 4; ++i) if (!a.ids[i].empty() && !b.ids[i].empty()) {
            if (a.ids[i] != b.ids[i]) return false;
            overlap = true;
        }
        if (!a.source.empty() && !b.source.empty() && a.source != b.source) return false;
        return overlap || a.note == b.note;
    };
    std::vector<Mark> marks;
    const std::set<std::string> knownTypes{"null", "local", "ban", "kick", "spam", "ruler", "other", "invite", "extern", "erase"};
    for (size_t index = 0; index < arr.size(); ++index) {
        const auto& entry = arr[index];
        const auto name = std::to_string(index);
        if (!entry.is_object()) { diagnostic(diagnostics, "BlackList.json", name, "failed", {"blacklist record is not an object"}, entry); continue; }
        try {
            Mark mark; mark.original = entry;
            mark.type = entry.value("type", std::string("null"));
            if (!knownTypes.count(mark.type)) { diagnostic(diagnostics, "BlackList.json", name, "skipped", {"unknown legacy blacklist type"}, entry); continue; }
            bool erased = mark.type == "erase";
            if (entry.contains("isErased")) {
                if (entry["isErased"].is_boolean()) erased = erased || entry["isErased"].get<bool>();
                else if (entry["isErased"].is_number_integer()) erased = erased || entry["isErased"].get<int>() != 0;
                else throw std::runtime_error("invalid isErased flag");
            }
            if (mark.type == "erase") mark.type = "null";
            mark.danger = entry.value("danger", mark.type == "ruler" ? 3 : 2);
            if (mark.danger < 1) { diagnostic(diagnostics, "BlackList.json", name, "ignored", {"inactive/invalid danger level"}); continue; }
            mark.wid = entry.value("wid", 0LL);
            mark.source = id(entry, "DiceMaid");
            const auto master = id(entry, "masterQQ", "master");
            // "local" is an explicit local decision. Old-format records without
            // any source/cloud identity are local history too; otherwise the
            // caller must supply identities proven to belong to the OLD dice.
            mark.local = mark.type == "local" || (!mark.wid && mark.source.empty() && master.empty() && mark.type != "extern")
                || ((!mark.source.empty() && localSourceIds.count(mark.source)) || (!master.empty() && localSourceIds.count(master)));
            if (mark.type == "extern") mark.local = false;
            mark.time = entry.value("time", std::string());
            mark.note = entry.value("note", entry.value("comment", std::string("legacy import")));
            mark.ids[0] = id(entry, "fromUID", "fromQQ");
            mark.ids[1] = id(entry, "fromGID", "fromGroup");
            mark.ids[2] = id(entry, "inviterQQ", "inviter");
            mark.ids[3] = id(entry, "ownerQQ");
            for (int i = 0; i < 4; ++i) mark.active[i] = !erased && !mark.ids[i].empty();
            if (entry.contains("erase")) {
                if (!entry["erase"].is_array()) throw std::runtime_error("invalid erase list");
                for (const auto& field : entry["erase"]) {
                    const auto key = field.get<std::string>();
                    if (key == "fromUID" || key == "fromQQ") mark.active[0] = false;
                    else if (key == "fromGID" || key == "fromGroup") mark.active[1] = false;
                    else if (key == "inviterQQ" || key == "inviter") mark.active[2] = false;
                    else if (key == "ownerQQ") mark.active[3] = false;
                }
            }
            const auto found = std::find_if(marks.begin(), marks.end(), [&](const auto& old) { return same(old, mark); });
            if (found != marks.end()) {
                // Later updates revoke the same record, not every independent
                // record concerning the user. This matches old loadJson/find.
                for (int i = 0; i < 4; ++i) if (!mark.ids[i].empty()) { found->ids[i] = mark.ids[i]; found->active[i] = mark.active[i]; }
                found->danger = mark.danger; found->original = entry;
                if (mark.type != "null") { found->type = mark.type; found->local = mark.local; }
                if (!mark.note.empty()) found->note = mark.note;
            } else marks.push_back(std::move(mark));
        } catch (const std::exception& error) {
            diagnostic(diagnostics, "BlackList.json", name, "failed", {error.what()}, entry);
            if (!diagnostics) throw;
        }
    }
    std::map<std::pair<int, std::string>, std::string> targets;
    for (size_t index = 0; index < marks.size(); ++index) {
        const auto& mark = marks[index];
        bool active = false;
        for (int i = 0; i < 4; ++i) active = active || mark.active[i];
        if (!active) { diagnostic(diagnostics, "BlackList.json", std::to_string(index), "ignored", {"all blacklist targets have been revoked"}); continue; }
        if (!mark.local) { diagnostic(diagnostics, "BlackList.json", std::to_string(index), "skipped", {"external/cloud or unattributed source is not converted into a local punishment"}, mark.original); continue; }
        if (mark.active[0]) targets[{0, mark.ids[0]}] = mark.note;
        // These penalties depend on old cleanup/credit/exemption policies.
        if (mark.active[1] || mark.active[2] || mark.active[3])
            diagnostic(diagnostics, "BlackList.json", std::to_string(index), "preserved",
                {"群及关联人记录需核对旧免黑/信任策略；未自动转为新版封禁"}, mark.original);
    }
    int imported = 0;
    st->transaction([&] {
        for (const auto& [target, note] : targets) {
            const auto& [type, targetId] = target;
            const auto count = st->count<BanlistRow>(sqlite_orm::where(sqlite_orm::c(&BanlistRow::targetType) == type
                and sqlite_orm::c(&BanlistRow::listType) == 0 and sqlite_orm::c(&BanlistRow::targetId) == targetId));
            if (count > 0) continue; // Never replace existing Next owner decisions.
            BanlistRow row; row.targetType = type; row.listType = 0; row.targetId = targetId;
            row.reason = utf8Truncate(note, 200); row.createdAt = utils::nowIso8601();
            st->insert(row); ++imported;
        }
        return true;
    });
    return imported;
}
} // namespace dice::legacyv2
