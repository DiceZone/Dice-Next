#pragma once
#include "presentation_style.h"

namespace dice::qq_table {
using Row = std::vector<std::string>;
inline std::string_view trim(std::string_view s) {
    auto a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == s.npos ? std::string_view{} : s.substr(a, b - a + 1);
}
// The toolbar emits outer pipes. Keep escaped delimiters in the original cell.
inline Row cells(std::string_view line) {
    line = trim(line);
    if (line.size() < 2 || line.front() != '|' || line.back() != '|') return {};
    Row row;
    size_t start = 1;
    for (size_t i = 1; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) { ++i; continue; }
        if (line[i] != '|') continue;
        row.emplace_back(trim(line.substr(start, i - start)));
        start = i + 1;
    }
    return start == line.size() ? row : Row{};
}
inline bool divider(const Row& row, size_t width) {
    if (row.size() != width || !width) return false;
    for (std::string_view cell : row) {
        if (cell.starts_with(':')) cell.remove_prefix(1);
        if (cell.ends_with(':')) cell.remove_suffix(1);
        if (cell.size() < 3 || cell.find_first_not_of('-') != cell.npos) return false;
    }
    return true;
}
inline std::string render(const std::string& text, bool math) {
    if (text.find('|') == text.npos || text.find("---") == text.npos) return text;
    std::vector<std::string_view> lines;
    const std::string_view source(text);
    for (size_t at = 0;;) {
        const auto end = source.find('\n', at);
        lines.push_back(source.substr(at, end == source.npos ? end : end - at));
        if (end == source.npos) break;
        at = end + 1;
    }
    char fence = 0;
    size_t fenceSize = 0;
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        const auto line = trim(lines[i]);
        if (!line.empty() && (line.front() == '\x60' || line.front() == '~')) {
            auto count = line.find_first_not_of(line.front());
            if (count == line.npos) count = line.size();
            if (!fence && count >= 3) { fence = line.front(); fenceSize = count; }
            else if (fence == line.front() && count >= fenceSize && count == line.size()) fence = 0;
            out += lines[i]; continue;
        }
        Row header;
        if (fence || i + 1 == lines.size() || (header = cells(lines[i])).empty() ||
            !divider(cells(lines[i + 1]), header.size())) {
            out += lines[i]; continue;
        }
        const auto width = header.size();
        std::vector<Row> rows{std::move(header)};
        for (++i; i + 1 < lines.size(); ++i) {
            auto row = cells(lines[i + 1]);
            if (row.empty()) break;
            rows.push_back(std::move(row));
        }
        // Large or irregular tables use readable rows rather than oversized math.
        bool compact = math && width <= 4 && rows.size() <= 12;
        for (auto& row : rows) {
            if (row.size() != width) compact = false;
            for (auto& cell : row) {
                cell = markdown::toPlainText(cell);
                // Cells are display text, not native QQ tags. Keep the visible
                // angle bracket but do not recreate a tag when unescaping it.
                for (size_t pos = 0; (pos = cell.find('<', pos)) != cell.npos; pos += 4)
                    cell.insert(pos + 1, "\xE2\x80\x8B");
                if (cell.size() > 96) compact = false;
            }
        }
        if (compact) out += "$\\begin{array}{" + std::string(width, 'l') + "}";
        for (size_t r = 0; r < rows.size(); ++r) {
            if (r) out += compact ? (r == 1 ? R"(\\ \hline )" : R"(\\ )") : "  \n";
            for (size_t c = 0; c < rows[r].size(); ++c) {
                if (c) out += compact ? " & " : " · ";
                out += compact ? presentation::mathLiteral(rows[r][c]) : markdown::escapeQQMarkdownLiteral(rows[r][c]);
            }
        }
        if (compact) out += "\\end{array}$";
    }
    return out;
}
} // namespace dice::qq_table
