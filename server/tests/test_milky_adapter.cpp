#include "test_framework.h"
#include "../src/adapter/milky_adapter.h"

#include <memory>

using namespace dice;

TEST(MilkyAdapter, ConvertsMediaMentionsRepliesAndFiles) {
    const json result = MilkyAdapter::buildOutgoingForTest(
        "before[CQ:image,file=https://example.test/image.png]"
        "[CQ:record,file=https://example.test/audio.ogg]"
        "[CQ:video,file=https://example.test/video.mp4,thumb=https://example.test/thumb.jpg]"
        "[CQ:at,qq=12345][CQ:at,qq=all][CQ:face,id=14][CQ:reply,id=99]"
        "[CQ:file,file=https://example.test/report.pdf,name=report.pdf]after");

    const json& segments = result.at("segments");
    ASSERT_EQ(segments.size(), static_cast<size_t>(9));
    ASSERT_EQ(segments[0]["data"]["text"].get<std::string>(), std::string("before"));
    ASSERT_EQ(segments[1]["type"].get<std::string>(), std::string("image"));
    ASSERT_EQ(segments[1]["data"]["uri"].get<std::string>(), std::string("https://example.test/image.png"));
    ASSERT_EQ(segments[2]["type"].get<std::string>(), std::string("record"));
    ASSERT_EQ(segments[3]["type"].get<std::string>(), std::string("video"));
    ASSERT_EQ(segments[3]["data"]["thumb_uri"].get<std::string>(), std::string("https://example.test/thumb.jpg"));
    ASSERT_EQ(segments[4]["data"]["user_id"].get<int64_t>(), int64_t{12345});
    ASSERT_EQ(segments[5]["type"].get<std::string>(), std::string("mention_all"));
    ASSERT_EQ(segments[6]["data"]["face_id"].get<std::string>(), std::string("14"));
    ASSERT_EQ(segments[7]["data"]["message_seq"].get<int64_t>(), int64_t{99});
    ASSERT_EQ(segments[8]["data"]["text"].get<std::string>(), std::string("after"));

    const json& files = result.at("files");
    ASSERT_EQ(files.size(), static_cast<size_t>(1));
    ASSERT_EQ(files[0]["name"].get<std::string>(), std::string("report.pdf"));
    ASSERT_EQ(files[0]["path"].get<std::string>(), std::string("https://example.test/report.pdf"));
}

TEST(MilkyAdapter, PreservesUnsupportedOrMalformedCqSegmentsAsText) {
    const json segments = MilkyAdapter::buildSegmentsForTest(
        "a[CQ:music,type=qq,id=1]b[CQ:at,qq=not-a-number]c[CQ:image]d");

    ASSERT_EQ(segments.size(), static_cast<size_t>(7));
    ASSERT_EQ(segments[0]["data"]["text"].get<std::string>(), std::string("a"));
    ASSERT_EQ(segments[1]["data"]["text"].get<std::string>(), std::string("[CQ:music,type=qq,id=1]"));
    ASSERT_EQ(segments[3]["data"]["text"].get<std::string>(), std::string("[CQ:at,qq=not-a-number]"));
    ASSERT_EQ(segments[5]["data"]["text"].get<std::string>(), std::string("[CQ:image]"));
    ASSERT_EQ(segments[6]["data"]["text"].get<std::string>(), std::string("d"));
}

TEST(MilkyAdapter, RejectsWebhookPayloadWithoutTypeOrObjectData) {
    auto adapter = std::make_shared<MilkyAdapter>("42");

    ASSERT_FALSE(adapter->handleWebhook(json::object()));
    ASSERT_FALSE(adapter->handleWebhook({{"event_type", "message_receive"}, {"data", json::array()}}));
}

TEST(MilkyAdapter, RoutesWebhookMessagesWithCqCompatibleContent) {
    auto adapter = std::make_shared<MilkyAdapter>("42");
    Message received;
    int callbacks = 0;
    adapter->onMessage([&](const Message& message) { received = message; ++callbacks; });

    ASSERT_TRUE(adapter->handleWebhook({
        {"event_type", "message_receive"},
        {"self_id", 10001},
        {"time", 123456},
        {"data", {
            {"message_seq", 77}, {"message_scene", "group"}, {"peer_id", 20002},
            {"sender_id", 30003},
            {"segments", json::array({
                {{"type", "text"}, {"data", {{"text", "hello "}}}},
                {{"type", "mention"}, {"data", {{"user_id", 10001}, {"name", "Dice"}}}},
                {{"type", "image"}, {"data", {{"temp_url", "https://example.test/image.png"}}}}
            })}
        }}
    }));

    ASSERT_EQ(callbacks, 1);
    ASSERT_EQ(received.platform, std::string("milky"));
    ASSERT_EQ(received.adapterId, std::string("42"));
    ASSERT_EQ(received.selfId, std::string("10001"));
    ASSERT_EQ(received.id, std::string("77"));
    ASSERT_EQ(received.targetId, std::string("20002"));
    ASSERT_EQ(received.senderId, std::string("30003"));
    ASSERT_EQ(received.content, std::string("hello "));
    ASSERT_EQ(received.rawContent,
              std::string("hello [CQ:at,qq=10001][CQ:image,file=https://example.test/image.png]"));
    ASSERT_EQ(received.atList.size(), static_cast<size_t>(1));
    ASSERT_EQ(received.atList.front(), std::string("10001"));
    ASSERT_EQ(received.timestamp, int64_t{123456});
}

TEST(MilkyAdapter, MapsFriendNudgeAndGroupDisbandEvents) {
    auto adapter = std::make_shared<MilkyAdapter>("42");
    std::vector<BotEvent> events;
    adapter->onEvent([&](const BotEvent& event) { events.push_back(event); });

    ASSERT_TRUE(adapter->handleWebhook({
        {"event_type", "friend_nudge"}, {"self_id", 10001},
        {"data", {{"user_id", 30003}, {"is_self_receive", true}}}
    }));
    ASSERT_TRUE(adapter->handleWebhook({
        {"event_type", "group_disband"},
        {"data", {{"group_id", 20002}, {"operator_id", 30003}}}
    }));

    ASSERT_EQ(events.size(), static_cast<size_t>(2));
    ASSERT_EQ(static_cast<int>(events[0].type), static_cast<int>(EventType::kPoke));
    ASSERT_EQ(events[0].userId, std::string("10001"));
    ASSERT_EQ(events[0].operatorId, std::string("30003"));
    ASSERT_EQ(static_cast<int>(events[1].type), static_cast<int>(EventType::kGroupDecrease));
    ASSERT_EQ(events[1].groupId, std::string("20002"));
    ASSERT_EQ(events[1].userId, std::string("10001"));
    ASSERT_EQ(events[1].operatorId, std::string("30003"));
}

TEST(MilkyAdapter, RoutesTemporaryMessagesAsPrivate) {
    auto adapter = std::make_shared<MilkyAdapter>("42");
    Message received;
    adapter->onMessage([&](const Message& message) { received = message; });

    ASSERT_TRUE(adapter->handleWebhook({
        {"event_type", "message_receive"},
        {"data", {{"message_scene", "temp"}, {"peer_id", 30003}, {"sender_id", 30003},
                   {"segments", json::array({{{"type", "text"}, {"data", {{"text", "hi"}}}}})}}}
    }));
    ASSERT_EQ(static_cast<int>(received.type), static_cast<int>(MessageType::kPrivate));
    ASSERT_EQ(received.targetId, std::string("30003"));
}
