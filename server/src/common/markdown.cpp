#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace dice::markdown {
namespace {

bool isBoundary(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isspace(u) || std::ispunct(u);
}

std::string placeholder(size_t index) {
    return std::string(1, '\x1d') + std::to_string(index) + '\x1e';
}

bool protectedOneBotCode(const std::string& text, size_t pos) {
    return text.compare(pos, 4, "[CQ:") == 0 ||
           text.compare(pos, 5, "[img,") == 0 ||
           text.compare(pos, 8, "[图片:") == 0 ||
           text.compare(pos, 5, "[图:") == 0;
}

bool expressionMarkerAt(const std::string& text, size_t pos, std::string_view marker) {
    if (pos == 0 || pos + marker.size() >= text.size()) return false;
    const unsigned char before = static_cast<unsigned char>(text[pos - 1]);
    const unsigned char after = static_cast<unsigned char>(text[pos + marker.size()]);
    if (marker == "__") {
        // CommonMark does not treat an underscore run inside an identifier as
        // emphasis. Preserve names such as foo__bar during plain-text output.
        return std::isalnum(before) && std::isalnum(after);
    }
    if (marker == "**" || marker == "||") {
        // Keep numeric expression operators (2**3 / 1||0). Default reply
        // result expressions are normally inline-code protected as well, but
        // this makes the converter safe when called on raw expression text.
        const bool leftOperand = std::isdigit(before) || before == ')' || before == ']';
        const bool rightOperand = std::isdigit(after) || after == '(' || after == '[' ||
                                  after == '+' || after == '-';
        return leftOperand && rightOperand;
    }
    return false;
}

// Protect content that must survive the Markdown downgrade byte-for-byte:
// OneBot protocol codes, escaped punctuation and inline-code contents.
std::string protectInline(const std::string& input, std::vector<std::string>& protectedText) {
    static constexpr std::string_view kEscapable = R"(\`*_{}[]()#+-.!>|~)";
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        bool protectedMarker = false;
        for (const std::string_view marker : {std::string_view("**"), std::string_view("__"), std::string_view("||")}) {
            if (input.compare(i, marker.size(), marker) == 0 && expressionMarkerAt(input, i, marker)) {
                protectedText.emplace_back(marker);
                out += placeholder(protectedText.size() - 1);
                i += marker.size();
                protectedMarker = true;
                break;
            }
        }
        if (protectedMarker) continue;
        if (input[i] == '[' && protectedOneBotCode(input, i)) {
            const size_t end = input.find(']', i + 1);
            if (end != std::string::npos) {
                protectedText.push_back(input.substr(i, end - i + 1));
                out += placeholder(protectedText.size() - 1);
                i = end + 1;
                continue;
            }
        }
        if (input[i] == '\\' && i + 1 < input.size() &&
            kEscapable.find(input[i + 1]) != std::string_view::npos) {
            protectedText.emplace_back(1, input[i + 1]);
            out += placeholder(protectedText.size() - 1);
            i += 2;
            continue;
        }
        if (input[i] == '`') {
            size_t ticks = 1;
            while (i + ticks < input.size() && input[i + ticks] == '`') ++ticks;
            const std::string delimiter(ticks, '`');
            const size_t close = input.find(delimiter, i + ticks);
            if (close != std::string::npos) {
                std::string code = input.substr(i + ticks, close - i - ticks);
                // CommonMark strips one padding space on each side of a code
                // span (unless its content is spaces only). Mirror that rule
                // so spans whose literal content starts/ends with a backtick
                // downgrade to exactly the same visible text on OneBot.
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' &&
                    code.find_first_not_of(' ') != std::string::npos) {
                    code = code.substr(1, code.size() - 2);
                }
                protectedText.push_back(std::move(code));
                out += placeholder(protectedText.size() - 1);
                i = close + ticks;
                continue;
            }
        }
        out += input[i++];
    }
    return out;
}

void restoreInline(std::string& text, const std::vector<std::string>& protectedText) {
    for (size_t i = 0; i < protectedText.size(); ++i) {
        const std::string token = placeholder(i);
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.replace(pos, token.size(), protectedText[i]);
            pos += protectedText[i].size();
        }
    }
}

bool fenceAt(const std::string& line, char& marker, size_t& count) {
    size_t pos = 0;
    while (pos < line.size() && pos < 3 && line[pos] == ' ') ++pos;
    if (pos >= line.size() || (line[pos] != '`' && line[pos] != '~')) return false;
    marker = line[pos];
    count = 0;
    while (pos + count < line.size() && line[pos + count] == marker) ++count;
    return count >= 3;
}

void stripPairs(std::string& text, const std::string& marker) {
    size_t from = 0;
    while (true) {
        const size_t open = text.find(marker, from);
        if (open == std::string::npos) break;
        const size_t close = text.find(marker, open + marker.size());
        if (close == std::string::npos) {
            // A long reply can be segmented between opening and closing
            // delimiters before the adapter downgrade. Remove the orphan on
            // this segment instead of leaking Markdown punctuation to OneBot.
            text.erase(open, marker.size());
            from = open;
            continue;
        }
        if (close == open + marker.size()) {
            // Empty interpolated emphasis such as **{optional}** becomes ****.
            text.erase(open, marker.size() * 2);
            from = open;
            continue;
        }
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

std::string escapeLiteral(const std::string& text) {
    static constexpr std::string_view kEscapable = R"(\`*_{}[]()#+-.!>|~)";
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '[' && protectedOneBotCode(text, i)) {
            const size_t end = text.find(']', i + 1);
            if (end != std::string::npos) {
                out.append(text, i, end - i + 1);
                i = end + 1;
                continue;
            }
        }
        const char ch = text[i++];
        if (kEscapable.find(ch) != std::string_view::npos) out += '\\';
        out += ch;
    }
    return out;
}

std::string toPlainText(const std::string& markdownText) {
    std::istringstream lines(markdownText);
    std::vector<std::string> plainLines;
    std::string line;
    bool inFence = false;
    char fenceMarker = 0;
    size_t fenceLength = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        char marker = 0;
        size_t markerCount = 0;
        if (fenceAt(line, marker, markerCount)) {
            if (!inFence) {
                inFence = true;
                fenceMarker = marker;
                fenceLength = markerCount;
                continue;
            }
            if (marker == fenceMarker && markerCount >= fenceLength) {
                inFence = false;
                continue;
            }
        }
        if (inFence) {
            plainLines.push_back(line);
            continue;
        }

        std::vector<std::string> protectedText;
        line = protectInline(line, protectedText);
        line = stripLinePrefix(line);
        line = stripLinks(line);
        stripPairs(line, "**");
        stripPairs(line, "__");
        stripPairs(line, "~~");
        stripPairs(line, "||");
        stripPairs(line, "`");
        stripSingleEmphasis(line, '*');
        stripSingleEmphasis(line, '_');

        restoreInline(line, protectedText);
        plainLines.push_back(std::move(line));
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
