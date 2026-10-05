#include "test_framework.h"
#include "common/sample_template.h"
#include "common/weighted_templates.h"
#include "common/template_preview.h"
#include "i18n/i18n.h"

using namespace dice;

TEST(WeightedTemplate, TicketsRespectWeightsAndZeroIsDisabled) {
    const auto items = weighted_templates::validate(json::array({
        {{"text", "停用"}, {"weight", 0}}, {{"text", "甲"}, {"weight", 1}},
        {{"text", "乙"}, {"weight", 3}}
    }));
    for (size_t ticket = 0; ticket < 4; ++ticket) {
        size_t seen = 0;
        auto choose = [ticket, &seen](size_t total) { seen = total; return ticket; };
        ASSERT_EQ(weighted_templates::pick(items, choose), ticket == 0 ? size_t(1) : size_t(2));
        ASSERT_EQ(seen, size_t(4));
    }
    ASSERT_EQ(weighted_templates::decode(weighted_templates::encode(items)), items);
    const auto exported = weighted_templates::apiRecord(weighted_templates::encode(items), "plain");
    ASSERT_EQ(exported["value"].get<std::string>(), "甲");
    ASSERT_EQ(weighted_templates::decode(weighted_templates::apiValue(exported)), items);
    ASSERT_TRUE(weighted_templates::decode("[ordinary JSON]").empty());
    for (const auto& invalid : std::vector<json>{
        json::array(), json::array({{{"text", "x"}, {"weight", 0}}}),
        json::array({{{"text", "x"}, {"weight", -1}}}),
        json::array({{{"text", "x"}, {"weight", 0.5}}}),
        json::array({{{"text", "x"}, {"weight", 1000000}}})
    }) {
        bool rejected = false;
        try { weighted_templates::encode(invalid); } catch (...) { rejected = true; }
        ASSERT_TRUE(rejected);
    }
}

TEST(WeightedTemplate, NativeChoiceNestedSamplePreviewAndCachedPlain) {
    const auto value = weighted_templates::encode(json::array({
        {{"text", "NEVER"}, {"weight", 0}},
        {{"text", "**{nick}**：{sample:{sample:好|好}|好} `{expr}`"}, {"weight", 2}}
    }));
    I18n i18n("nonexistent_dir");
    i18n.setOverride(Locale::kZhHans, "weighted.test", value, ContentFormat::kMarkdown);
    const I18n::Args args{{"nick", "A*B"}, {"expr", "D100"}};
    ASSERT_EQ(I18n::previewTemplate(value, args, ContentFormat::kMarkdown), "**A\\*B**：好 `D100`");
    I18n::beginOutboundCapture(ContentFormat::kPlainText);
    ASSERT_EQ(i18n.tr(Locale::kZhHans, "weighted.test", args), "A*B：好 D100");
    ASSERT_TRUE(I18n::endOutboundCapture() == ContentFormat::kPlainText);
    ASSERT_EQ(I18n::previewTemplate("{sample:{nick}|{nick}}", {{"nick", "{sample:literal|name}"}}),
              "{sample:literal|name}");
}

TEST(WeightedTemplate, PreviewAllPlatformsShareASingleSampleAndArgumentsAreLiteral) {
    const json variants = json::array({
        {{"text", "wrong"}, {"weight", 0}},
        {{"text", "**{nick}** {sample:甲|乙} `{expr}`"}, {"weight", 1}}
    });
    for (const char* platform : {"qq_group", "qq_channel", "discord", "kook", "plain"}) {
        const auto result = outbound::templatePreview({{"variants", variants}, {"format", "markdown"},
            {"platform", platform}, {"args", {{"nick", "玩家"}, {"expr", "D100"}}}});
        const auto md = result["markdown"].get<std::string>();
        ASSERT_EQ(result["templateVersion"].get<int>(), 1);
        const auto plain = result["onebot"].get<std::string>();
        ASSERT_TRUE(md == "**玩家** 甲 `D100`" || md == "**玩家** 乙 `D100`");
        ASSERT_EQ(markdown::toPlainText(md), plain);
        ASSERT_EQ(result["preview"]["plain"].get<std::string>(), plain);
    }
    const auto literal = outbound::templatePreview({{"text", "{sample:{nick}|{nick}}"},
        {"args", {{"nick", "{sample:这是|用户数据}"}}}});
    ASSERT_EQ(literal["onebot"].get<std::string>(), "{sample:这是|用户数据}");
}

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
