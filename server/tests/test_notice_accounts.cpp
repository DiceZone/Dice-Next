#include "test_framework.h"
#include "../src/service/notice_accounts.h"

namespace {
using Json = nlohmann::json;
namespace accounts = dice::notice_accounts;
}

TEST(NoticeAccounts, BuiltinLoginUsesOneBotPlatformAndKeepsSelectedAccount) {
    const Json config{{"id", "builtin-7"}, {"type", "openopq"}, {"name", "内置登录"}};
    const auto meta = accounts::describe(config, "3626455591", "骰娘");
    ASSERT_EQ(meta.at("platform").get<std::string>(), std::string("onebot_v11"));
    ASSERT_EQ(meta.at("id").get<std::string>(), std::string("builtin-7"));
    ASSERT_EQ(meta.at("name").get<std::string>(), std::string("内置登录"));
    ASSERT_EQ(meta.at("login_id").get<std::string>(), std::string("3626455591"));
    ASSERT_EQ(meta.at("login_name").get<std::string>(), std::string("骰娘"));
    ASSERT_TRUE(accounts::matches(config, "builtin-7", "onebot_v11"));
    ASSERT_FALSE(accounts::matches(config, "another-account", "onebot_v11"));
    ASSERT_FALSE(accounts::matches(config, "3626455591", "onebot_v11"));
    ASSERT_FALSE(accounts::matches(config, "builtin-7", "qq_official"));
    ASSERT_EQ(config.at("type").get<std::string>(), std::string("openopq"));
}

TEST(NoticeAccounts, OfflineBuiltinAccountRemainsSelectableAndValid) {
    const Json config{{"id", 7}, {"type", "openopq"}};
    const auto meta = accounts::describe(config);
    ASSERT_EQ(meta.at("id").get<std::string>(), std::string("7"));
    ASSERT_EQ(meta.at("platform").get<std::string>(), std::string("onebot_v11"));
    ASSERT_TRUE(meta.at("login_id").get<std::string>().empty());
    ASSERT_TRUE(meta.at("login_name").get<std::string>().empty());
    ASSERT_TRUE(accounts::matches(config, "7", "onebot_v11"));
    ASSERT_FALSE(accounts::matches(config, "8", "onebot_v11"));
}

TEST(NoticeAccounts, OtherPlatformsKeepTheirOwnAccounts) {
    for (const auto* platform : {"onebot_v11", "qq_official", "milky", "kook", "discord"}) {
        const Json config{{"id", "account-1"}, {"type", platform}};
        const auto meta = accounts::describe(config);
        ASSERT_EQ(meta.at("platform").get<std::string>(), std::string(platform));
        ASSERT_TRUE(accounts::matches(config, "account-1", platform));
        ASSERT_FALSE(accounts::matches(config, "wrong-account", platform));
        ASSERT_FALSE(accounts::matches(config, "account-1", "unrelated-platform"));
    }
}
