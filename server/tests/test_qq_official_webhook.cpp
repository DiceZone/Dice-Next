#include "test_framework.h"
#include "../src/adapter/qq_official_webhook.h"
#include "../src/adapter/qq_official_adapter.h"

#include <memory>

using namespace dice;

TEST(QQOfficialWebhook, SignsTheOfficialValidationVector) {
    const std::string signature = qqwebhook::validationSignature(
        "DG5g3B4j9X2KOErG", "1725442341", "Arq0D5A61EgUu4OxUvOp");
    ASSERT_EQ(signature, std::string("87befc99c42c651b3aac0278e71ada338433ae26fcb24307bdc5ad38c1adc2d01bcfcadc0842edac85e85205028a1132afe09280305f13aa6909ffc2d652c706"));
}

TEST(QQOfficialWebhook, VerifiesTheOfficialCallbackVector) {
    const std::string secret = "naOC0ocQE3shWLAfffVLB1rhYPG7";
    // 文档把这段 JSON 折成了一行；实际签名对应的是带两个空格缩进、末尾换行的原文。
    const std::string body = "{\n  \"op\": 0,\n  \"d\": {},\n  \"t\": \"GATEWAY_EVENT_NAME\"\n}";
    const std::string signature = "865ad13a61752ca65e26bde6676459cd36cf1be609375b37bd62af366e1dc25a8dc789ba7f14e017ada3d554c671a911bfdf075ba54835b23391d509579ed002";
    ASSERT_EQ(qqwebhook::sign(secret, "1725442341" + body), signature);
    ASSERT_TRUE(qqwebhook::verify(secret, "1725442341", body, signature));
    ASSERT_FALSE(qqwebhook::verify(secret, "1725442341", body + " ", signature));
    ASSERT_FALSE(qqwebhook::verify(secret, "1725442342", body, signature));
    ASSERT_FALSE(qqwebhook::verify("", "1725442341", body, signature));
}

TEST(QQOfficialWebhook, ValidationAndDispatchFollowApiV2) {
    auto adapter = std::make_shared<QQOfficialAdapter>("webhook-test");
    ASSERT_TRUE(adapter->configure({
        {"appId", "11111111"},
        {"appSecret", "DG5g3B4j9X2KOErG"},
        {"receiveMode", "webhook"},
    }));
    ASSERT_TRUE(adapter->webhookMode());
    const std::string validation = R"({"d":{"plain_token":"Arq0D5A61EgUu4OxUvOp","event_ts":"1725442341"},"op":13})";
    const auto challenge = adapter->acceptWebhook("", "", validation);
    ASSERT_EQ(challenge.status, 200);
    const auto parsed = json::parse(challenge.body);
    ASSERT_EQ(parsed.value("plain_token", std::string()), std::string("Arq0D5A61EgUu4OxUvOp"));
    ASSERT_EQ(parsed.value("signature", std::string()), std::string("87befc99c42c651b3aac0278e71ada338433ae26fcb24307bdc5ad38c1adc2d01bcfcadc0842edac85e85205028a1132afe09280305f13aa6909ffc2d652c706"));

    int calls = 0;
    adapter->onMessage([&](const Message&) { ++calls; });
    const std::string event = json{
        {"id", "event-1"},
        {"op", 0},
        {"t", "C2C_MESSAGE_CREATE"},
        {"d", {{"id", "message-1"}, {"content", ".r"}, {"author", {{"user_openid", "user-1"}}}}},
    }.dump();
    const std::string timestamp = "1725442341";
    const std::string signature = qqwebhook::sign("DG5g3B4j9X2KOErG", timestamp + event);
    ASSERT_EQ(adapter->acceptWebhook("", "", event).status, 401);
    ASSERT_EQ(calls, 0);
    const auto accepted = adapter->acceptWebhook(timestamp, signature, event);
    ASSERT_EQ(accepted.status, 200);
    ASSERT_EQ(accepted.body, std::string(R"({"op":12})"));
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(adapter->acceptWebhook(timestamp, std::string(128, '0'), event).status, 401);
    ASSERT_EQ(calls, 1);

    adapter->bindWebhookForTest();
    ASSERT_TRUE(QQOfficialAdapter::findWebhook("11111111").get() == adapter.get());
    adapter->unbindWebhookForTest();
    ASSERT_TRUE(QQOfficialAdapter::findWebhook("11111111") == nullptr);
}

TEST(QQOfficialWebhook, GatewayRemainsTheDefault) {
    QQOfficialAdapter adapter("gateway-test");
    ASSERT_TRUE(adapter.configure({{"appId", "11111111"}, {"appSecret", "secret"}}));
    ASSERT_FALSE(adapter.webhookMode());
}
