#pragma once

#include <algorithm>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dice::card_sheet {

// Human-readable character sheets. Numeric tokens stay `name:value` so one
// line can still be copied into `.st`. Readability comes from groups and
// line breaks, not from a second notation.

enum class Kind { kAbility, kVital, kSkill, kCoin, kFormula, kOther };
enum class Mode { kFull, kDelta };

struct Item {
    std::string name;
    Kind kind = Kind::kSkill;
    std::optional<int> value;
    std::optional<int> before;
    std::optional<int> maximum;
    std::optional<int> beforeMaximum;
    int shield = 0;
    std::string literal;
    bool derived = false;
    size_t index = 0;
};

struct Labels {
    std::string abilities;
    std::string vitals;
    std::string skills;
    std::string coins;
    std::string formulas;
    std::string other;
    std::string derived;
};

struct Options {
    Mode mode = Mode::kFull;
    bool bars = false;
};

inline Kind classify(const std::string& name) {
    static const std::map<std::string, Kind> kinds = [] {
        std::map<std::string, Kind> out;
        for (const char* ability : {"力量", "体质", "体型", "敏捷", "外貌", "智力", "意志", "教育", "感知", "魅力"})
            out.emplace(ability, Kind::kAbility);
        for (const char* vital : {"生命值", "临时生命值", "理智", "魔法值", "幸运", "体格", "移动",
                                  "护甲等级", "速度", "先攻", "熟练加值", "等级", "BUILD", "MOV"})
            out.emplace(vital, Kind::kVital);
        for (const char* coin : {"铂金币", "金币", "银金币", "银币", "铜币"})
            out.emplace(coin, Kind::kCoin);
        return out;
    }();
    const auto it = kinds.find(name);
    return it == kinds.end() ? Kind::kSkill : it->second;
}

inline int rankIn(const std::string& name, std::initializer_list<const char*> order) {
    int index = 0;
    for (const char* item : order) {
        if (name == item) return index;
        ++index;
    }
    return 1000;
}

inline std::string numberText(int value, std::optional<int> maximum, int shield) {
    std::string text = std::to_string(value);
    if (maximum) text += "/" + std::to_string(*maximum);
    else if (shield > 0) text += "(+临时" + std::to_string(shield) + ")";
    return text;
}

inline const char* barLabel(const std::string& name) {
    if (name == "生命值") return "HP";
    if (name == "理智") return "SAN";
    if (name == "魔法值") return "MP";
    return nullptr;
}

inline std::string cellText(const Item& item, const Options& options) {
    if (!item.literal.empty()) return item.literal;
    if (!item.value) return {};
    const bool bar = options.bars && options.mode == Mode::kFull && item.maximum && *item.maximum > 0
        && item.kind == Kind::kVital && barLabel(item.name) != nullptr && !item.before;
    if (bar) {
        return std::string("[[bar:") + barLabel(item.name) + "|" + std::to_string(*item.value)
            + "|" + std::to_string(*item.maximum) + "]]";
    }
    const std::string current = numberText(*item.value, item.maximum, item.shield);
    if (!item.before) return item.name + ":" + current;
    const std::optional<int> beforeMaximum = item.beforeMaximum ? item.beforeMaximum : item.maximum;
    const std::string previous = numberText(*item.before, beforeMaximum, 0);
    if (previous == current) return item.name + ":" + current;
    return item.name + ":" + previous + " → " + current;
}

inline void appendCells(std::string& out, const std::vector<std::string>& cells, int columns) {
    if (columns < 1) columns = 1;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i) out += (i % static_cast<size_t>(columns) == 0) ? "\n" : "  ";
        out += cells[i];
    }
}

inline std::string formatSheet(std::vector<Item> items, const Labels& labels, const Options& options) {
    for (size_t i = 0; i < items.size(); ++i) items[i].index = i;
    std::vector<const Item*> main;
    std::vector<const Item*> derived;
    bool cocShape = false;
    bool dndShape = false;
    for (const auto& item : items) {
        if (item.derived) { derived.push_back(&item); continue; }
        main.push_back(&item);
        if (item.name == "体型" || item.name == "外貌" || item.name == "意志" || item.name == "教育")
            cocShape = true;
        if (item.name == "感知" || item.name == "魅力" || item.kind == Kind::kCoin) dndShape = true;
    }
    const bool dndOrder = dndShape && !cocShape;

    std::map<Kind, int> counts;
    for (const Item* item : main) ++counts[item->kind];
    int groups = 0;
    bool big = false;
    for (const auto& [unused, count] : counts) {
        (void)unused;
        if (count > 0) ++groups;
        if (count >= 4) big = true;
    }
    const bool headers = options.mode == Mode::kFull || groups >= 2 || big;
    const bool wide = options.mode == Mode::kFull || headers;

    auto columns = [&](Kind kind) {
        if (!wide) return 1;
        if (kind == Kind::kAbility || kind == Kind::kCoin) return 4;
        if (kind == Kind::kSkill) return 2;
        return 1;
    };
    auto byIndex = [](const Item* a, const Item* b) { return a->index < b->index; };
    auto sortMain = [&](std::vector<const Item*>& group, Kind kind) {
        if (kind == Kind::kFormula || kind == Kind::kOther) {
            std::stable_sort(group.begin(), group.end(), byIndex);
            return;
        }
        std::stable_sort(group.begin(), group.end(), [&](const Item* a, const Item* b) {
            if (kind == Kind::kAbility) {
                const int ra = rankIn(a->name, dndOrder
                    ? std::initializer_list<const char*>{"力量", "敏捷", "体质", "智力", "感知", "魅力"}
                    : std::initializer_list<const char*>{"力量", "体质", "体型", "敏捷", "外貌", "智力", "意志", "教育", "感知", "魅力"});
                const int rb = rankIn(b->name, dndOrder
                    ? std::initializer_list<const char*>{"力量", "敏捷", "体质", "智力", "感知", "魅力"}
                    : std::initializer_list<const char*>{"力量", "体质", "体型", "敏捷", "外貌", "智力", "意志", "教育", "感知", "魅力"});
                if (ra != rb) return ra < rb;
            } else if (kind == Kind::kVital) {
                const auto order = {"生命值", "临时生命值", "理智", "魔法值", "幸运", "体格", "移动",
                                    "护甲等级", "速度", "先攻", "熟练加值", "等级", "BUILD", "MOV"};
                const int ra = rankIn(a->name, order);
                const int rb = rankIn(b->name, order);
                if (ra != rb) return ra < rb;
            } else if (kind == Kind::kCoin) {
                const auto order = {"铂金币", "金币", "银金币", "银币", "铜币"};
                const int ra = rankIn(a->name, order);
                const int rb = rankIn(b->name, order);
                if (ra != rb) return ra < rb;
            } else if (kind == Kind::kSkill) {
                if (a->value != b->value) {
                    if (!a->value) return false;
                    if (!b->value) return true;
                    if (*a->value != *b->value) return *a->value > *b->value;
                }
            }
            if (a->name != b->name) return a->name < b->name;
            return a->index < b->index;
        });
    };

    const std::pair<Kind, const std::string*> sections[] = {
        {Kind::kAbility, &labels.abilities},
        {Kind::kVital, &labels.vitals},
        {Kind::kSkill, &labels.skills},
        {Kind::kCoin, &labels.coins},
        {Kind::kFormula, &labels.formulas},
        {Kind::kOther, &labels.other},
    };
    std::string out;
    auto writeSection = [&](const std::string& label, std::vector<const Item*> group, int perLine, bool showLabel) {
        std::vector<std::string> cells;
        for (const Item* item : group) {
            std::string text = cellText(*item, options);
            if (!text.empty()) cells.push_back(std::move(text));
        }
        if (cells.empty()) return;
        if (!out.empty()) out += "\n\n";
        if (showLabel && !label.empty()) out += label + "\n";
        appendCells(out, cells, perLine);
    };
    for (const auto& [kind, label] : sections) {
        std::vector<const Item*> group;
        for (const Item* item : main) if (item->kind == kind) group.push_back(item);
        sortMain(group, kind);
        writeSection(*label, std::move(group), columns(kind), headers);
    }
    std::stable_sort(derived.begin(), derived.end(), byIndex);
    writeSection(labels.derived, std::move(derived), 1, true);
    return out;
}

} // namespace dice::card_sheet
