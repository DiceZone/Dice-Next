#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace dice::markdown {
namespace {

bool isBoundary(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isspace(u) || std::ispunct(u);
}

void stripPairs(std::string& text, const std::string& marker) {
    size_t from = 0;
    while (true) {
        const size_t open = text.find(marker, from);
        if (open == std::string::npos) break;
        const size_t close = text.find(marker, open + marker.size());
        if (close == std::string::npos || close == open + marker.size()) break;
        text.erase(close, marker.size());
        text.erase(open, marker.size());
        from = close - marker.size();
    }
}

void stripSingleEmphasis(std::string& text, char marker) {
    size_t from = 0;
    while (from < text.size()) {
        const size_t open = text.find(marker, from);
        if (open == std::string::npos) break;
        const bool leftBoundary = open == 0 || isBoundary(text[open - 1]);
        if (!leftBoundary || open + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[open + 1]))) {
            from = open + 1;
            continue;
        }
        const size_t close = text.find(marker, open + 1);
        if (close == std::string::npos || close == open + 1) break;
        const bool rightBoundary = close + 1 == text.size() || isBoundary(text[close + 1]);
        if (!rightBoundary || std::isspace(static_cast<unsigned char>(text[close - 1]))) {
            from = close + 1;
            continue;
        }
        text.erase(close, 1);
        text.erase(open, 1);
        from = close - 1;
    }
}

std::string stripLinks(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        const bool image = input[i] == '!' && i + 1 < input.size() && input[i + 1] == '[';
        const size_t labelStart = image ? i + 2 : (input[i] == '[' ? i + 1 : std::string::npos);
        if (labelStart != std::string::npos) {
            const size_t labelEnd = input.find("](", labelStart);
            const size_t urlEnd = labelEnd == std::string::npos ? std::string::npos : input.find(')', labelEnd + 2);
            if (labelEnd != std::string::npos && urlEnd != std::string::npos) {
                const std::string label = input.substr(labelStart, labelEnd - labelStart);
                const std::string url = input.substr(labelEnd + 2, urlEnd - labelEnd - 2);
                if (!label.empty()) out += label;
                if (!url.empty() && (label.empty() || label != url)) {
                    if (!label.empty()) out += " (";
                    out += url;
                    if (!label.empty()) out += ')';
                }
                i = urlEnd + 1;
                continue;
            }
        }
        out += input[i++];
    }
    return out;
}

std::string stripLinePrefix(const std::string& line) {
    size_t pos = 0;
    while (pos < line.size() && pos < 3 && line[pos] == ' ') ++pos;
    if (line.compare(pos, 3, "```") == 0 || line.compare(pos, 3, "~~~") == 0) return {};
    if (pos < line.size() && line[pos] == '#') {
        size_t end = pos;
        while (end < line.size() && line[end] == '#') ++end;
        if (end - pos <= 6 && end < line.size() && line[end] == ' ') return line.substr(end + 1);
    }
    if (pos < line.size() && line[pos] == '>') {
        ++pos;
        if (pos < line.size() && line[pos] == ' ') ++pos;
        return line.substr(pos);
    }
    if (pos + 1 < line.size() && (line[pos] == '-' || line[pos] == '*' || line[pos] == '+') && line[pos + 1] == ' ') {
        size_t content = pos + 2;
        if (content + 2 < line.size() && line[content] == '[' &&
            (line[content + 1] == 'x' || line[content + 1] == 'X' || line[content + 1] == ' ') && line[content + 2] == ']') {
            const bool checked = line[content + 1] != ' ';
            content += 3;
            if (content < line.size() && line[content] == ' ') ++content;
            return std::string(checked ? "[x] " : "[ ] ") + line.substr(content);
        }
        return "- " + line.substr(content);
    }
    return line;
}

} // namespace

bool hasFormatting(const std::string& text) {
    if (text.find("**") != std::string::npos || text.find("__") != std::string::npos ||
        text.find("~~") != std::string::npos || text.find('`') != std::string::npos ||
        text.find("](") != std::string::npos) return true;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        size_t p = 0;
        while (p < line.size() && p < 3 && line[p] == ' ') ++p;
        if (p < line.size() && (line[p] == '#' || line[p] == '>')) return true;
        if (p + 1 < line.size() && (line[p] == '-' || line[p] == '*' || line[p] == '+') && line[p + 1] == ' ') return true;
    }
    return false;
}

std::string toPlainText(const std::string& markdownText) {
    std::istringstream lines(markdownText);
    std::vector<std::string> plainLines;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = stripLinePrefix(line);
        line = stripLinks(line);
        stripPairs(line, "**");
        stripPairs(line, "__");
        stripPairs(line, "~~");
        stripPairs(line, "||");
        stripPairs(line, "`");
        stripSingleEmphasis(line, '*');
        stripSingleEmphasis(line, '_');

        std::string unescaped;
        unescaped.reserve(line.size());
        static const std::string escapable = R"(\`*_{}[]()#+-.!>|~)";
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size() && escapable.find(line[i + 1]) != std::string::npos)
                unescaped += line[++i];
            else
                unescaped += line[i];
        }
        plainLines.push_back(std::move(unescaped));
    }

    while (!plainLines.empty() && plainLines.front().empty()) plainLines.erase(plainLines.begin());
    while (!plainLines.empty() && plainLines.back().empty()) plainLines.pop_back();
    std::string out;
    bool previousBlank = false;
    for (const auto& value : plainLines) {
        const bool blank = value.empty();
        if (blank && previousBlank) continue;
        if (!out.empty()) out += '\n';
        out += value;
        previousBlank = blank;
    }
    return out;
}

} // namespace dice::markdown
