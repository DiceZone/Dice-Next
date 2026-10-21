#include "common/check_reply.h"
#include "common/check_rules.h"
#include "common/template_preview.h"
#include "test_framework.h"
#include <filesystem>
#include <set>

using namespace dice;
namespace {
I18n &previewTranslations() {
    static const auto path = (std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n").u8string();
    static I18n i18n(std::string(path.begin(), path.end()));
    static const bool loaded = i18n.load();
    if (!loaded)
        throw std::runtime_error("cannot load preview translations");
    return i18n;
}
json example(const std::string &key, uint32_t seed = 17, const std::string &source = "") {
    return {{"version", 1}, {"key", key}, {"sourceKey", source.empty() ? key : source}, {"seed", seed}};
}
int number(const I18n::Args &args, const char *name) { return std::stoi(args.at(name)); }
} // namespace

TEST(TemplateSimulation, EveryBundledTextHasExampleDataInEveryLocale) {
    auto &i18n = previewTranslations();
    for (const auto locale : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa}) {
        for (const auto &[key, text] : i18n.flatten(locale)) {
            if (key.rfind("_meta.", 0) == 0)
                continue;
            auto simulation = example(key);
            auto result = outbound::templatePreview(
                {{"text", text}, {"simulation", simulation}, {"locale", localeToString(locale)}}, &i18n);
            const auto missing = result["simulation"]["unresolved"];
            // These braces are literal syntax examples, not reply arguments.
            if (key == "fun.text.usage")
                continue;
            if (!missing.empty())
                std::cerr << "Unresolved preview: " << localeToString(locale) << "/" << key << " " << missing << '\n';
            EXPECT_TRUE(missing.empty());
            EXPECT_TRUE(result["simulation"]["facts"]["readOnly"].get<bool>());
        }
    }
}

TEST(TemplateSimulation, SharedCocRulesPreserveTheExistingRuntimeBehavior) {
    // Golden fingerprint of the pre-extraction router implementation: every
    // D100 face, rates 0..150, and all eight house rules (120,800 decisions).
    std::uint32_t hash = 2166136261u;
    for (int rule = 0; rule < 8; ++rule)
        for (int rate = 0; rate <= 150; ++rate)
            for (int roll = 1; roll <= 100; ++roll) {
                hash ^= static_cast<std::uint32_t>(check_rules::coc(roll, rate, rule));
                hash *= 16777619u;
            }
    ASSERT_EQ(hash, 304957935u);
}

TEST(TemplateSimulation, AllCheckGradesArePossibleUnderTheActualRules) {
    auto &i18n = previewTranslations();
    for (const auto &family : check_reply::families())
        for (const auto &grade : family.grades) {
            for (uint32_t seed = 0; seed < 40; ++seed) {
                const auto context =
                    outbound::templatePreviewContext(example(check_reply::key(family.name, grade), seed), &i18n);
                const auto &a = context.args;
                ASSERT_EQ(context.grade, grade);
                if (family.name == "standard" && (grade == "special" || grade == "tie"))
                    continue;
                if (family.name == "standard" || family.name == "sanity" || family.name == "psychology" ||
                    family.name == "bonus" || family.name == "penalty" || family.name == "brp") {
                    const int value = number(a, "result"), rate = number(a, "rate");
                    const auto actual =
                        family.name == "brp" ? check_rules::brp(value, rate) : check_rules::coc(value, rate);
                    ASSERT_EQ(std::string(check_rules::grade(actual, family.name == "brp")), grade);
                    if (family.name == "sanity") {
                        ASSERT_EQ(number(a, "final"), number(a, "san") - number(a, "loss"));
                        if (grade == "fumble")
                            ASSERT_EQ(number(a, "loss"), 6);
                        else if (grade == "failure")
                            ASSERT_TRUE(number(a, "loss") >= 1 && number(a, "loss") <= 6);
                        else
                            ASSERT_EQ(number(a, "loss"), 1);
                    }
                    if (family.name == "bonus" || family.name == "penalty") {
                        std::vector<int> rolls;
                        for (const int tens : context.facts["tens"]) {
                            const int v = tens * 10 + context.facts["units"].get<int>();
                            rolls.push_back(v == 0 ? 100 : v);
                        }
                        ASSERT_EQ(value, family.name == "bonus" ? *std::min_element(rolls.begin(), rolls.end())
                                                                : *std::max_element(rolls.begin(), rolls.end()));
                    }
                } else if (family.name == "growth") {
                    ASSERT_EQ(number(a, "result") > number(a, "rate") || number(a, "result") > 95, grade == "regular");
                    ASSERT_EQ(number(a, "final"), 60 + context.facts["changeValue"].get<int>());
                } else if (family.name == "dnd_check") {
                    ASSERT_EQ(number(a, "total"), number(a, "result") + number(a, "mod"));
                    ASSERT_EQ(number(a, "total") >= number(a, "threshold"), grade == "regular");
                } else if (family.name == "death_save") {
                    ASSERT_EQ(number(a, "s"), grade == "regular" ? 1 : 0);
                    ASSERT_EQ(number(a, "f"), grade == "fumble" ? 2 : grade == "failure" ? 1 : 0);
                    if (grade == "critical")
                        ASSERT_EQ(number(a, "roll"), 20);
                    if (grade == "fumble")
                        ASSERT_EQ(number(a, "roll"), 1);
                } else if (family.name == "resist") {
                    ASSERT_EQ(number(a, "target"), 50 + (number(a, "va") - number(a, "vb")) * 5);
                    ASSERT_EQ(number(a, "result") <= number(a, "target"), grade == "regular");
                } else if (family.name == "opposed") {
                    const auto left = check_rules::coc(number(a, "ra"), number(a, "va"));
                    const auto right = check_rules::coc(number(a, "rb"), number(a, "vb"));
                    ASSERT_EQ(left > right, grade == "regular");
                    ASSERT_EQ(left < right, grade == "failure");
                }
            }
        }
}

TEST(TemplateSimulation, OriginalSanGrowthAndDeathSaveBranchesHaveRealValues) {
    auto &i18n = previewTranslations();
    for (const auto *key : {"card.sc.result", "card.sc.result_noloss", "card.en.success", "card.en.fail",
                            "card.en.fail_grow", "dnd.ds.result"}) {
        const auto rendered = outbound::templatePreview(
            {{"text", i18n.getDefault(Locale::kZhHans, key)}, {"simulation", example(key)}}, &i18n);
        ASSERT_TRUE(rendered["simulation"]["unresolved"].empty());
        const auto args = rendered["simulation"]["args"].get<I18n::Args>();
        if (std::string(key) == "card.sc.result_noloss")
            ASSERT_EQ(number(args, "loss"), 0);
        if (std::string(key) == "card.en.fail")
            ASSERT_EQ(number(args, "final"), 60);
        if (std::string(key) == "card.en.success")
            ASSERT_TRUE(number(args, "final") > 60);
        if (std::string(key) == "card.en.fail_grow")
            ASSERT_TRUE(number(args, "final") < 60);
    }
}

TEST(TemplateSimulation, InheritedStandardTextKeepsTheOriginalCommandContext) {
    auto &i18n = previewTranslations();
    auto request = example("dice.outcome.sanity.fumble", 5, "dice.outcome.standard.fumble");
    auto result = outbound::templatePreview(
        {{"text", "{roll}/{rate} {level} loss={loss} final={final}"}, {"simulation", request}}, &i18n);
    ASSERT_EQ(result["simulation"]["family"].get<std::string>(), "sanity");
    ASSERT_TRUE(result["onebot"].get<std::string>().find("loss=6 final=54") != std::string::npos);
    request = example("dice.outcome.sanity.fumble", 5, "dice.compat.sanity.result");
    result = outbound::templatePreview(
        {{"text", i18n.getDefault(Locale::kZhHans, "dice.compat.sanity.result")}, {"simulation", request}}, &i18n);
    ASSERT_TRUE(result["simulation"]["unresolved"].empty());
    ASSERT_TRUE(result["onebot"].get<std::string>().find("Max{1d6}=6") != std::string::npos);
}

TEST(TemplateSimulation, StablePlatformViewsAndResamplingUseIndependentRandomness) {
    auto &i18n = previewTranslations();
    json request{{"variants", json::array({{{"text", "NEVER"}, {"weight", 0}},
                                           {{"text", "{sample:甲|乙} {result} {ran:1~10000}"}, {"weight", 1}}})},
                 {"simulation", example("dice.roll.result")}};
    const auto first = outbound::templatePreview(request, &i18n);
    request["platform"] = "plain";
    ASSERT_EQ(first["onebot"].get<std::string>(),
              outbound::templatePreview(request, &i18n)["onebot"].get<std::string>());
    std::set<std::string> samples;
    for (unsigned seed = 0; seed < 40; ++seed) {
        request["simulation"]["seed"] = seed;
        samples.insert(outbound::templatePreview(request, &i18n)["onebot"].get<std::string>());
    }
    ASSERT_TRUE(samples.size() > 20);
}

TEST(TemplateSimulation, UnknownVariablesAreReportedAndNeverInventedOrEvaluated) {
    auto &i18n = previewTranslations();
    const auto result =
        outbound::templatePreview({{"text", "{loss} {not_a_variable} {var:local=ok}{local} {strSuccess}"},
                                   {"simulation", example("card.sc.result")}},
                                  &i18n);
    ASSERT_EQ(result["simulation"]["unresolved"].dump(), "[\"not_a_variable\"]");
    ASSERT_TRUE(result["onebot"].get<std::string>().find("{not_a_variable}") != std::string::npos);
    ASSERT_TRUE(result["onebot"].get<std::string>().find("{loss}") == std::string::npos);
    // Existing literal-argument previews retain injection isolation.
    const auto literal =
        outbound::templatePreview({{"text", "{nick}"}, {"args", {{"nick", "{sample:BAD|BAD}"}}}}, &i18n);
    ASSERT_EQ(literal["onebot"].get<std::string>(), "{sample:BAD|BAD}");
}

TEST(TemplateSimulation, RejectsMalformedAndConflictingSimulationInputs) {
    for (const auto &request :
         {json(nullptr), json::array(), json{{"version", 2}}, example(""),
          json{{"version", 1}, {"key", "card.sc.result"}, {"seed", -1}},
          json{{"version", 1}, {"key", "card.sc.result"}, {"seed", 4294967296ULL}},
          json{{"version", 1}, {"key", "x"}, {"outcome", {{"family", "x"}, {"grade", "failure"}}}}}) {
        bool threw = false;
        try {
            outbound::templatePreviewContext(request);
        } catch (const std::exception &) {
            threw = true;
        }
        ASSERT_TRUE(threw);
    }
    bool threw = false;
    try {
        outbound::templatePreview(
            {{"text", "{loss}"}, {"simulation", example("card.sc.result")}, {"args", {{"loss", "999"}}}});
    } catch (const std::exception &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(TemplateSimulation, PersonaGradeTextAppliesToGeneratedArgumentsAndDoesNotLeak) {
    auto& i18n = previewTranslations();
    i18n.setPersonaBundles(7999, Locale::kZhHans, {{"dice.level.failure", "PERSONA_FAILURE"}});
    const auto request = example("dice.outcome.sanity.failure");
    const auto result = outbound::templatePreview({{"text", "{level} {grade} {loss}"}, {"simulation", request}, {"personaId", 7999}}, &i18n);
    ASSERT_EQ(result["simulation"]["args"]["level"].get<std::string>(), "PERSONA_FAILURE");
    ASSERT_TRUE(result["onebot"].get<std::string>().find("PERSONA_FAILURE") != std::string::npos);
    const auto global = outbound::templatePreview({{"text", "{level}"}, {"simulation", request}}, &i18n);
    ASSERT_NE(global["onebot"].get<std::string>(), "PERSONA_FAILURE");
    i18n.clearPersonaBundles(7999);
}

TEST(TemplateSimulation, NestedSanityAndCheckOutputsDoNotLeakMarkdownIntoPlainViews) {
    auto &i18n = previewTranslations();
    for (const auto locale : {Locale::kZhHans, Locale::kZhHant, Locale::kEn, Locale::kJa})
        for (const auto *key : {"dice.sc.result", "dice.st.result"})
            for (const auto *format : {"plain", "markdown"}) {
                // Plain-authored wrappers may intentionally contain literal
                // stars. Exercise a plain wrapper and the actual Markdown
                // wrapper separately instead of reclassifying authored text.
                auto request = json{{"text",std::string(format) == "plain" ? "{output}" : i18n.getDefault(locale,key)},
                                    {"format", format},
                                    {"simulation", example(key)},
                                    {"locale", localeToString(locale)},
                                    {"forcePlain", true}};
                const auto result = outbound::templatePreview(request, &i18n);
                ASSERT_TRUE(result["simulation"]["unresolved"].empty());
                for (const auto *view : {"onebot", "preview"}) {
                    const auto text = std::string(view) == "preview" ? result[view]["text"].get<std::string>()
                                                                     : result[view].get<std::string>();
                    ASSERT_TRUE(text.find("**") == std::string::npos);
                    ASSERT_TRUE(text.find('`') == std::string::npos);
                    ASSERT_TRUE(text.find("{output}") == std::string::npos);
                    ASSERT_TRUE(!text.empty());
                }
                ASSERT_FALSE(result["preview"]["markdown"].get<bool>());
            }
}
TEST(TemplateSimulation, ComputedFragmentsRespectMarkdownAndPlainOverridesSeparately) {
    // A private translation instance prevents override state leaking to another test.
    I18n i18n("");
    i18n.setOverride(Locale::kZhHans, "card.sc.result", "**{nick}**：`{roll}`；损失 **{loss}**",
                     ContentFormat::kMarkdown);
    const auto markdown = outbound::templatePreview(
        {{"text", "{output}"}, {"format", "markdown"}, {"simulation", example("dice.sc.result")}}, &i18n);
    auto text = markdown["onebot"].get<std::string>();
    ASSERT_TRUE(text.find("**") == std::string::npos);
    ASSERT_TRUE(text.find('`') == std::string::npos);
    ASSERT_TRUE(text.find("损失") != std::string::npos);
    i18n.setOverride(Locale::kZhHans, "card.sc.result", "literal **keep** 2**3 {loss}",
                     ContentFormat::kPlainText);
    const auto plain = outbound::templatePreview(
        {{"text", "{output}"}, {"format", "markdown"}, {"simulation", example("dice.sc.result")}}, &i18n);
    ASSERT_TRUE(plain["onebot"].get<std::string>().find("literal **keep** 2**3") != std::string::npos);
}
TEST(TemplateSimulation, MarkdownConversionDoesNotReinterpretInsertedUserData) {
    const std::string literal = "**昵称** 2**3 {sample:BAD|BAD} [CQ:image,file=/data/images/test.png]";
    const auto result = outbound::templatePreview(
        {{"text", "**{nick}**"}, {"format", "markdown"}, {"args", {{"nick", literal}}}});
    ASSERT_EQ(result["onebot"].get<std::string>(), literal);
    const auto plain = outbound::templatePreview({{"text", "**literal plain**"}, {"format", "plain"}});
    ASSERT_EQ(plain["onebot"].get<std::string>(), "**literal plain**");
}

TEST(TemplateSimulation, StyledGradeFragmentsDoNotLeakMarkdownFromOverridesOrPersonas) {
    I18n i18n("");
    i18n.setOverride(Locale::kZhHans,"dice.level.failure","**失败**",ContentFormat::kMarkdown);
    auto request = json{{"text","{level}"},{"format","markdown"},
        {"simulation",example("dice.outcome.standard.failure")}};
    const auto global = outbound::templatePreview(request,&i18n);
    ASSERT_EQ(global["onebot"].get<std::string>(),"失败");
    i18n.setPersonaBundles(7998,Locale::kZhHans,{{"dice.level.failure","**人格失败**"}},
        {{"dice.level.failure","markdown"}});
    request["personaId"] = 7998;
    const auto persona = outbound::templatePreview(request,&i18n);
    ASSERT_EQ(persona["onebot"].get<std::string>(),"人格失败");
    i18n.setOverride(Locale::kZhHans,"dice.level.failure","**字面文本**",ContentFormat::kPlainText);
    request["personaId"] = 0;
    const auto literal = outbound::templatePreview(request,&i18n);
    ASSERT_EQ(literal["onebot"].get<std::string>(),"**字面文本**");
}
