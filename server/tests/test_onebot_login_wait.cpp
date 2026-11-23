#include "test_framework.h"
#include "../src/adapter/onebot_login_wait.h"

namespace {
nlohmann::json pendingLogin() {
    return {{"status", "failed"}, {"retcode", 100}, {"message", "not connected: nothing has been signed in"}};
}
}
TEST(OneBotLoginWait, KnownStartupReadsMayWaitWithoutClaimingAnError) {
    for (const auto* echo : {"peer_status", "login_info", "login_info_retry", "glist", "flist"}) {
        ASSERT_TRUE(dice::onebot_login::expectedWait(true, false, echo, pendingLogin()));
        ASSERT_FALSE(dice::onebot_login::expectedWait(false, false, echo, pendingLogin()));
        ASSERT_FALSE(dice::onebot_login::expectedWait(true, true, echo, pendingLogin()));
    }
    auto wording = pendingLogin();
    wording["wording"] = wording["message"]; wording.erase("message");
    ASSERT_TRUE(dice::onebot_login::expectedWait(true, false, "peer_status", wording));
}
TEST(OneBotLoginWait, SendFailuresAndRealApiFailuresAreNeverDowngraded) {
    for (const auto* echo : {"send", "42", "imglocal:1", "", "set_group_ban"})
        ASSERT_FALSE(dice::onebot_login::expectedWait(true, false, echo, pendingLogin()));
    auto reply = pendingLogin();
    reply["message"] = "permission denied";
    ASSERT_FALSE(dice::onebot_login::expectedWait(true, false, "login_info", reply));
    reply = pendingLogin(); reply["retcode"] = 401;
    ASSERT_FALSE(dice::onebot_login::expectedWait(true, false, "peer_status", reply));
}
TEST(OneBotLoginWait, MalformedResponsesDoNotLookLikeNormalLoginProgress) {
    for (const auto& response : {nlohmann::json(), nlohmann::json::array(), nlohmann::json::object()})
        ASSERT_FALSE(dice::onebot_login::expectedWait(true, false, "peer_status", response));
    auto reply = pendingLogin(); reply["retcode"] = "100";
    ASSERT_FALSE(dice::onebot_login::expectedWait(true, false, "peer_status", reply));
    reply = pendingLogin(); reply["message"] = 100;
    ASSERT_FALSE(dice::onebot_login::expectedWait(true, false, "peer_status", reply));
}
