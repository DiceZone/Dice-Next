#pragma once
#include "model_formula.h"
#include <filesystem>
#include <map>
#include <set>
#include <fstream>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dice::legacy_model {
namespace fs = std::filesystem;
inline std::string utf8(const fs::path& path) { const auto text = path.u8string(); return {text.begin(), text.end()}; }
inline std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
inline std::string decode(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') { out += raw[i]; continue; }
        const auto end = raw.find(';', i + 1);
        if (end == std::string::npos || end - i > 16) { out += raw[i]; continue; }
        const auto entity = raw.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&'; else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>'; else if (entity == "quot") out += '"'; else if (entity == "apos") out += '\'';
        else if (!entity.empty() && entity[0] == '#') {
            try {
                const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X'); size_t used = 0;
                const auto digits = entity.substr(hex ? 2 : 1);
                const auto code = std::stoul(digits, &used, hex ? 16 : 10);
                if (used != digits.size() || code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) throw std::runtime_error("invalid entity");
                if (code < 0x80) out += static_cast<char>(code);
                else if (code < 0x800) { out += static_cast<char>(0xc0 | code >> 6); out += static_cast<char>(0x80 | (code & 63)); }
                else if (code < 0x10000) { out += static_cast<char>(0xe0 | code >> 12); out += static_cast<char>(0x80 | (code >> 6 & 63)); out += static_cast<char>(0x80 | (code & 63)); }
                else { out += static_cast<char>(0xf0 | code >> 18); out += static_cast<char>(0x80 | (code >> 12 & 63)); out += static_cast<char>(0x80 | (code >> 6 & 63)); out += static_cast<char>(0x80 | (code & 63)); }
            } catch (...) { out += raw.substr(i, end - i + 1); }
        } else { out += raw.substr(i, end - i + 1); }
        i = end;
    }
    return out;
}
inline std::string expression(std::string raw) {
    raw = trim(raw);
    if (raw.rfind("<![CDATA[", 0) == 0 && raw.ends_with("]]>")) raw = raw.substr(9, raw.size() - 12);
    else raw = decode(raw);
    raw = trim(raw);
    if (raw.rfind("return ", 0) == 0) raw = trim(raw.substr(7));
    if (!raw.empty() && raw.back() == ';') raw = trim(raw.substr(0, raw.size() - 1));
    return model_formula::supported(raw) ? raw : std::string();
}
struct Property { std::string name, alias, language, raw, formula; };
struct Document { std::vector<Property> properties; std::vector<std::string> warnings; bool valid = true; };
// Read only <property><any ...> data. Old model bodies are often not strict XML
// (unescaped JS &&/comparisons); do not route them through an executable engine.
inline Document parse(const std::string& text) {
    Document result;
    if (text.size() > 4 * 1024 * 1024 || text.find("<!DOCTYPE") != std::string::npos || text.find("<!ENTITY") != std::string::npos)
        return {{}, {"模型过大或含不允许的 DTD/实体声明"}, false};
    bool inProperties = false;
    for (size_t pos = 0; pos < text.size() && result.properties.size() < 10000;) {
        pos = text.find('<', pos); if (pos == std::string::npos) break;
        if (text.compare(pos, 4, "<!--") == 0) {
            const auto close = text.find("-->", pos + 4);
            if (close == std::string::npos) { result.valid = false; break; }
            pos = close + 3; continue;
        }
        bool quote = false; char quoteChar = 0; size_t end = pos + 1;
        for (; end < text.size(); ++end) {
            const char c = text[end];
            if (quote) { if (c == quoteChar) quote = false; }
            else if (c == '\'' || c == '"') { quote = true; quoteChar = c; }
            else if (c == '>') break;
        }
        if (end == text.size() || end - pos > 65536) { result.valid = false; break; }
        const auto tag = text.substr(pos + 1, end - pos - 1);
        const auto split = tag.find_first_of(" \t\r\n/");
        const auto name = tag.substr(0, split);
        if (tag == "/property") inProperties = false;
        if (name == "property") inProperties = true;
        if (name == "script") {
            result.warnings.push_back("模型 script 不执行；仅转换可验证的数值表达式");
            const auto close = text.find("</script>", end + 1);
            if (close == std::string::npos) { result.valid = false; break; }
            pos = close + 9; continue;
        }
        if (!inProperties || name != "any") { pos = end + 1; continue; }
        std::map<std::string, std::string> attributes;
        size_t cursor = name.size();
        while (cursor < tag.size()) {
            while (cursor < tag.size() && std::string_view(" \t\r\n/").find(tag[cursor]) != std::string_view::npos) ++cursor;
            if (cursor == tag.size()) break;
            const auto start = cursor;
            while (cursor < tag.size() && tag[cursor] != '=' && std::string_view(" \t\r\n").find(tag[cursor]) == std::string_view::npos) ++cursor;
            const auto key = tag.substr(start, cursor - start);
            while (cursor < tag.size() && std::string_view(" \t\r\n").find(tag[cursor]) != std::string_view::npos) ++cursor;
            if (cursor == tag.size() || tag[cursor++] != '=') { result.valid = false; break; }
            while (cursor < tag.size() && std::string_view(" \t\r\n").find(tag[cursor]) != std::string_view::npos) ++cursor;
            if (cursor == tag.size() || (tag[cursor] != '\'' && tag[cursor] != '"')) { result.valid = false; break; }
            const auto delimiter = tag[cursor++]; const auto close = tag.find(delimiter, cursor);
            if (close == std::string::npos || attributes.count(key)) { result.valid = false; break; }
            attributes[key] = decode(tag.substr(cursor, close - cursor)); cursor = close + 1;
        }
        if (!result.valid) break;
        Property property; property.name = attributes["name"]; property.alias = attributes["alias"]; property.language = attributes["text"];
        if (tag.empty() || tag.back() != '/') {
            const auto close = text.find("</any>", end + 1);
            if (close == std::string::npos) { result.valid = false; break; }
            property.raw = text.substr(end + 1, close - end - 1); pos = close + 6;
        } else pos = end + 1;
        if (property.name.empty()) { result.warnings.push_back("无 name 的属性已跳过"); continue; }
        if (!property.raw.empty() && (property.language == "javascript" || property.language == "dicexp")) {
            property.formula = expression(property.raw);
            if (property.formula.empty()) result.warnings.push_back("属性「" + property.name + "」的默认/派生值不是安全数值表达式，未转换");
        }
        result.properties.push_back(std::move(property));
    }
    if (!result.valid) { result.properties.clear(); result.warnings.push_back("模型属性声明损坏，已跳过该文件"); }
    return result;
}
inline bool linked(const fs::path& path) {
    std::error_code error;
    if (fs::is_symlink(path, error)) return true;
#ifdef _WIN32
    const auto attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
inline std::vector<fs::path> orderedFiles(const fs::path& root) {
    std::vector<fs::path> files; std::error_code error;
    if (!fs::is_directory(root, error) || linked(root)) return files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         it != end && files.size() < 10000; it.increment(error)) {
        if (error) { error.clear(); continue; }
        const auto path = it->path(); const auto name = utf8(path.filename());
        if (linked(path) || name.ends_with(".disabled") || it.depth() > 16) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(error)) {
            if (fs::exists(path.parent_path() / fs::path(path.filename()).concat(".json.disabled"), error)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(error) || path.extension() != ".xml") continue;
        for (auto parent = path.parent_path(); parent != root && !parent.empty(); parent = parent.parent_path())
            if (parent.filename() == "model") { files.push_back(path); break; }
    }
    std::map<std::string, size_t> order;
    std::ifstream input(root.parent_path() / "mod_order.json", std::ios::binary);
    if (input) { const auto data = nlohmann::json::parse(input, nullptr, false);
        if (data.is_array()) for (const auto& name : data) if (name.is_string() && !order.count(name.get<std::string>())) order[name.get<std::string>()] = order.size(); }
    auto owner = [&](const fs::path& file) {
        std::string result;
        for (const auto& part : file.lexically_relative(root)) {
            if (part == "model") break;
            if (!result.empty()) result += '.';
            result += utf8(part);
        }
        return result;
    };
    std::stable_sort(files.begin(), files.end(), [&](const auto& a, const auto& b) {
        const auto an = owner(a), bn = owner(b); const auto ai = order.find(an), bi = order.find(bn);
        const auto ar = ai == order.end() ? static_cast<size_t>(-1) : ai->second, br = bi == order.end() ? static_cast<size_t>(-1) : bi->second;
        return ar != br ? ar < br : an != bn ? an < bn : a < b;
    });
    return files;
}
} // namespace dice::legacy_model
