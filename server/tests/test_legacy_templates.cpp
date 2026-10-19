#include "test_framework.h"
#include "i18n/i18n.h"
#include "common/legacy_deck_generator.h"
#include "adapter/onebot_member_snapshot.h"
#include "common/deferred_reply_queue.h"

using namespace dice;

TEST(LegacyTemplates, PureFunctionsAndLocalValuesNeverReparseData) {
    ASSERT_EQ(I18n::previewTemplate("{ran:7~7}"), "7");
    ASSERT_EQ(I18n::previewTemplate("{ran:+7~+7}"), "7");
    ASSERT_EQ(I18n::previewTemplate("{ran:9~1}"), "");
    ASSERT_EQ(I18n::previewTemplate("{len:nick}", {{"nick", "测试"}}), "6");
    ASSERT_EQ(I18n::previewTemplate("{expired:until}", {{"until", "1"}}), "true");
    ASSERT_EQ(I18n::previewTemplate("{expired:until}", {{"until", "9223372036854775807"}}), "false");
    ASSERT_EQ(I18n::previewTemplate("{var:n=12}{case:n?12=OK&else=NO}/{grade:n?0=LOW&10=HIGH}"), "OK/HIGH");
    for (int i = 0; i < 30; ++i) {
        const auto result = I18n::previewTemplate("{var:n={ran:1~2}}{n}/{n}");
        ASSERT_TRUE(result == "1/1" || result == "2/2");
    }
    const std::string data = "{sample:INJECTED}*value*";
    ASSERT_EQ(I18n::previewTemplate("{var:x?{nick}}|{x}", {{"nick", data}}), data + "|" + data);
    ASSERT_EQ(I18n::previewTemplate("{var:n=first}{n}{var:n=second}{n}"), "firstsecond");
    ASSERT_EQ(I18n::previewTemplate("{n}"), "{n}"); // No cross-render locals.
    ASSERT_EQ(I18n::previewTemplate("{wait:1}{js:1}{py:1}"), "{js:1}{py:1}");
    ASSERT_EQ(I18n::previewTemplate(R"(\{ran:1~2})"), R"(\{ran:1~2})");
    ASSERT_EQ(I18n::previewTemplate(R"({nick} \{nick})",{{"nick","reader"}}), R"(reader \{nick})");
    std::string oversized = "{sample:";
    for (int i = 0; i < 5000; ++i) oversized += "a|";
    oversized += "a}";
    ASSERT_FALSE(safe_template::compile(oversized).issues.empty());
}

TEST(LegacyTemplates, MentionValidationIdentityAndMarkdownAreConsistent) {
    ASSERT_EQ(I18n::previewTemplate("{at:}{at:self}", {{"uid", "123"}, {"selfId", "456"}}), "[CQ:at,qq=123][CQ:at,qq=456]");
    ASSERT_EQ(I18n::previewTemplate("{at:{uid}}", {{"uid", "1][CQ:image,file=evil"}}), "");
    ASSERT_EQ(I18n::previewTemplate("{print:uid}/{print:gid}", {{"uid", "123"}, {"print.uid", "Alice(123)"}, {"gid", "456"}}), "Alice(123)/456");
    ASSERT_EQ(I18n::previewTemplate("{print:gid&uid=123}", {{"print.uid=123","Alice(123)"}}), "Alice(123)");
    ASSERT_EQ(I18n::previewTemplate("{print:uid=1][CQ:at,qq=all}"), "");
    ASSERT_EQ(I18n::previewTemplate("{case:n?invalid}/{grade:n?bad=x}", {{"n","must-not-replace-invalid"}}), "{case:n?invalid}/{grade:n?bad=x}");
    ASSERT_EQ(I18n::previewTemplate("**{var:x?{nick}}** `{x}`", {{"nick", "A*B"}}, ContentFormat::kMarkdown), "**A\\*B** `A*B`");
    {
        I18n::TemplateContextScope context([](const std::string& key) -> std::optional<std::string> {
            if (key == "user.favor") return "20";
            return std::nullopt;
        });
        ASSERT_EQ(I18n::previewTemplate("{grade:user.favor?0=陌生&20=朋友}"), "朋友");
    }
    ASSERT_EQ(I18n::previewTemplate("{user.favor}"), "{user.favor}");
}

TEST(LegacyTemplates, TypedLengthsStayScopedAndExplicitValuesWin) {
    I18n::TemplateContextScope context({}, [](const std::string& key) -> std::optional<size_t> {
        if (key == "game.pls") return 3;
        return std::nullopt;
    });
    ASSERT_EQ(I18n::previewTemplate("{len:game.pls}"), "3");
    ASSERT_EQ(I18n::previewTemplate("{len:game.pls}", {{"game.pls","override"}}), "8");
    ASSERT_EQ(I18n::previewTemplate("{var:local=ok}{len:local}"), "2");
    {
        I18n::TemplateContextScope empty({});
        ASSERT_EQ(I18n::previewTemplate("{len:game.pls}"), "0");
    }
    ASSERT_EQ(I18n::previewTemplate("{len:game.pls}"), "3");
}

TEST(LegacyTemplates, WaitRecordsOnlySelectedAuthoredBranchesWithoutSleeping) {
    ASSERT_EQ(I18n::previewTemplate("{wait:{var:x=7}}{x}"), "7");
    safe_template::Delay delay;
    safe_template::DelayScope scope(&delay);
    ASSERT_EQ(I18n::previewTemplate("before{wait:10}{case:n?1={wait:20}yes&else={wait:100}no}after",{{"n","1"}}), "beforeyesafter");
    ASSERT_EQ(delay.milliseconds,30u);
    ASSERT_EQ(I18n::previewTemplate("{nick}",{{"nick","{wait:500}"}}), "{wait:500}");
    ASSERT_EQ(delay.milliseconds,30u);
    I18n::previewTemplate("{wait:-1}{wait:600000}{wait:invalid}");
    ASSERT_EQ(delay.milliseconds,30u);
    I18n::previewTemplate("{wait:500000}{wait:500000}");
    ASSERT_EQ(delay.milliseconds,600000u);
}

TEST(LegacyTemplates, DeferredRepliesBoundMemoryCountAndCancelOnShutdown) {
    std::vector<std::function<void()>> callbacks;
    int delivered=0;
    DeferredReplyQueue queue([&](double seconds,std::function<void()> callback) {
        if (seconds!=0.01) throw std::runtime_error("unexpected delay");
        callbacks.push_back(std::move(callback));
    });
    for (int i=0;i<256;++i) ASSERT_TRUE(queue.post(10,1,[&] { ++delivered; }));
    ASSERT_EQ(queue.pending(),size_t(256));
    ASSERT_FALSE(queue.post(10,1,[] {}));
    ASSERT_FALSE(queue.post(600001,1,[] {}));
    ASSERT_FALSE(queue.post(10,17*1024*1024,[] {}));
    auto callback=std::move(callbacks.back()); callbacks.pop_back(); callback(); callback={};
    ASSERT_EQ(delivered,1); ASSERT_EQ(queue.pending(),size_t(255));
    queue.cancel(); for (auto& item : callbacks) item(); callbacks.clear();
    ASSERT_EQ(delivered,1); ASSERT_EQ(queue.pending(),size_t(0));
    ASSERT_FALSE(queue.post(10,1,[] {}));
    DeferredReplyQueue memory([&](double,std::function<void()> item) { callbacks.push_back(std::move(item)); });
    ASSERT_TRUE(memory.post(10,16*1024*1024,[] {}));
    ASSERT_FALSE(memory.post(10,1,[] {})); callbacks.clear();
    ASSERT_EQ(memory.pending(),size_t(0));
    DeferredReplyQueue broken([](double,std::function<void()>) { throw std::runtime_error("scheduler unavailable"); });
    ASSERT_FALSE(broken.post(10,1,[] {})); ASSERT_EQ(broken.pending(),size_t(0));
}

TEST(LegacyTemplates, DeferredRepliesCannotChangeAccountsOrOutlivePermissionSnapshots) {
    auto original=std::make_shared<int>(1), replacement=std::make_shared<int>(2);
    const DeferredReplyAuthority authority{original,"bot-A","plugin-enabled"};
    ASSERT_TRUE(authority.matches(original,"bot-A","plugin-enabled"));
    ASSERT_FALSE(authority.matches(replacement,"bot-A","plugin-enabled"));
    ASSERT_FALSE(authority.matches(original,"bot-B","plugin-enabled"));
    ASSERT_FALSE(authority.matches(original,"bot-A","plugin-disabled"));
    std::vector<std::function<void()>> callbacks;
    DeferredReplyQueue queue([&](double,auto callback) { callbacks.push_back(std::move(callback)); });
    std::string permissions="plugin-enabled";
    int sent=0;
    ASSERT_TRUE(queue.post(10,1,[&] {
        if (authority.matches(original,"bot-A",permissions)) ++sent;
    }));
    permissions="plugin-disabled";
    auto callback=std::move(callbacks.back()); callbacks.clear(); callback(); callback={};
    ASSERT_EQ(sent,0);
    ASSERT_EQ(queue.pending(),size_t(0));
    original.reset();
    ASSERT_FALSE(authority.matches(replacement,"bot-A","plugin-enabled"));
}

TEST(LegacyTemplates, RangeGenerationIsBoundedAndLiteralCardsDoNotInterpretWeights) {
    std::string error;
    ASSERT_TRUE(session_deck::rangeCards("1 5 2", error) == std::vector<std::string>({"1","3","5"}));
    ASSERT_TRUE(session_deck::rangeCards("5 1 2", error) == std::vector<std::string>({"1","3","5"}));
    ASSERT_TRUE(session_deck::rangeCards("2", error) == std::vector<std::string>({"0","1","2"}));
    ASSERT_TRUE(session_deck::rangeCards("-9223372036854775808 9223372036854775807", error).empty());
    ASSERT_TRUE(session_deck::rangeCards("1 4 0", error).empty());
    ASSERT_TRUE(session_deck::rangeCards("1 4 -1", error).empty());
    ASSERT_TRUE(session_deck::rangeCards("1 4 1 extra", error).empty());
    ASSERT_EQ(session_deck::rangeCards("9223372036854775806 9223372036854775807", error).size(), size_t(2));
    const auto deck = session_deck::create({"::999999::literal{sample:still data}"}, {}, error, true);
    ASSERT_TRUE(deck.is_object()); ASSERT_EQ(deck["size"].get<int>(), 1);
    ASSERT_EQ(deck["meta"][0].get<std::string>(), "::999999::literal{sample:still data}");
    ASSERT_EQ(session_deck::memberLabel("A[CQ:at,qq=all]\fB"), "A［CQ:at,qq=all] B");
}

TEST(LegacyTemplates, OneBotMemberMetadataSkipsInvalidIdsAndRetainsLiteralNames) {
    const auto rows = nlohmann::json::array({{{"user_id",123},{"nickname","base"},{"card","{ran:9~9}"}},
        {{"user_id","123"}}, {{"user_id",456},{"nickname","second"}}, {{"user_id",0}}, {{"user_id","broken"}}, false});
    ASSERT_TRUE(onebot_members::ids(rows) == std::vector<std::string>({"123","456"}));
    ASSERT_EQ(onebot_members::name(rows,"123"), "{ran:9~9}");
    ASSERT_EQ(onebot_members::name(rows,"456"), "second");
    ASSERT_EQ(onebot_members::name(rows,"999"), "");
    const auto snapshot = onebot_members::snapshot(rows);
    ASSERT_TRUE(snapshot.ids == std::vector<std::string>({"123","456"}));
    ASSERT_EQ(snapshot.names.at("123"), "{ran:9~9}");
}
