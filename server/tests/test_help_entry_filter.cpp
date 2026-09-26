#include "test_framework.h"
#include "../src/core/help_entry_filter.h"

TEST(HelpEntryFilter, RuntimeStillUsesFirstNonemptyTopic) {
    dice::HelpEntryFilter filter(false);
    ASSERT_FALSE(filter.add("empty", "").include);
    ASSERT_TRUE(filter.add("empty", "fallback").include);
    ASSERT_TRUE(filter.add("roll", "builtin").include);
    const auto duplicate = filter.add("roll", "file");
    ASSERT_FALSE(duplicate.include);
    ASSERT_TRUE(duplicate.shadowed);
    ASSERT_FALSE(filter.add("", "invalid").include);
}

TEST(HelpEntryFilter, ManagementKeepsEmptyAndShadowedDocuments) {
    dice::HelpEntryFilter filter(true);
    const auto empty = filter.add("空文档", "");
    ASSERT_TRUE(empty.include); ASSERT_FALSE(empty.shadowed);
    ASSERT_TRUE(filter.add("roll", "builtin").include);
    const auto duplicate = filter.add("roll", "custom file");
    ASSERT_TRUE(duplicate.include); ASSERT_TRUE(duplicate.shadowed);
    const auto fallback = filter.add("空文档", "fallback");
    ASSERT_TRUE(fallback.include); ASSERT_FALSE(fallback.shadowed);
    ASSERT_FALSE(filter.add("", "invalid").include);
}
