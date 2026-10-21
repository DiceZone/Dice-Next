#pragma once
#include <string>

namespace dice::update {
inline std::string boundedReleaseNotes(std::string text, size_t limit = 24000) {
    if (text.size() > limit) {
        auto end = limit;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
            --end;
        text.resize(end);
    }
    return text;
}
// Keep user-facing changes, not the collapsed commit/source history. Both the
// fetched manifest and older staged packages may legitimately have no notes.
inline std::string releaseNotesPreview(std::string text) {
    for (const auto *marker : {"## 源码与完整比较", "## Source", "<details>"}) {
        const auto pos = text.find(marker);
        if (pos != std::string::npos)
            text.resize(pos);
    }
    std::string plain;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '<' && i + 1 < text.size() &&
            ((text[i + 1] >= 'a' && text[i + 1] <= 'z') || text[i + 1] == '/' || text[i + 1] == '!')) {
            const auto end = text.find('>', i + 1), newline = text.find('\n', i + 1);
            if (end != std::string::npos && (newline == std::string::npos || end < newline)) {
                i = end;
                continue;
            }
        }
        if (c == '\r' || c == '`')
            continue;
        plain += c;
    }
    for (size_t pos = 0; pos < plain.size();) {
        auto next = plain.find('\n', pos);
        if (plain[pos] == '#') {
            auto end = plain.find_first_not_of('#', pos);
            if (end != std::string::npos && plain[end] == ' ')
                plain.erase(pos, end - pos + 1);
        }
        next = plain.find('\n', pos);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    // Inline links are kept as readable labels. The canonical release URL is
    // supplied separately and never taken from arbitrary Markdown destinations.
    for (size_t pos = 0; (pos = plain.find('[', pos)) != std::string::npos;) {
        const auto mid = plain.find("](", pos),
                   end = mid == std::string::npos ? mid : plain.find(')', mid + 2);
        if (mid == std::string::npos || end == std::string::npos) {
            ++pos;
            continue;
        }
        plain.replace(pos, end - pos + 1, plain.substr(pos + 1, mid - pos - 1));
        ++pos;
    }
    const auto begin = plain.find_first_not_of(" \n\t");
    if (begin == std::string::npos)
        return "本次发布的更新元数据未附带具体说明，请打开发布页查看（不代表没有改动）。";
    plain.erase(0, begin);
    if (plain.size() > 3200)
        plain = boundedReleaseNotes(plain, 3200) + "\n…更多改动请查看发布页。";
    return plain;
}
} // namespace dice::update
