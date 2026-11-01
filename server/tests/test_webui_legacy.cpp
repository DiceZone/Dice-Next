#include "test_framework.h"
#include "service/webui_legacy.h"

TEST(WebuiLegacy, OldVersionEntriesReturnToTheSingleUi) {
    for (const char* path : {"/v1", "/v2", "/v1/", "/v2/", "/v1/index.html", "/v2/index.html"})
        ASSERT_TRUE(dice::webui::isLegacyEntry(path));
    for (const char* path : {"/", "/index.html", "/v3/", "/v1/assets/a.js", "/api/v1/x", "/v1x"})
        ASSERT_FALSE(dice::webui::isLegacyEntry(path));
}

TEST(WebuiLegacy, RelativeLocationKeepsProxyPrefixAndQuery) {
    ASSERT_EQ(dice::webui::legacyEntryLocation("/v2/", ""), std::string("../"));
    ASSERT_EQ(dice::webui::legacyEntryLocation("/v1/index.html", "sso=bound"), std::string("../?sso=bound"));
    ASSERT_EQ(dice::webui::legacyEntryLocation("/v2", ""), std::string("./"));
    ASSERT_EQ(dice::webui::legacyEntryLocation("/v1", "a=1&b=2"), std::string("./?a=1&b=2"));
}
