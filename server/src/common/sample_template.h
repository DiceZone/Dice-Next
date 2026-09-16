#pragma once

#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace dice::sample_template {

// Cosmetic choices use a random source independent of game dice.
inline size_t choose(size_t count) {
    if (count <= 1) return 0;
    thread_local std::mt19937_64 random(std::random_device{}());
    return std::uniform_int_distribution<size_t>(0, count - 1)(random);
}

inline bool escaped(std::string_view text, size_t pos) {
    size_t slashes = 0;
    while (pos > 0 && text[--pos] == '\\') ++slashes;
    return slashes % 2 != 0;
}

// Expand authored macros BEFORE interpolation: inserted user text is not code.
// Split only top-level pipes; process only the chosen branch, with bounded depth.
// Injecting a chooser lets tests cover every branch without probabilistic checks.
template<class Choose>
std::string expand(std::string_view text, Choose& select, unsigned depth = 0) {
    if (depth >= 32 || text.find("{sample:") == std::string_view::npos)
        return std::string(text);
    std::string result;
    result.reserve(text.size());
    size_t copied = 0, search = 0;
    while (true) {
        const size_t open = text.find("{sample:", search);
        if (open == std::string_view::npos) break;
        if (escaped(text, open)) { search = open + 8; continue; }
        size_t level = 0, start = open + 8, close = std::string_view::npos;
        std::vector<std::string_view> choices;
        for (size_t pos = start; pos < text.size(); ++pos) {
            const char c = text[pos];
            if ((c == '{' || c == '}') && escaped(text, pos)) continue;
            if (c == '{') ++level;
            else if (c == '}') {
                if (level == 0) {
                    choices.push_back(text.substr(start, pos - start));
                    close = pos;
                    break;
                }
                --level;
            } else if (c == '|' && level == 0) {
                choices.push_back(text.substr(start, pos - start));
                start = pos + 1;
            }
        }
        if (close == std::string_view::npos) break;
        result.append(text.substr(copied, open - copied));
        const size_t index = choices.size() == 1 ? 0 : select(choices.size());
        result += expand(choices.at(index), select, depth + 1);
        copied = search = close + 1;
    }
    result.append(text.substr(copied));
    return result;
}

inline std::string expand(std::string_view text) {
    auto select = [](size_t count) { return choose(count); };
    return expand(text, select);
}

} // namespace dice::sample_template
