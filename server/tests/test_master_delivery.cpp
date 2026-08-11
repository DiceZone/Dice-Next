#include "test_framework.h"
#include "../src/core/master_delivery.h"

using dice::master_delivery::Recipient;

TEST(MasterDelivery, GroupsBoundRealQQAndOpenId) {
    const auto groups = dice::master_delivery::groupRecipients({
        {"onebot_v11", "napcat", "123456789", "123456789"},
        {"qq_official", "official", "open-id", "123456789"},
    });
    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups.front().size(), 2u);
}

TEST(MasterDelivery, DoesNotGuessUnboundCrossPlatformIdentity) {
    const auto groups = dice::master_delivery::groupRecipients({
        {"onebot_v11", "napcat", "123456789", "123456789"},
        {"qq_official", "official", "open-id", ""},
        {"kook", "kook-bot", "123456789", ""},
    });
    ASSERT_EQ(groups.size(), 3u);
}

TEST(MasterDelivery, DeduplicatesExactEndpoint) {
    const auto groups = dice::master_delivery::groupRecipients({
        {"qq_official", "official", "open-id", ""},
        {"qq_official", "official", "open-id", ""},
    });
    ASSERT_EQ(groups.size(), 1u);
    ASSERT_EQ(groups.front().size(), 2u);
}

TEST(MasterDelivery, PrefersOneBotThenOfficial) {
    const Recipient onebot{"onebot_v11", "napcat", "123456789", "123456789"};
    const Recipient official{"qq_official", "official", "open-id", "123456789"};
    ASSERT_TRUE(dice::master_delivery::transportPriority(onebot) <
                dice::master_delivery::transportPriority(official));
}
