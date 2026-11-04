#pragma once
// Turn a plain-text reply template into Markdown source that shows the same
// text. Template syntax ({variables}, {sample:...}, [[components]]) is kept as
// written; only the literal text around it receives the minimal escapes, so
// "2*3", "a_b" and ordinary punctuation stay readable in the editor.
#include "markdown.h"
#include "weighted_templates.h"
#include <string>
#include <string_view>

namespace dice::template_markdown {

inline std::string escapeLiterals(std::string_view source) {
    std::string out, literal;
    out.reserve(source.size() + source.size() / 16);
    auto flush = [&] { if (!literal.empty()) { out += markdown::escapeQQMarkdownLiteral(literal); literal.clear(); } };
    for (size_t i = 0; i < source.size();) {
        const char ch = source[i];
        // An escaped brace is literal template text; keep the escape as written.
        if (ch == '\\' && i + 1 < source.size() && (source[i + 1] == '{' || source[i + 1] == '}')) {
            flush(); out.append(source.substr(i, 2)); i += 2; continue;
        }
        if (ch == '[' && source.substr(i, 2) == "[[") {
            const auto close = source.find("]]", i + 2);
            if (close != std::string_view::npos) { flush(); out.append(source.substr(i, close + 2 - i)); i = close + 2; continue; }
        }
        if (ch == '{') {
            size_t depth = 0, end = i;
            for (; end < source.size(); ++end) {
                if (source[end] == '\\' && end + 1 < source.size()) { ++end; continue; }
                if (source[end] == '{') ++depth;
                else if (source[end] == '}' && --depth == 0) break;
            }
            if (end < source.size()) { flush(); out.append(source.substr(i, end + 1 - i)); i = end + 1; continue; }
        }
        literal += ch; ++i;
    }
    flush();
    return out;
}

/// Stored override value (single text or weighted variants) converted for Markdown.
inline std::string convert(const std::string& stored) {
    auto variants = weighted_templates::decode(stored);
    if (variants.empty()) return escapeLiterals(stored);
    for (auto& item : variants) item["text"] = escapeLiterals(item["text"].get<std::string>());
    return weighted_templates::encode(variants);
}

} // namespace dice::template_markdown
