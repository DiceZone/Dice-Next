#include "test_framework.h"
#include <limits>
#include "../src/adapter/qq_official_adapter.h"
#include "../src/adapter/adapter_manager.h"
#include "../src/storage/group_account_settings.h"

using namespace dice;

TEST(QQOfficialAdapter, BuildsQueryStringForGetEndpoints) {
    // 官方 GET 接口（分页游标等）只读查询串，不读请求体。
    const std::string query = QQOfficialAdapter::queryStringForTest(
        json{{"cursor", "abc"}, {"limit", 30}});
    ASSERT_EQ(query, std::string("cursor=abc&limit=30"));

    // 布尔要写成 true/false，不是 1/0。
    ASSERT_EQ(QQOfficialAdapter::queryStringForTest(json{{"flag", true}}), std::string("flag=true"));

    // 对象与数组无法出现在查询串里，跳过而不是塞一段 JSON 进去。
    ASSERT_EQ(QQOfficialAdapter::queryStringForTest(
        json{{"a", "1"}, {"nested", json::object()}, {"list", json::array({1, 2})}}),
        std::string("a=1"));

    ASSERT_EQ(QQOfficialAdapter::queryStringForTest(json::object()), std::string());
}

TEST(QQOfficialAdapter, PercentEncodesQueryValues) {
    ASSERT_EQ(QQOfficialAdapter::urlEncodeForTest("a b"), std::string("a%20b"));
    ASSERT_EQ(QQOfficialAdapter::urlEncodeForTest("a&b=c"), std::string("a%26b%3Dc"));
    // 未保留字符必须原样留下，否则 openid 会被改写。
    ASSERT_EQ(QQOfficialAdapter::urlEncodeForTest("A1b2-_.~"), std::string("A1b2-_.~"));
}

TEST(QQOfficialAdapter, MapsExtensionsToOfficialFileTypes) {
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(1, "a.png"), 1);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(1, "a.JPG"), 1);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(2, "a.mp4"), 2);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(3, "a.silk"), 3);
    // 官方只认 png/jpg、mp4、silk；其余一律降级成文件类型，而不是报错丢弃。
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(1, "a.psd"), 4);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(2, "a.mkv"), 4);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(4, "a.zip"), 4);
}

TEST(QQOfficialAdapter, SanitisesMediaFileNames) {
    // 无后缀时补该类型的默认后缀，否则官方按后缀判类型会失败。
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("", 1, "data/assets/abc"), std::string("abc.png"));
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("", 2, "data/assets/clip"), std::string("clip.mp4"));
    // 路径分隔符与保留字符会被换掉，避免拼出非法文件名。
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("a/b:c.png", 1, ""), std::string("a_b_c.png"));
    // 显式文件名优先于路径 basename。
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("report.pdf", 4, "data/assets/xyz"), std::string("report.pdf"));
}

TEST(QQOfficialAdapter, SplitsMediaCodesAndKeepsUnsendableOnes) {
    const json parts = QQOfficialAdapter::splitMediaForTest(
        "before[CQ:image,file=https://example.test/a.png]middle"
        "[file,url=https://example.test/b.pdf,name=b.pdf]after");
    ASSERT_EQ(parts["text"].get<std::string>(), std::string("beforemiddleafter"));
    ASSERT_EQ(parts["media"].size(), static_cast<size_t>(2));
    ASSERT_EQ(parts["media"][0]["kind"].get<int>(), 1);
    ASSERT_EQ(parts["media"][0]["ref"].get<std::string>(), std::string("https://example.test/a.png"));
    ASSERT_EQ(parts["media"][1]["kind"].get<int>(), 4);
    ASSERT_EQ(parts["media"][1]["name"].get<std::string>(), std::string("b.pdf"));

    // 没有实体引用的码（入站群文件记录）发不出去，原样留在文本里而不是吞掉。
    const json kept = QQOfficialAdapter::splitMediaForTest("x[CQ:file,name=doc.txt,id=42]y");
    ASSERT_EQ(kept["media"].size(), static_cast<size_t>(0));
    ASSERT_EQ(kept["text"].get<std::string>(), std::string("x[CQ:file,name=doc.txt,id=42]y"));
}

TEST(QQOfficialAdapter, KeepsOfficialHardSizeLimit) {
    // 官方硬上限 200MB，超过直接报 850031；预检靠这个常量。
    ASSERT_EQ(QQOfficialAdapter::mediaHardLimitForTest(), static_cast<size_t>(200) * 1024 * 1024);
}

namespace {
struct CardModeScope {
    bool previous = IAdapter::cardMessageMode();
    CardModeScope() { IAdapter::setCardMessageMode(true); }
    ~CardModeScope() { IAdapter::setCardMessageMode(previous); }
};
Message statusReply() {
    Message msg;
    msg.platform = "qq_official"; msg.targetId = "canonical-group"; msg.senderId = "canonical-user";
    msg.extra = {{"__identity_native_sender", "real-openid"}};
    auto card = std::make_shared<qq_rich::Card>();
    card->title = "调查员"; card->original = "生命值已修改：7"; card->footer = "已绑卡";
    card->vitals = {{"HP", 7, 14}, {"SAN", 65, 99}};
    card->changes = {{"生命值", 14, 7}};
    card->commands = {{"修改属性", ".st "}, {"查看状态", ".pc status"}};
    msg.qqRichReply = card;
    return msg;
}
}

TEST(QQRichReply, PayloadUsesOptInAndKeepsExactPlainFallback) {
    CardModeScope mode;
    QQOfficialAdapter adapter("test");
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"}}));
    auto msg = statusReply();
    auto original = msg.qqRichReply->original;
    auto body = adapter.textPayload(msg, original, ContentFormat::kPlainText);
    ASSERT_FALSE(body.contains("keyboard"));
    ASSERT_TRUE(body["markdown"]["content"].get<std::string>().find("HP") == std::string::npos);
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"}, {"qqRichReplies", "math"}, {"qqInteractions", "links"}}));
    body = adapter.textPayload(msg, original, ContentFormat::kPlainText);
    auto text = body["markdown"]["content"].get<std::string>();
    ASSERT_TRUE(text.find("\\rule{50px}") != std::string::npos);
    ASSERT_TRUE(text.find("7/14") != std::string::npos);
    ASSERT_TRUE(text.find("<qqbot-cmd-input") != std::string::npos);
    ASSERT_TRUE(text.find("<qqbot-cmd-enter") == std::string::npos);
    body = adapter.textPayload(msg, original, ContentFormat::kPlainText, 1);
    text = body["markdown"]["content"].get<std::string>();
    ASSERT_TRUE(text.find("\\rule") == std::string::npos);
    ASSERT_TRUE(text.find("qqbot-cmd") == std::string::npos);
    body = adapter.textPayload(msg, original, ContentFormat::kPlainText, 2);
    ASSERT_FALSE(body.contains("markdown"));
    ASSERT_FALSE(body.contains("keyboard"));
    ASSERT_EQ(body["content"].get<std::string>(), original);
    IAdapter::setCardMessageMode(false);
    ASSERT_FALSE(adapter.textPayload(msg, original, ContentFormat::kPlainText).contains("markdown"));
}

TEST(QQRichReply, ConversationPlainModeSuppressesMarkdownCardAndKeyboard) {
    CardModeScope mode;
    QQOfficialAdapter adapter("test");
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"},
        {"qqRichReplies", "math"}, {"qqInteractions", "buttons"}}));
    auto msg = statusReply();
    msg.forcePlainText = true;
    auto body = adapter.textPayload(msg, msg.qqRichReply->original, ContentFormat::kPlainText);
    ASSERT_FALSE(body.contains("markdown"));
    ASSERT_FALSE(body.contains("keyboard"));
    ASSERT_EQ(body["content"].get<std::string>(), msg.qqRichReply->original);
    body = adapter.textPayload(msg, "**结果**：`1D1=1`", ContentFormat::kMarkdown);
    ASSERT_EQ(body["content"].get<std::string>(), "结果：1D1=1");
    msg.forcePlainText = false;
    adapter.plainTextResolver = [](const Message&) { return true; };
    ASSERT_FALSE(adapter.textPayload(msg, "text", ContentFormat::kPlainText).contains("markdown"));
    msg.textPreferenceCaptured = true; // In-flight reply retains the mode captured at receipt.
    ASSERT_TRUE(adapter.textPayload(msg, "text", ContentFormat::kPlainText).contains("markdown"));
    IAdapter::setCardMessageMode(false);
    ASSERT_FALSE(adapter.textPayload(msg, "text", ContentFormat::kPlainText).contains("markdown"));
}

TEST(QQRichReply, DirectSendsResolveStoredDestinationWithoutCrossingAccountsOrPrivateChats) {
    CardModeScope mode;
    struct TempDir {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("dice-text-destination-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        TempDir() { std::filesystem::create_directory(path); }
        ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } dir;
    Database db;
    ASSERT_TRUE(db.open((dir.path / "test.db").string()));
    AdapterManager manager(db);
    auto adapter = std::make_shared<QQOfficialAdapter>("one");
    ASSERT_TRUE(adapter->configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"}}));
    manager.registerAdapter(adapter);
    setAccountGroupSetting(*db.getStorage(), "one", "qq_official", "public-group", "native-group",
        "replyTextGroup", "1");
    Message msg; msg.targetId = "public-group";
    ASSERT_FALSE(adapter->textPayload(msg, "**text**", ContentFormat::kMarkdown).contains("markdown"));
    msg.targetId = "native-group";
    ASSERT_FALSE(adapter->textPayload(msg, "**text**", ContentFormat::kMarkdown).contains("markdown"));
    msg.type = MessageType::kPrivate;
    ASSERT_TRUE(adapter->textPayload(msg, "**text**", ContentFormat::kMarkdown).contains("markdown"));
    msg.type = MessageType::kGroup; msg.targetId = "different-group";
    ASSERT_TRUE(adapter->textPayload(msg, "**text**", ContentFormat::kMarkdown).contains("markdown"));
    auto second = std::make_shared<QQOfficialAdapter>("two");
    ASSERT_TRUE(second->configure({{"appId", "test2"}, {"appSecret", "not-a-real-secret"}}));
    manager.registerAdapter(second);
    msg.targetId = "public-group";
    ASSERT_TRUE(second->textPayload(msg, "**text**", ContentFormat::kMarkdown).contains("markdown"));
}

TEST(QQRichReply, ButtonsAreConfirmableAndRestrictedToTransportSender) {
    CardModeScope mode;
    QQOfficialAdapter adapter("test");
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"}, {"qqRichReplies", "markdown"}, {"qqInteractions", "buttons"}}));
    auto msg = statusReply();
    auto body = adapter.textPayload(msg, msg.qqRichReply->original, ContentFormat::kPlainText);
    auto action = body["keyboard"]["content"]["rows"][0]["buttons"][0]["action"];
    ASSERT_EQ(action["type"].get<int>(), 2);
    ASSERT_FALSE(action["enter"].get<bool>());
    ASSERT_EQ(action["permission"]["type"].get<int>(), 0);
    ASSERT_EQ(action["permission"]["specify_user_ids"][0].get<std::string>(), "real-openid");
    ASSERT_EQ(action["data"].get<std::string>(), ".st ");
    msg.extra = json::object();
    ASSERT_FALSE(adapter.textPayload(msg, msg.qqRichReply->original, ContentFormat::kPlainText).contains("keyboard"));
    ASSERT_FALSE(adapter.textPayload(msg, "plugin replacement", ContentFormat::kPlainText).contains("keyboard"));
    msg.type = MessageType::kChannel;
    ASSERT_FALSE(adapter.textPayload(msg, msg.qqRichReply->original, ContentFormat::kPlainText).contains("markdown"));
}

TEST(QQRichReply, GenericUsageActionsBecomePrefillControlsOnlyOnOfficialRichOutput) {
    CardModeScope mode;
    QQOfficialAdapter adapter("test");
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"},
        {"qqRichReplies", "markdown"}, {"qqInteractions", "buttons"}}));
    Message msg;
    msg.platform = "qq_official";
    msg.type = MessageType::kGroup;
    msg.extra = {{"__identity_native_sender", "real-openid"}};
    msg.presentationActions = {{"新建人物卡", ".pc new "}, {"查看人物卡", ".pc show"}};
    const std::string usage = ".pc new <名字> // 新建人物卡";
    auto body = adapter.textPayload(msg, usage, ContentFormat::kMarkdown);
    ASSERT_TRUE(body.contains("keyboard"));
    ASSERT_EQ(body["keyboard"]["content"]["rows"][0]["buttons"][0]["action"]["data"].get<std::string>(),
              std::string(".pc new "));
    ASSERT_FALSE(body["keyboard"]["content"]["rows"][0]["buttons"][0]["action"]["enter"].get<bool>());

    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"},
        {"qqRichReplies", "markdown"}, {"qqInteractions", "links"}}));
    body = adapter.textPayload(msg, usage, ContentFormat::kMarkdown);
    ASSERT_TRUE(body["markdown"]["content"].get<std::string>().find(
        "<qqbot-cmd-input text=\".pc%20new%20\"") != std::string::npos);

    msg.forcePlainText = true;
    ASSERT_FALSE(adapter.textPayload(msg, usage, ContentFormat::kMarkdown).contains("keyboard"));
    ASSERT_FALSE(adapter.textPayload(msg, usage, ContentFormat::kMarkdown).contains("markdown"));
    msg.forcePlainText = false;
    msg.platform = "onebot_v11";
    body = adapter.textPayload(msg, usage, ContentFormat::kMarkdown);
    ASSERT_FALSE(body.contains("keyboard"));
    ASSERT_TRUE(body["markdown"]["content"].get<std::string>().find("qqbot-cmd-input") == std::string::npos);
}

TEST(QQRichReply, UsageKeyboardFitsTenSafeShortLabels) {
    std::vector<qq_rich::Command> commands;
    for (int i = 0; i < 12; ++i)
        commands.push_back({"这是一个很长的操作按钮" + std::to_string(i), ".pc action " + std::to_string(i)});
    const auto keys = qq_rich::keyboard(commands, "real-openid");
    ASSERT_EQ(keys["content"]["rows"].size(), static_cast<size_t>(5));
    ASSERT_EQ(keys["content"]["rows"][4]["buttons"].size(), static_cast<size_t>(2));
    ASSERT_EQ(keys["content"]["rows"][0]["buttons"][0]["render_data"]["label"].get<std::string>(),
              std::string("这是一个很长的操作按"));
}

TEST(QQRichReply, DynamicTextCannotInjectActionsImagesOrMath) {
    qq_rich::Card card;
    card.title = "<qqbot-cmd-enter text=\"attack\" /> $\\color{red}$ [CQ:image,file=https://example.test/a]";
    card.original = card.title;
    card.vitals = {{"HP", -2, 14}, {"SAN", 120, 99}, {"MP", 1, 0}};
    card.commands = {{"bad\" label", ".st \"quoted\" &value"}};
    const auto text = qq_rich::render(card, true, true);
    ASSERT_TRUE(text.find("<qqbot-cmd-enter") == std::string::npos);
    ASSERT_TRUE(text.find("[CQ:image") == std::string::npos);
    ASSERT_TRUE(text.find("$\\color{red}$") == std::string::npos);
    ASSERT_TRUE(text.find("%22quoted%22%20%26value") != std::string::npos);
    ASSERT_EQ(qq_rich::filledCells(card.vitals[0]), 0);
    ASSERT_EQ(qq_rich::filledCells(card.vitals[1]), 10);
    ASSERT_EQ(qq_rich::bar(card.vitals[2], true), std::string());
    ASSERT_TRUE(text.find("120/99") != std::string::npos);
    ASSERT_EQ(qq_rich::signedDelta({"x", (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()}), "+4294967295");
}

TEST(QQRichReply, OversizePresentationFallsBackWithoutTruncation) {
    CardModeScope mode;
    QQOfficialAdapter adapter("test");
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "not-a-real-secret"}, {"qqRichReplies", "math"}}));
    auto msg = statusReply();
    auto card = std::make_shared<qq_rich::Card>(*msg.qqRichReply);
    card->title = std::string(4000, 'x');
    msg.qqRichReply = card;
    auto body = adapter.textPayload(msg, card->original, ContentFormat::kPlainText);
    ASSERT_FALSE(body.contains("markdown"));
    ASSERT_EQ(body["content"].get<std::string>(), card->original);
}

TEST(QQRichReply, CompactCardKeepsTrueValuesAndBoundedNumericMath) {
    auto card = *statusReply().qqRichReply;
    card.body = "";
    card.changes = {{"生命值", 14, 12}, {"危险$\\href{url}{x}", 12, 14}};
    card.vitals = {{"HP", 12, 14}, {"SAN", 65, 99}};
    auto text = qq_rich::render(card, true, true);
    ASSERT_TRUE(text.find("\\rule{86px}") != std::string::npos);
    ASSERT_TRUE(text.find("\\rule{66px}") != std::string::npos);
    ASSERT_TRUE(text.find("14\\xrightarrow{-2}\\textcolor{#EF4444}{12}") != std::string::npos);
    ASSERT_TRUE(text.find("12\\xrightarrow{+2}\\textcolor{#10B981}{14}") != std::string::npos);
    ASSERT_TRUE(text.find("\\href") == std::string::npos);
    ASSERT_TRUE(text.find(card.original) == std::string::npos);
    ASSERT_TRUE(text.find(" /> · <qqbot-cmd-input") != std::string::npos);
    text = qq_rich::render(card, false, false);
    ASSERT_TRUE(text.find("14 → **12** (-2)") != std::string::npos);
    ASSERT_TRUE(text.find("\\xrightarrow") == std::string::npos);
}

TEST(QQRichReply, ForeignPlatformAndExplicitMarkdownCannotConsumeStructuredCard) {
    CardModeScope mode;
    QQOfficialAdapter adapter("test");
    ASSERT_TRUE(adapter.configure({{"appId", "test"}, {"appSecret", "test"}, {"qqRichReplies", "math"}, {"qqInteractions", "buttons"}}));
    auto msg = statusReply();
    const auto original = msg.qqRichReply->original;
    for (const auto* platform : {"onebot_v11", "milky", "discord", "kook"}) {
        msg.platform = platform;
        auto body = adapter.textPayload(msg, original, ContentFormat::kPlainText);
        ASSERT_FALSE(body.contains("keyboard"));
        ASSERT_EQ(body["markdown"]["content"].get<std::string>(), original);
    }
    msg.platform = "qq_official";
    auto body = adapter.textPayload(msg, original, ContentFormat::kMarkdown);
    ASSERT_FALSE(body.contains("keyboard"));
    ASSERT_EQ(body["markdown"]["content"].get<std::string>(), original);
}
