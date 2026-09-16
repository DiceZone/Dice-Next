#include "test_framework.h"
#include "common/sample_template.h"
#include "i18n/i18n.h"

using namespace dice;

TEST(SampleTemplate, EveryTopLevelOptionIncludingEmptyIsReachable) {
    const std::vector<std::string> expected{"甲", "", "乙", ""};
    for (size_t i = 0; i < expected.size(); ++i) {
        size_t seen = 0;
        auto select = [&](size_t count) { seen = count; return i; };
        ASSERT_EQ(sample_template::expand("{sample:甲||乙|}", select), expected[i]);
        ASSERT_EQ(seen, expected.size());
    }
}

TEST(SampleTemplate, NestedChoicesAreLazyAndVariablesStayIntact) {
    size_t calls = 0;
    auto first = [&](size_t) { ++calls; return size_t(0); };
    ASSERT_EQ(sample_template::expand("{sample:{nick}|{sample:乙|丙}}", first), "{nick}");
    ASSERT_EQ(calls, size_t(1));
    auto last = [](size_t n) { return n - 1; };
    ASSERT_EQ(sample_template::expand("前{sample:甲|{sample:乙|{res}}}后", last), "前{res}后");
    ASSERT_EQ(sample_template::expand("{sample:甲|乙}{sample:丙|丁}", last), "乙丁");
    ASSERT_EQ(sample_template::expand("{sample:}{sample:独项}", last), "独项");
}

TEST(SampleTemplate, MalformedEscapedAndDeepInputIsBounded) {
    auto first = [](size_t) { return size_t(0); };
    ASSERT_EQ(sample_template::expand("{sample:甲|{sample:乙|丙}", first), "{sample:甲|{sample:乙|丙}");
    ASSERT_EQ(sample_template::expand(R"(\{sample:甲|乙})", first), R"(\{sample:甲|乙})");
    ASSERT_EQ(sample_template::expand("{other:甲|乙}", first), "{other:甲|乙}");
    std::string deep = "值";
    for (int n = 0; n < 100; ++n) deep = "{sample:" + deep + "}";
    const auto result = sample_template::expand(deep, first);
    ASSERT_TRUE(result.find("{sample:") != std::string::npos);
    ASSERT_TRUE(result.size() < deep.size());
}

TEST(SampleTemplate, I18nOverridesPersonasAndLiteralArguments) {
    I18n i18n("nonexistent_dir");
    i18n.setOverride(Locale::kZhHans, "sample.test", "{sample:{nick}={res}|{nick}={res}}");
    const std::string name = "{sample:用户|内容}";
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "sample.test", {{"nick", name}, {"res", "42"}}), name + "=42");
    i18n.setPersonaBundles(7, Locale::kZhHans, {{"sample.test", "{sample:人格{res}}"}});
    auto scope = i18n.scopedPersona(7);
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "sample.test", {{"res", "42"}}), "人格42");
    ASSERT_EQ(i18n.tr(Locale::kEn, "sample.test", {{"res", "42"}}), "人格42");
}

TEST(SampleTemplate, MarkdownAndCachedPlainTextKeepVariablesAndChoices) {
    I18n i18n("nonexistent_dir");
    i18n.setOverride(Locale::kZhHans, "sample.test", "{sample:**{nick}**：`{res}`|**{nick}**：`{res}`}", ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kMarkdown);
    const auto rich = i18n.tr(Locale::kZhHans, "sample.test", {{"nick", "A*B"}, {"res", "42"}});
    const auto richFormat = I18n::endOutboundCapture();
    ASSERT_EQ(rich, "**A\\*B**：`42`");
    ASSERT_TRUE(richFormat == ContentFormat::kMarkdown);
    I18n::beginOutboundCapture(ContentFormat::kPlainText);
    const auto plain = i18n.tr(Locale::kZhHans, "sample.test", {{"nick", "A*B"}, {"res", "42"}});
    const auto plainFormat = I18n::endOutboundCapture();
    ASSERT_EQ(plain, "A*B：42");
    ASSERT_TRUE(plainFormat == ContentFormat::kPlainText);
    i18n.setOverride(Locale::kZhHans, "sample.test", "{sample:效果拔群|干得漂亮}");
    for (int n = 0; n < 32; ++n) {
        const auto text = i18n.tr(Locale::kZhHans, "sample.test");
        ASSERT_TRUE(text == "效果拔群" || text == "干得漂亮");
    }
}
