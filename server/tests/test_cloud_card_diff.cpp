#include "test_framework.h"
#include "src/common/cloud_card_diff.h"
using namespace dice::cloud_cards;

TEST(CloudCardDiff, ClassifiesIndependentEditsConflictsAndEqualChangesWithoutWrites) {
    DiffJson base = {{"attrs", {{"HP", 10}, {"SAN", 60}, {"MP", 10}, {"力", 50}}}, {"rev", 1}};
    auto local = base, remote = base;
    local["attrs"]["HP"] = 8; remote["attrs"]["SAN"] = 59;
    local["attrs"]["MP"] = 9; remote["attrs"]["MP"] = 7;
    local["attrs"]["力"] = remote["attrs"]["力"] = 55; remote["rev"] = 2;
    const auto differences = compareDocuments(base, local, remote);
    ASSERT_EQ(differences.size(), size_t(4));
    EXPECT_TRUE(differences[0].kind == Difference::Kind::Local);
    EXPECT_TRUE(differences[1].kind == Difference::Kind::Conflict);
    EXPECT_TRUE(differences[2].kind == Difference::Kind::Remote);
    EXPECT_TRUE(differences[3].kind == Difference::Kind::Same);
    EXPECT_EQ(base["attrs"]["HP"].get<int>(), 10);
}
TEST(CloudCardDiff, MissingNullArraysAndEscapedPathsAreDistinct) {
    DiffJson base = {{"extra", {{"a/b~c", nullptr}, {"items", DiffJson::array({1, 2})}}}};
    auto local = base, remote = base;
    local["extra"].erase("a/b~c"); remote["extra"]["a/b~c"] = 1;
    local["extra"]["items"] = DiffJson::array({1, 2, 3});
    const auto differences = compareDocuments(base, local, remote);
    ASSERT_EQ(differences.size(), size_t(2));
    EXPECT_EQ(differences[0].path, "/extra/a~1b~0c");
    EXPECT_TRUE(differences[0].base.has_value()); EXPECT_FALSE(differences[0].local.has_value());
    EXPECT_TRUE(differences[0].kind == Difference::Kind::Conflict);
}
TEST(CloudCardDiff, ReceiptsBoundSizeAndNeverActivateMediaCodes) {
    const auto text = differenceText(compareDocuments(DiffJson::object(), {{"x", "[CQ:image,file=private]"}}, DiffJson::object()));
    EXPECT_TRUE(text.find("[CQ:") == std::string::npos);
    EXPECT_TRUE(diffValue(DiffJson(std::string(257, 'x'))).size() < 100);
    DiffJson local;
    for (int i = 0; i < 50; ++i) local[std::to_string(i)] = i;
    EXPECT_TRUE(differenceText(compareDocuments(DiffJson::object(), local, DiffJson::object())).size() < 5000);
}
TEST(CloudCardDiff, ParentDeletionAndTypeChangesConflictWithChildEdits) {
    const DiffJson base = {{"attrs", {{"HP", 10}, {"SAN", 60}}}};
    auto remote = base;
    remote["attrs"]["HP"] = 8;
    for (const auto& local : {DiffJson::object(), DiffJson{{"attrs", "replaced"}}, DiffJson{{"attrs", nullptr}}}) {
        const auto diff = compareDocuments(base, local, remote);
        ASSERT_EQ(diff.size(), size_t(1));
        EXPECT_EQ(diff[0].path, "/attrs");
        EXPECT_TRUE(diff[0].kind == Difference::Kind::Conflict);
    }
}
