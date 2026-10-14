#include "test_framework.h"
#include "../src/adapter/qq_official_adapter.h"
#include <memory>
#include <set>

using namespace dice;
namespace {
struct OfficialProtocolFixture {
    std::shared_ptr<QQOfficialAdapter> adapter = std::make_shared<QQOfficialAdapter>("protocol");
    struct Request { drogon::HttpMethod method; std::string path; json body; };
    std::vector<Request> requests;
    std::vector<Message> messages;
    std::vector<BotEvent> events;
    json response = json::object(), result;
    OfficialProtocolFixture() {
        adapter->configure({{"appId", "app"}, {"appSecret", "fixture-only"}});
        adapter->setApiTransportForTest([this](auto method, const auto& path, const auto& body, auto cb) {
            requests.push_back({method, path, body}); cb(QQOfficialAdapter::apiResponseForTest(200, response));
        });
        adapter->onMessage([this](const auto& message) { messages.push_back(message); });
        adapter->onEvent([this](const auto& event) { events.push_back(event); });
    }
    void call(const std::string& action, const json& params) {
        result = nullptr; adapter->invokeActionAsync(action, params, [this](json reply) { result = std::move(reply); });
    }
};
}

TEST(OfficialProtocol, TimestampAcceptsUnixAndRFC3339OffsetsAndRejectsBrokenDates) {
    const auto utc = qqprotocol::timestamp("2026-10-09T10:37:20Z", -1);
    ASSERT_TRUE(utc > 0);
    ASSERT_EQ(qqprotocol::timestamp("2026-10-09T18:37:20+08:00", -1), utc);
    ASSERT_EQ(qqprotocol::timestamp("2026-10-09T04:07:20-06:30", -1), utc);
    ASSERT_EQ(qqprotocol::timestamp("2026-10-09T18:37:20.123+08:00", -1), utc);
    ASSERT_EQ(qqprotocol::timestamp(utc, -1), utc);
    for (const auto& input : {json(nullptr), json::object(), json("invalid"), json("2026-02-30T10:00:00Z"),
            json("2026-10-09T18:37:20+99:00"), json("2026-10-09T25:00:00Z"), json(-1)})
        ASSERT_EQ(qqprotocol::timestamp(input, 123), int64_t(123));
}

TEST(OfficialProtocol, SharesUseCurrentDataUrlAndKeepSafeLegacyFallbacks) {
    ASSERT_EQ(qqprotocol::shareUrl({{"data", {{"url", "https://qun.qq.com/robot?robot_uin=123"}}}}), "https://qun.qq.com/robot?robot_uin=123");
    ASSERT_EQ(qqprotocol::shareUrl({{"url_link", "https://qun.qq.com/legacy"}}), "https://qun.qq.com/legacy");
    ASSERT_TRUE(qqprotocol::shareUrl({{"data", {{"url", "javascript:alert(1)"}}}}).empty());
    ASSERT_TRUE(qqprotocol::shareUrl({{"data", nullptr}}).empty());
}

TEST(OfficialProtocol, NewMemberIntentsAreOptInAndLeaveExistingSubscriptionsIntact) {
    OfficialProtocolFixture f;
    const int original = (1 << 25) | (1 << 26) | (1 << 30);
    ASSERT_EQ(f.adapter->gatewayIntentsForTest(), original);
    f.adapter->configure({{"appId", "app"}, {"appSecret", "fixture"}, {"subscribeGroupMemberEvents", true}});
    ASSERT_EQ(f.adapter->gatewayIntentsForTest(), original | (1 << 24));
    ASSERT_TRUE(f.adapter->capabilities().value("group_member_events_enabled", false));
}

TEST(OfficialProtocol, AllAdvertisedApiActionsHaveADocumentedWireRoute) {
    struct Route { std::string action, path; drogon::HttpMethod method; };
    const std::vector<Route> routes = {
        {"qq_get_mute", "/v2/groups/g/restrict_chat_setting", drogon::Get}, {"qq_set_mute", "/v2/groups/g/restrict_chat_setting", drogon::Post},
        {"qq_join_requests", "/v2/groups/g/join_request_list", drogon::Get}, {"qq_approve_join", "/v2/groups/g/approval_join_request/u", drogon::Post},
        {"qq_list_join_strategies", "/v2/groups/join_approval_strategy", drogon::Get}, {"qq_create_join_strategy", "/v2/groups/join_approval_strategy", drogon::Post},
        {"qq_update_join_strategy", "/v2/groups/join_approval_strategy/s", drogon::Patch}, {"qq_delete_join_strategy", "/v2/groups/join_approval_strategy/s", drogon::Delete},
        {"qq_execute_join_strategy", "/v2/groups/join_approval_strategy/s/execute", drogon::Post}, {"qq_update_join_whitelist", "/v2/groups/join_approval_strategy/s/whitelist_users", drogon::Post},
        {"qq_group_info", "/v2/groups/g/info", drogon::Get}, {"qq_bot_state", "/v2/groups/g/bot_state", drogon::Get},
        {"qq_group_members", "/v2/groups/g/members", drogon::Get}, {"qq_group_member", "/v2/groups/g/members/u", drogon::Get},
        {"qq_blacklist", "/v2/groups/g/member_blacklist", drogon::Get}, {"qq_set_blacklist", "/v2/groups/g/member_blacklist", drogon::Post},
        {"qq_remove_members", "/v2/groups/g/batch_remove_members", drogon::Post}, {"qq_recall", "/v2/groups/g/messages/m", drogon::Delete},
        {"qq_stream_message", "/v2/users/u/stream_messages", drogon::Post}, {"qq_get_menu", "/v2/menu", drogon::Get}, {"qq_set_menu", "/v2/menu", drogon::Put},
        {"qq_list_panels", "/v2/panels", drogon::Get}, {"qq_create_panel", "/v2/panels", drogon::Post}, {"qq_get_panel", "/v2/panels/p", drogon::Get},
        {"qq_update_panel", "/v2/panels/p", drogon::Put}, {"qq_delete_panel", "/v2/panels/p", drogon::Delete}, {"qq_panel_target", "/v2/panels/p/target", drogon::Put},
        {"qq_me", "/users/@me", drogon::Get}, {"qq_my_guilds", "/users/@me/guilds", drogon::Get}, {"qq_guild", "/guilds/guild", drogon::Get},
        {"qq_guild_channels", "/guilds/guild/channels", drogon::Get}, {"qq_create_channel", "/guilds/guild/channels", drogon::Post},
        {"qq_channel", "/channels/channel", drogon::Get}, {"qq_update_channel", "/channels/channel", drogon::Patch}, {"qq_delete_channel", "/channels/channel", drogon::Delete},
        {"qq_interaction", "/interactions/i", drogon::Put}, {"qq_generate_share_url", "/v2/generate_url_link", drogon::Post},
        {"qq_send_message", "/v2/groups/g/messages", drogon::Post}, {"qq_input_notify", "/v2/users/u/messages", drogon::Post},
        {"qq_wakeup_message", "/v2/users/u/messages", drogon::Post}, {"qq_media_files", "/v2/groups/g/files", drogon::Post},
        {"qq_upload_prepare", "/v2/groups/g/upload_prepare", drogon::Post}, {"qq_upload_part_finish", "/v2/groups/g/upload_part_finish", drogon::Post},
        {"qq_gateway", "/gateway", drogon::Get}
    };
    std::set<std::string> tested;
    for (const auto& route : routes) {
        OfficialProtocolFixture f;
        json params = {{"groupId", "g"}, {"userOpenId", "u"}, {"memberOpenId", "u"}, {"strategyId", "s"}, {"panelId", "p"},
            {"messageId", "m"}, {"guildId", "guild"}, {"channelId", "channel"}, {"interactionId", "i"}, {"memberOpenIds", json::array({"u"})},
            {"userOpenIds", json::array({"u"})}, {"whitelistUsers", json::array({"12345"})},
            {"members", json::array({{{"op", "add"}, {"member_openid", "u"}, {"mute_expire_at", "2026-10-30T10:00:00+08:00"}}})},
            {"body", {{"content", "fixture-only"}, {"code", 0}}}};
        if (route.action == "qq_input_notify" || route.action == "qq_wakeup_message" || route.action == "qq_stream_message") params.erase("groupId");
        f.call(route.action, params);
        ASSERT_TRUE(f.result.is_object() && f.result.value("ok", false));
        ASSERT_EQ(f.requests.size(), size_t(1)); ASSERT_EQ(f.requests[0].path, route.path); ASSERT_EQ(f.requests[0].method, route.method);
        tested.insert(route.action);
    }
    const auto advertised = qqprotocol::actions(); ASSERT_EQ(tested.size(), advertised.size());
    for (const auto& action : advertised) ASSERT_TRUE(tested.count(action.get<std::string>()));
}

TEST(OfficialProtocol, InvalidParametersCannotIssuePlatformRequests) {
    OfficialProtocolFixture f;
    const std::vector<std::pair<std::string, json>> invalid = {
        {"qq_group_info", nullptr}, {"qq_group_info", {{"groupId", 123}}}, {"qq_set_mute", {{"groupId", "g"}, {"members", json::array()}}},
        {"qq_remove_members", {{"groupId", "g"}, {"memberOpenIds", json::array({1})}}},
        {"qq_remove_members", {{"groupId", "g"}, {"memberOpenIds", json(std::vector<std::string>(21, "u"))}}},
        {"qq_join_requests", {{"groupId", "g"}, {"limit", 51}}}, {"qq_generate_share_url", {{"callbackData", std::string(33, 'x')}}},
        {"qq_panel_target", {{"panelId", "p"}, {"op", "remove"}}}, {"qq_input_notify", {{"userOpenId", "u"}, {"seconds", 61}}},
        {"qq_input_notify", {{"userOpenId", "u"}, {"seconds", "five"}}}, {"qq_wakeup_message", {{"userOpenId", "u"}, {"body", {{"event_id", "e"}}}}},
        {"qq_interaction", {{"interactionId", "i"}, {"body", {{"code", 6}}}}}, {"qq_send_message", {{"groupId", "g"}, {"body", nullptr}}}
    };
    for (const auto& [action, params] : invalid) { f.call(action, params); ASSERT_FALSE(f.result.value("ok", true)); }
    ASSERT_TRUE(f.requests.empty());
}

TEST(OfficialProtocol, InteractionsParseRFC3339AndAcknowledgeOnlyButtonsAndMenusOnce) {
    OfficialProtocolFixture f;
    const json click = {{"id", "click-1"}, {"type", 11}, {"scene", "c2c"}, {"user_openid", "u"},
        {"timestamp", "2026-10-09T18:37:20+08:00"}, {"data", {{"resolved", {{"button_data", ".dangerous-command"}}}}}};
    f.adapter->dispatchForTest("INTERACTION_CREATE", click);
    ASSERT_EQ(f.requests.size(), size_t(1)); ASSERT_EQ(f.requests[0].path, "/interactions/click-1");
    ASSERT_EQ(f.requests[0].body.value("code", -1), 0); ASSERT_EQ(f.events[0].userId, "u");
    ASSERT_EQ(f.events[0].timestamp, qqprotocol::timestamp("2026-10-09T10:37:20Z", -1)); ASSERT_TRUE(f.messages.empty());
    f.adapter->dispatchForTest("INTERACTION_CREATE", click);
    ASSERT_EQ(f.requests.size(), size_t(1)); ASSERT_EQ(f.events.size(), size_t(1));
    for (int type : {13, 14, 15, 16, 18, 19, 20}) {
        auto notice = click; notice["id"] = "notice-" + std::to_string(type); notice["type"] = type;
        f.adapter->dispatchForTest("INTERACTION_CREATE", notice);
    }
    ASSERT_EQ(f.requests.size(), size_t(1)); ASSERT_EQ(f.events.size(), size_t(8));
    f.call("qq_interaction", {{"interactionId", "click-1"}}); ASSERT_FALSE(f.result.value("ok", true));
}

TEST(OfficialProtocol, ManualInteractionModeAllowsCustomResponseWithoutAutomaticAck) {
    OfficialProtocolFixture f;
    f.adapter->configure({{"appId", "app"}, {"appSecret", "fixture"}, {"autoAcknowledgeInteractions", false}});
    f.adapter->dispatchForTest("INTERACTION_CREATE", {{"id", "manual"}, {"type", 12}, {"user_openid", "u"}, {"timestamp", "2026-10-09T18:37:20+08:00"}});
    ASSERT_TRUE(f.requests.empty()); ASSERT_FALSE(f.events[0].extra.value("interaction_ack_automatic", true));
    f.call("qq_interaction", {{"interactionId", "manual"}, {"body", {{"code", 4}}}});
    ASSERT_TRUE(f.result.value("ok", false)); ASSERT_EQ(f.requests[0].body.value("code", -1), 4);
}

TEST(OfficialProtocol, SubscriptionStatusesKeepTheirOwnerAndTemplateResults) {
    OfficialProtocolFixture f;
    const auto results = json::array({{{"template_id", 10001}, {"op", 1}, {"subscribe_id", "opaque"}, {"update_ts", 1000}}});
    f.adapter->dispatchForTest("SUBSCRIBE_MESSAGE_STATUS", {{"openid", "u"}, {"result", results}});
    ASSERT_EQ(f.events.size(), size_t(1)); ASSERT_EQ(f.events[0].userId, "u");
    ASSERT_EQ(f.events[0].extra["subscription_results"], results); ASSERT_TRUE(f.requests.empty());
}

TEST(OfficialProtocol, DeduplicationIncludesMessageIndexAndDoesNotOverwriteFreshNames) {
    OfficialProtocolFixture f;
    json message = {{"id", "shared-id"}, {"content", ".r"}, {"author", {{"user_openid", "u"}, {"username", "旧名"}}},
        {"message_scene", {{"ext", json::array({"msg_idx=one"})}}}};
    f.adapter->dispatchForTest("C2C_MESSAGE_CREATE", message);
    auto newer = message; newer["id"] = "new-id"; newer["author"]["username"] = "新名";
    f.adapter->dispatchForTest("C2C_MESSAGE_CREATE", newer); f.adapter->dispatchForTest("C2C_MESSAGE_CREATE", message);
    ASSERT_EQ(f.messages.size(), size_t(2)); ASSERT_EQ(f.adapter->getUserName("u"), "新名");
    message["message_scene"]["ext"] = json::array({"msg_idx=two"}); message["author"]["username"] = "新名";
    f.adapter->dispatchForTest("C2C_MESSAGE_CREATE", message); ASSERT_EQ(f.messages.size(), size_t(3));
    ASSERT_EQ(f.messages.back().extra.value("message_reference_id", std::string()), "two");
}

TEST(OfficialProtocol, QuotePayloadUsesThePlatformReferenceIndex) {
    OfficialProtocolFixture f; Message message; message.type = MessageType::kGroup;
    message.id = "provider-message-id"; message.extra = {{"message_reference_id", "REFIDX_native"}, {"__qq_quote_reference", message.id}};
    ASSERT_EQ(f.adapter->textPayload(message, "reply", ContentFormat::kPlainText)["message_reference"]["message_id"].get<std::string>(), "REFIDX_native");
    message.extra.erase("message_reference_id");
    const auto legacy = f.adapter->textPayload(message, "reply", ContentFormat::kPlainText);
    ASSERT_FALSE(legacy.contains("message_reference"));
    ASSERT_EQ(legacy["content"].get<std::string>(), "reply");
    message.id.clear();
    message.extra = {{"message_reference_id", "REFIDX_native"}};
    ASSERT_FALSE(f.adapter->textPayload(message, "reply", ContentFormat::kPlainText).contains("message_reference"));
}

TEST(OfficialProtocol, BotStateSuppliesGroupSpecificSelfRoleAndHasItsOwnPermissionBackoff) {
    OfficialProtocolFixture f; f.response = {{"member_openid", "group-specific-bot"}, {"member_role", "admin"}};
    ASSERT_EQ(f.adapter->getSelfRole("g"), "admin"); ASSERT_EQ(f.requests[0].path, "/v2/groups/g/bot_state");
    ASSERT_EQ(f.adapter->getSelfRole("g"), "admin"); ASSERT_EQ(f.requests.size(), size_t(1));
    OfficialProtocolFixture denied; denied.response = {{"err_code", 11253}};
    ASSERT_TRUE(denied.adapter->getSelfRole("g").empty()); denied.adapter->getSelfRole("other");
    ASSERT_EQ(denied.requests.size(), size_t(1));
}

TEST(OfficialProtocol, ManagementQueriesAlsoPopulateNamesWithoutGrantingGroupMembership) {
    OfficialProtocolFixture f;
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{{"qq_get_mute", "members"}, {"qq_join_requests", "list"}, {"qq_blacklist", "users"}}) {
        f.response = {{pair.second, json::array({{{"member_openid", pair.first}, {"username", "查询昵称"}}})}};
        f.call(pair.first, {{"groupId", "g"}}); ASSERT_EQ(f.adapter->getUserName(pair.first), "查询昵称");
    }
    f.response = {{"members", json::array()}, {"next_cursor", ""}};
    ASSERT_TRUE(f.adapter->getMembers("g").empty());
}

TEST(OfficialProtocol, MenuSwitchesAndPanelTargetsAreNotDroppedByTheActionBridge) {
    OfficialProtocolFixture f;
    const json menu = {{"menu", {{"items", json::array({{{"type", "switch"}, {"name", "联网"}, {"switch", {{"switch_id", "search"}, {"default", false}}}}})}}}};
    f.call("qq_set_menu", {{"body", menu}}); ASSERT_EQ(f.requests[0].body, menu);
    f.call("qq_panel_target", {{"panelId", "panel/id"}, {"op", "add"}, {"groupOpenIds", json::array({"group"})}});
    ASSERT_EQ(f.requests[1].path, "/v2/panels/panel%2Fid/target");
    ASSERT_EQ(f.requests[1].body["group_openids"], json::array({"group"}));
}
