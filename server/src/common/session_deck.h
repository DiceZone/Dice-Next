#pragma once
// Data-only legacy Session decks. The first `size` indices are the remaining
// cards; the tail is draw history. Keep that permutation intact on migration.
#include "legacy_conf_value.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace dice::session_deck {
using Json = nlohmann::json;
inline constexpr size_t maxDecks = 10, maxCards = 10000, maxCopies = 100000, maxBytes = 4 * 1024 * 1024;
inline std::string nameKey(std::string name) {
    for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
    return name;
}
inline std::string findName(const Json& collection, const std::string& name) {
    if (!collection.is_object()) return {};
    const auto key = nameKey(name);
    for (const auto& [candidate, unused] : collection.items()) if (nameKey(candidate) == key) return candidate;
    return {};
}
inline bool sizeValue(const Json& value, size_t maximum, size_t& result) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) return false;
    if (value.is_number_integer() && !value.is_number_unsigned() && value.get<long long>() < 0) return false;
    const auto n = value.get<unsigned long long>();
    if (n > maximum) return false;
    result = static_cast<size_t>(n); return true;
}
inline bool valid(const Json& deck, std::string& error) {
    if (!deck.is_object() || !deck.contains("meta") || !deck["meta"].is_array()
        || !deck.contains("idxs") || !deck["idxs"].is_array() || !deck.contains("size")) {
        error = "牌堆缺少 meta / idxs / size"; return false;
    }
    const auto& cards = deck["meta"]; const auto& indices = deck["idxs"];
    size_t remaining = 0, bytes = 0;
    if (cards.size() > maxCards || indices.size() > maxCopies || !sizeValue(deck["size"], indices.size(), remaining)) {
        error = "牌堆数量或剩余数量无效"; return false;
    }
    for (const auto& card : cards) {
        if (!card.is_string() || card.get_ref<const std::string&>().size() > 32768) {
            error = "牌面不是文本或超过长度限制"; return false;
        }
        bytes += card.get_ref<const std::string&>().size();
        if (bytes > maxBytes) { error = "牌堆文本过大"; return false; }
    }
    for (const auto& index : indices) {
        size_t value = 0;
        if (cards.empty() || !sizeValue(index, cards.size() - 1, value)) {
            error = "牌堆索引越界"; return false;
        }
    }
    return true;
}
inline bool validCollection(const Json& collection, std::string& error) {
    if (!collection.is_object() || collection.size() > maxDecks) { error = "团务牌堆数量超过限制或不是对象"; return false; }
    std::set<std::string> names;
    for (const auto& [name, deck] : collection.items()) {
        if (name.empty() || name.size() > 256 || !names.insert(nameKey(name)).second || !valid(deck, error)) {
            if (error.empty()) error = "牌堆名无效";
            error = name + ": " + error; return false;
        }
    }
    if (collection.dump().size() > maxBytes) { error = "团务牌堆数据超过 4 MiB"; return false; }
    return true;
}
inline bool decode(const std::string& raw, Json& collection, std::string& error) {
    if (raw.size() > maxBytes) { error = "团务牌堆数据超过 4 MiB"; return false; }
    if (raw.empty()) collection = Json::object();
    else if (!legacy_conf::decode(raw, collection)) collection = Json::parse(raw, nullptr, false);
    return validCollection(collection, error);
}
using Expand = std::function<std::string(const std::string&)>;
inline Json create(const std::vector<std::string>& cards, const Expand& expandWeight, std::string& error) {
    if (cards.empty() || cards.size() > maxCards) { error = "牌面数量无效"; return {}; }
    Json result = {{"meta", Json::array()}, {"idxs", Json::array()}, {"size", 0}};
    size_t bytes = 0;
    for (auto card : cards) {
        bytes += card.size();
        if (card.size() > 32768 || bytes > maxBytes) { error = "牌堆文本过大"; return {}; }
        size_t copies = 1;
        if (card.rfind("::", 0) == 0) {
            const auto close = card.find("::", 2);
            if (close != std::string::npos) {
                auto weight = card.substr(2, close - 2);
                if (expandWeight) weight = expandWeight(weight);
                const auto parsed = std::from_chars(weight.data(), weight.data() + weight.size(), copies);
                if (weight.empty() || parsed.ec != std::errc() || parsed.ptr != weight.data() + weight.size() || copies > maxCopies) {
                    error = "牌面数量权重必须是有效的非负整数"; return {};
                }
                card.erase(0, close + 2);
            }
        }
        if (copies > maxCopies - result["idxs"].size()) { error = "牌堆总份数超过限制"; return {}; }
        const auto index = result["meta"].size();
        result["meta"].push_back(card);
        for (size_t i = 0; i < copies; ++i) result["idxs"].push_back(index);
    }
    result["size"] = result["idxs"].size();
    if (result["idxs"].empty()) { error = "牌堆没有可抽取的牌"; return {}; }
    if (!valid(result, error)) return {};
    return result;
}
struct Draw { bool ok = false, empty = false; std::string text, error; };
inline Draw draw(Json& deck, std::mt19937_64& rng) {
    Draw result;
    if (!valid(deck, result.error)) return result;
    const auto remaining = deck["size"].get<size_t>();
    if (!remaining) { result.empty = true; return result; }
    const auto selected = std::uniform_int_distribution<size_t>(0, remaining - 1)(rng);
    const auto index = deck["idxs"][selected].get<size_t>();
    std::swap(deck["idxs"][selected], deck["idxs"][remaining - 1]);
    deck["size"] = remaining - 1;
    result.ok = true; result.text = deck["meta"][index].get<std::string>();
    return result;
}
inline bool reset(Json& deck, std::string& error) {
    if (!valid(deck, error)) return false;
    deck["size"] = deck["idxs"].size(); return true;
}
} // namespace dice::session_deck
