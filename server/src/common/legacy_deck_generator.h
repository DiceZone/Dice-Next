#pragma once
#include "safe_template.h"
#include "session_deck.h"
#include <sstream>

namespace dice::session_deck {
// Cached nicknames are data, not outbound CQ actions or page controls.
inline std::string memberLabel(std::string_view name) {
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name.substr(i, 4) == "[CQ:") result += "［";
        else if (static_cast<unsigned char>(name[i]) < 32 || name[i] == 127) result += ' ';
        else result += name[i];
    }
    return result;
}
inline std::vector<std::string> rangeCards(const std::string& args, std::string& error) {
    std::istringstream input(args);
    std::vector<int64_t> numbers;
    std::string token;
    while (input >> token) {
        auto value = safe_template::integer(token);
        if (!value || numbers.size() == 3) { error = "范围需要 1—3 个整数：起点、终点、正步长"; return {}; }
        numbers.push_back(*value);
    }
    if (numbers.empty()) { error = "缺少数字范围"; return {}; }
    int64_t low = numbers.size() == 1 ? 0 : numbers[0];
    int64_t high = numbers.size() == 1 ? numbers[0] : numbers[1];
    const int64_t step = numbers.size() == 3 ? numbers[2] : 1;
    if (step <= 0) { error = "步长必须是正整数"; return {}; }
    if (low > high) std::swap(low, high);
    const uint64_t distance = static_cast<uint64_t>(high) - static_cast<uint64_t>(low);
    const auto intervals = distance / static_cast<uint64_t>(step);
    if (intervals >= maxCards) { error = "生成的牌面超过 10000 张"; return {}; }
    std::vector<std::string> cards;
    int64_t current = low;
    for (uint64_t i = 0; i <= intervals; ++i) {
        cards.push_back(std::to_string(current));
        if (i != intervals) current += step; // Proven <= high; no signed overflow.
    }
    return cards;
}
} // namespace dice::session_deck
