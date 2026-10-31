#include "test_framework.h"
#include "common/card_sheet_format.h"

using namespace dice::card_sheet;

static Labels labels() {
    return {"属性", "状态", "技能", "钱币", "公式", "其他", "连带"};
}

static Item num(std::string name, int value) {
    Item item;
    item.name = std::move(name);
    item.kind = classify(item.name);
    item.value = value;
    return item;
}

TEST(CardSheetFormat, FullCocSheetGroupsAbilitiesVitalsAndSkills) {
    std::vector<Item> items;
    for (const auto& [name, value] : std::vector<std::pair<const char*, int>>{
             {"教育", 75}, {"意志", 40}, {"智力", 80}, {"外貌", 45},
             {"敏捷", 70}, {"体型", 65}, {"体质", 55}, {"力量", 50},
             {"图书馆使用", 70}, {"侦察", 60}, {"心理学", 40}, {"急救", 30}})
        items.push_back(num(name, value));
    Item hp = num("生命值", 13);
    hp.maximum = 10;
    hp.shield = 3;
    items.push_back(hp);
    Item san = num("理智", 55);
    san.maximum = 99;
    items.push_back(san);
    Item mp = num("魔法值", 8);
    mp.maximum = 8;
    items.push_back(mp);
    Item formula;
    formula.kind = Kind::kFormula;
    formula.literal = "伤害加值=1d4(=2)";
    items.push_back(formula);

    const std::string text = formatSheet(items, labels(), {});
    ASSERT_EQ(text,
        "属性\n"
        "力量:50 体质:55 体型:65 敏捷:70 外貌:45 智力:80 意志:40 教育:75\n"
        "状态\n"
        "生命值:13/10 理智:55/99 魔法值:8/8\n"
        "技能\n"
        "图书馆使用:70 侦察:60 心理学:40 急救:30\n"
        "公式\n"
        "伤害加值=1d4(=2)");
}

TEST(CardSheetFormat, ShieldWithoutMaximumStaysVisible) {
    Item hp = num("生命值", 13);
    hp.shield = 3;
    const std::string text = formatSheet({hp}, labels(), {});
    ASSERT_EQ(text, "状态\n生命值:13(+临时3)");
}

TEST(CardSheetFormat, DndAbilitiesUseDndOrderAndDropNothingIntoSkills) {
    std::vector<Item> items = {
        num("魅力", 10), num("力量", 16), num("敏捷", 14),
        num("体质", 12), num("智力", 8), num("感知", 13), num("金币", 20)};
    const std::string text = formatSheet(items, labels(), {});
    ASSERT_EQ(text,
        "属性\n"
        "力量:16 敏捷:14 体质:12 智力:8 感知:13 魅力:10\n"
        "钱币\n"
        "金币:20");
}

TEST(CardSheetFormat, SmallDeltaIsOneLineAndDerivedChangesFollow) {
    Item con = num("体质", 60);
    con.before = 50;
    Item hp = num("生命值", 12);
    hp.before = 11;
    hp.maximum = 13;
    hp.beforeMaximum = 12;
    hp.derived = true;
    Item db;
    db.derived = true;
    db.literal = "伤害加值:0";
    const std::string text = formatSheet({con, hp, db}, labels(), {Mode::kDelta, false});
    ASSERT_EQ(text,
        "体质:50 → 60\n"
        "连带\n"
        "生命值:11/12 → 12/13 伤害加值:0");
}

TEST(CardSheetFormat, PastingAWholeSheetUsesTheSameGroupsWithoutArrows) {
    std::vector<Item> items;
    for (const auto& [name, value] : std::vector<std::pair<const char*, int>>{
             {"力量", 50}, {"体质", 55}, {"体型", 65}, {"敏捷", 70},
             {"侦查", 60}, {"聆听", 40}})
        items.push_back(num(name, value));
    const std::string text = formatSheet(items, labels(), {Mode::kDelta, false});
    ASSERT_EQ(text,
        "属性\n"
        "力量:50 体质:55 体型:65 敏捷:70\n"
        "技能\n"
        "侦查:60 聆听:40");
}

TEST(CardSheetFormat, SingleNewValueHasNoHeaderOrArrow) {
    const std::string text = formatSheet({num("侦查", 60)}, labels(), {Mode::kDelta, false});
    ASSERT_EQ(text, "侦查:60");
}

TEST(CardSheetFormat, VisualBarsOnlyReplaceVitalsOnAFullSheet) {
    Item hp = num("生命值", 12);
    hp.maximum = 14;
    Item skill = num("侦查", 60);
    Options full;
    full.bars = true;
    const std::string shown = formatSheet({hp, skill}, labels(), full);
    ASSERT_TRUE(shown.find("[[bar:HP|12|14]]") != std::string::npos);
    ASSERT_TRUE(shown.find("侦查:60") != std::string::npos);

    hp.before = 14;
    Options delta;
    delta.mode = Mode::kDelta;
    delta.bars = true;
    ASSERT_EQ(formatSheet({hp}, labels(), delta), "生命值:14/14 → 12/14");
}
