#include "markdown.h"

#include <md4c.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <vector>

namespace dice::markdown {

std::string legacyToPlainText(const std::string& markdownText);

namespace {

std::string parserPlaceholder(size_t index) {
    return std::string(1, '\x1d') + std::to_string(index) + '\x1e';
}

bool parserFenceAt(const std::string& line, char& marker, size_t& count) {
    size_t pos = 0;
    while (pos < line.size() && pos < 3 && line[pos] == ' ') ++pos;
    if (pos >= line.size() || (line[pos] != '\x60' && line[pos] != '~')) return false;
    marker = line[pos];
    count = 0;
    while (pos + count < line.size() && line[pos + count] == marker) ++count;
    return count >= 3;
}

bool parserProtocolCodeAt(const std::string& text, size_t pos) {
    return text.compare(pos, 4, "[CQ:") == 0 ||
           text.compare(pos, 5, "[img,") == 0 ||
           text.compare(pos, 8, "[图片:") == 0 ||
           text.compare(pos, 5, "[图:") == 0;
}

std::string protectParserLine(const std::string& input,
                              std::vector<std::string>& protectedText) {
    static constexpr std::string_view kEscapable = R"(\`*_{}[]()#+-.!>|~)";
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size();) {
        if (input[i] == '[' && parserProtocolCodeAt(input, i)) {
            const size_t end = input.find(']', i + 1);
            if (end != std::string::npos) {
                protectedText.push_back(input.substr(i, end - i + 1));
                out += parserPlaceholder(protectedText.size() - 1);
                i = end + 1;
                continue;
            }
        }
        if (input[i] == '\\' && i + 1 < input.size() &&
            kEscapable.find(input[i + 1]) != std::string_view::npos) {
            protectedText.emplace_back(1, input[i + 1]);
            out += parserPlaceholder(protectedText.size() - 1);
            i += 2;
            continue;
        }
        if (input[i] == '*' && i > 0 && i + 1 < input.size()) {
            const unsigned char before = static_cast<unsigned char>(input[i - 1]);
            const unsigned char after = static_cast<unsigned char>(input[i + 1]);
            if (std::isalnum(before) && std::isalnum(after)) {
                protectedText.emplace_back("*");
                out += parserPlaceholder(protectedText.size() - 1);
                ++i;
                continue;
            }
        }
        if (input[i] == '\x60') {
            size_t ticks = 1;
            while (i + ticks < input.size() && input[i + ticks] == '\x60') ++ticks;
            const std::string delimiter(ticks, '\x60');
            const size_t close = input.find(delimiter, i + ticks);
            if (close != std::string::npos) {
                std::string code = input.substr(i + ticks, close - i - ticks);
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' &&
                    code.find_first_not_of(' ') != std::string::npos) {
                    code = code.substr(1, code.size() - 2);
                }
                protectedText.push_back(std::move(code));
                out += parserPlaceholder(protectedText.size() - 1);
                i = close + ticks;
                continue;
            }
        }
        out += input[i++];
    }
    return out;
}

std::string prepareForParser(const std::string& input,
                             std::vector<std::string>& protectedText) {
    std::string out;
    out.reserve(input.size());
    bool inFence = false;
    char fenceMarker = 0;
    size_t fenceLength = 0;

    for (size_t start = 0; start <= input.size();) {
        const size_t newline = input.find('\n', start);
        const bool hasNewline = newline != std::string::npos;
        const size_t end = hasNewline ? newline : input.size();
        std::string line = input.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        char marker = 0;
        size_t count = 0;
        const bool fence = parserFenceAt(line, marker, count);
        if (fence) {
            if (!inFence) {
                inFence = true;
                fenceMarker = marker;
                fenceLength = count;
            } else if (marker == fenceMarker && count >= fenceLength) {
                inFence = false;
            }
            out += line;
        } else if (inFence) {
            out += line;
        } else {
            out += protectParserLine(line, protectedText);
        }

        if (hasNewline) out += '\n';
        if (!hasNewline) break;
        start = newline + 1;
    }
    return out;
}

void restoreParserText(std::string& text,
                       const std::vector<std::string>& protectedText) {
    for (size_t i = 0; i < protectedText.size(); ++i) {
        const std::string token = parserPlaceholder(i);
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.replace(pos, token.size(), protectedText[i]);
            pos += protectedText[i].size();
        }
    }
}

void trimRight(std::string& text) {
    while (!text.empty()) {
        const char ch = text.back();
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        text.pop_back();
    }
}

void trimTrailingLineBreaks(std::string& text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
}

void finishLine(std::string& text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r')) {
        text.pop_back();
    }
    if (!text.empty() && text.back() != '\n') text += '\n';
}

void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        codepoint = 0xfffd;
    }
    if (codepoint <= 0x7f) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7ff) {
        out += static_cast<char>(0xc0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        out += static_cast<char>(0xe0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

std::string decodeEntity(std::string_view entity) {
    if (entity == "&amp;") return "&";
    if (entity == "&lt;") return "<";
    if (entity == "&gt;") return ">";
    if (entity == "&quot;") return "\"";
    if (entity == "&apos;" || entity == "&#39;") return "'";
    if (entity == "&nbsp;") return " ";

    if (entity.size() >= 4 && entity.front() == '&' &&
        entity[1] == '#' && entity.back() == ';') {
        const bool hex = entity.size() >= 5 && (entity[2] == 'x' || entity[2] == 'X');
        const size_t beginIndex = hex ? 3 : 2;
        const char* begin = entity.data() + beginIndex;
        const char* end = entity.data() + entity.size() - 1;
        std::uint32_t codepoint = 0;
        const auto result = std::from_chars(begin, end, codepoint, hex ? 16 : 10);
        if (result.ec == std::errc{} && result.ptr == end) {
            std::string decoded;
            appendUtf8(decoded, codepoint);
            return decoded;
        }
    }
    return std::string(entity);
}

std::string decodeEntities(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '&') {
            const size_t end = input.find(';', i + 1);
            if (end != std::string_view::npos && end - i <= 50) {
                out += decodeEntity(input.substr(i, end - i + 1));
                i = end + 1;
                continue;
            }
        }
        out += input[i++];
    }
    return out;
}

std::string attributeText(const MD_ATTRIBUTE& attribute) {
    if (!attribute.text || attribute.size == 0) return {};
    return decodeEntities(std::string_view(attribute.text, attribute.size));
}

struct ListContext {
    bool ordered = false;
    unsigned next = 1;
};

struct LinkContext {
    MD_SPANTYPE type = MD_SPAN_A;
    size_t labelStart = 0;
    std::string destination;
};

struct PlainTextState {
    std::string output;
    std::vector<std::string>* protectedText = nullptr;
    std::vector<ListContext> lists;
    std::vector<LinkContext> links;
    unsigned tableCell = 0;
    unsigned codeBlockDepth = 0;
    bool atListPrefix = false;
    bool inHtmlTag = false;
};

void appendProtected(PlainTextState& state, std::string value) {
    if (value.empty()) return;
    state.protectedText->push_back(std::move(value));
    state.output += parserPlaceholder(state.protectedText->size() - 1);
}

int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<PlainTextState*>(userdata);
    switch (type) {
    case MD_BLOCK_UL:
        if (!state.lists.empty() && !state.atListPrefix) finishLine(state.output);
        state.lists.push_back({false, 1});
        break;
    case MD_BLOCK_OL: {
        if (!state.lists.empty() && !state.atListPrefix) finishLine(state.output);
        const auto* ordered = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
        state.lists.push_back({true, ordered ? ordered->start : 1});
        break;
    }
    case MD_BLOCK_LI: {
        finishLine(state.output);
        if (state.lists.size() > 1) state.output.append((state.lists.size() - 1) * 2, ' ');
        const auto* item = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
        if (item && item->is_task) {
            state.output += item->task_mark == ' ' ? "[ ] " : "[x] ";
        } else if (!state.lists.empty() && state.lists.back().ordered) {
            state.output += std::to_string(state.lists.back().next) + ". ";
        } else {
            state.output += "- ";
        }
        state.atListPrefix = true;
        break;
    }
    case MD_BLOCK_H:
    case MD_BLOCK_P:
    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
        if (state.atListPrefix) {
            state.atListPrefix = false;
        } else {
            finishLine(state.output);
        }
        if (type == MD_BLOCK_CODE) ++state.codeBlockDepth;
        break;
    case MD_BLOCK_QUOTE:
        finishLine(state.output);
        break;
    case MD_BLOCK_HR:
        finishLine(state.output);
        state.output += "---";
        finishLine(state.output);
        break;
    case MD_BLOCK_TR:
        finishLine(state.output);
        state.tableCell = 0;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (state.tableCell++ > 0) state.output += " | ";
        break;
    default:
        break;
    }
    return 0;
}

int leaveBlock(MD_BLOCKTYPE type, void*, void* userdata) {
    auto& state = *static_cast<PlainTextState*>(userdata);
    switch (type) {
    case MD_BLOCK_H:
    case MD_BLOCK_P:
    case MD_BLOCK_HTML:
    case MD_BLOCK_TR:
        finishLine(state.output);
        break;
    case MD_BLOCK_CODE:
        if (state.codeBlockDepth > 0) --state.codeBlockDepth;
        finishLine(state.output);
        break;
    case MD_BLOCK_LI:
        finishLine(state.output);
        state.atListPrefix = false;
        if (!state.lists.empty() && state.lists.back().ordered) {
            ++state.lists.back().next;
        }
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (!state.lists.empty()) state.lists.pop_back();
        state.atListPrefix = false;
        break;
    case MD_BLOCK_TABLE:
        finishLine(state.output);
        break;
    default:
        break;
    }
    return 0;
}

int enterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<PlainTextState*>(userdata);
    LinkContext link;
    link.type = type;
    link.labelStart = state.output.size();

    if (type == MD_SPAN_A) {
        const auto* value = static_cast<const MD_SPAN_A_DETAIL*>(detail);
        link.destination = value ? attributeText(value->href) : std::string{};
    } else if (type == MD_SPAN_IMG) {
        const auto* value = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
        link.destination = value ? attributeText(value->src) : std::string{};
    } else if (type == MD_SPAN_WIKILINK) {
        const auto* value = static_cast<const MD_SPAN_WIKILINK_DETAIL*>(detail);
        link.destination = value ? attributeText(value->target) : std::string{};
    } else {
        return 0;
    }

    state.links.push_back(std::move(link));
    return 0;
}

int leaveSpan(MD_SPANTYPE type, void*, void* userdata) {
    auto& state = *static_cast<PlainTextState*>(userdata);
    if (type != MD_SPAN_A && type != MD_SPAN_IMG && type != MD_SPAN_WIKILINK) return 0;
    if (state.links.empty()) return 0;

    LinkContext link = std::move(state.links.back());
    state.links.pop_back();
    if (link.type != type || link.labelStart > state.output.size()) return 0;

    const std::string label = state.output.substr(link.labelStart);
    std::string comparableDestination = link.destination;
    if (comparableDestination.starts_with("mailto:")) {
        comparableDestination.erase(0, 7);
    }
    if (link.destination.empty() || label == link.destination ||
        label == comparableDestination) {
        return 0;
    }
    if (!label.empty()) {
        state.output += " (" + link.destination + ")";
    } else {
        state.output += link.destination;
    }
    return 0;
}

int appendText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& state = *static_cast<PlainTextState*>(userdata);
    const std::string_view value(text, size);
    state.atListPrefix = false;

    switch (type) {
    case MD_TEXT_NORMAL:
        state.output.append(value);
        break;
    case MD_TEXT_NULLCHAR:
        appendUtf8(state.output, 0xfffd);
        break;
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
        finishLine(state.output);
        break;
    case MD_TEXT_ENTITY:
        state.output += decodeEntity(value);
        break;
    case MD_TEXT_CODE:
    case MD_TEXT_LATEXMATH: {
        std::string code(value);
        bool endedWithNewline = false;
        if (state.codeBlockDepth > 0 && !code.empty() && code.back() == '\n') {
            code.pop_back();
            if (!code.empty() && code.back() == '\r') code.pop_back();
            endedWithNewline = true;
        }
        appendProtected(state, std::move(code));
        if (endedWithNewline) finishLine(state.output);
        break;
    }
    case MD_TEXT_HTML:
        for (const char ch : value) {
            if (ch == '<') {
                state.inHtmlTag = true;
            } else if (ch == '>') {
                state.inHtmlTag = false;
            } else if (!state.inHtmlTag) {
                state.output += ch;
            }
        }
        break;
    }
    return 0;
}

bool renderPlainText(const std::string& source,
                     std::vector<std::string>& protectedText,
                     std::string& output) {
    PlainTextState state;
    state.protectedText = &protectedText;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = enterBlock;
    parser.leave_block = leaveBlock;
    parser.enter_span = enterSpan;
    parser.leave_span = leaveSpan;
    parser.text = appendText;

    if (md_parse(source.data(), static_cast<MD_SIZE>(source.size()),
                 &parser, &state) != 0) {
        return false;
    }
    trimRight(state.output);
    output = std::move(state.output);
    return true;
}

} // namespace

std::string toPlainText(const std::string& markdownText) {
    if (markdownText.empty()) return {};
    if (markdownText.find_first_of("*_~\x60[#><") == std::string::npos) {
        return markdownText;
    }

    std::vector<std::string> protectedText;
    const std::string source = prepareForParser(markdownText, protectedText);
    std::string parsed;
    if (!renderPlainText(source, protectedText, parsed)) {
        return legacyToPlainText(markdownText);
    }

    // Keep the legacy pass as a compatibility cleanup for segmented messages
    // with orphaned delimiters. Well-formed structure has already been handled
    // by MD4C, so this pass does not have to guess nested Markdown syntax.
    std::string plain = legacyToPlainText(parsed);
    restoreParserText(plain, protectedText);
    trimTrailingLineBreaks(plain);
    return plain;
}

} // namespace dice::markdown

