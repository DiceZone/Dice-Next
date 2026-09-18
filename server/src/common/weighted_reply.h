#pragma once

#include <charconv>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace dice::weighted_reply {

struct Answer {
    uint64_t weight = 1;
    std::string_view text;
};

// Original Dice! Deck replies accept ::N:: (positive, at most six digits).
// Invalid markers remain literal, rather than silently hiding authored text.
inline Answer parse(std::string_view text) {
    const auto left = text.find("::");
    if (left == std::string_view::npos) return {1, text};
    const auto right = text.find("::", left + 2);
    if (right == std::string_view::npos) return {1, text};
    const auto number = text.substr(left + 2, right - left - 2);
    if (number.empty() || number.size() > 6) return {1, text};
    uint64_t weight = 0;
    const auto result = std::from_chars(number.data(), number.data() + number.size(), weight);
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || weight == 0)
        return {1, text};
    return {weight, text.substr(right + 2)};
}

// Injectable ticket selection makes weight boundaries testable without flaky
// frequency assertions. Do not expand a weight into N duplicate strings.
template<class Choose>
std::string pick(const std::vector<std::string>& answers, Choose choose) {
    if (answers.empty()) return {};
    uint64_t total = 0;
    for (const auto& answer : answers) total += parse(answer).weight;
    uint64_t ticket = answers.size() == 1 ? 0 : choose(total);
    for (const auto& answer : answers) {
        const auto parsed = parse(answer);
        if (ticket < parsed.weight) return std::string(parsed.text);
        ticket -= parsed.weight;
    }
    return {};
}

inline std::string pick(const std::vector<std::string>& answers) {
    return pick(answers, [](uint64_t total) {
        thread_local std::mt19937_64 random(std::random_device{}());
        return std::uniform_int_distribution<uint64_t>(0, total - 1)(random);
    });
}

} // namespace dice::weighted_reply
