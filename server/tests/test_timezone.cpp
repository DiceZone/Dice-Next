// ─── Issue #16: 统一时区工具回归测试 ─────────────────────────
// 存储统一为 UTC；面向用户的展示/上传按 server/timezone_minutes 换算。

#include "test_framework.h"
#include "../src/common/utils.h"

#include <ctime>
#include <limits>

using namespace dice;

namespace {

struct TZGuard {
    explicit TZGuard(int minutes) { utils::setTimezoneOffset(minutes); }
    ~TZGuard() { utils::setTimezoneOffset((std::numeric_limits<int>::min)()); }
};

std::time_t epochOf(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

}  // namespace

TEST(Timezone, ParseUtcIsoWithAndWithoutSuffix) {
    const std::time_t expected = epochOf(2026, 8, 6, 4, 5, 6);
    ASSERT_EQ(utils::parseIsoUtcToEpoch("2026-08-06T04:05:06"), expected);
    ASSERT_EQ(utils::parseIsoUtcToEpoch("2026-08-06T04:05:06Z"), expected);
    ASSERT_EQ(utils::parseIsoUtcToEpoch("2026-08-06T04:05:06.123Z"), expected);
    ASSERT_EQ(utils::parseIsoUtcToEpoch("not-a-time"), 0);
}

TEST(Timezone, FormatsInConfiguredOffset) {
    TZGuard guard(480);   // UTC+8
    const std::time_t t = epochOf(2026, 8, 6, 4, 0, 0);
    ASSERT_EQ(utils::formatTimeInTimezone(t, "%Y-%m-%d %H:%M"), std::string("2026-08-06 12:00"));
    ASSERT_EQ(utils::formatTimeInTimezone(t, "%Y-%m-%d"), std::string("2026-08-06"));
}

TEST(Timezone, FormatsInNegativeOffset) {
    TZGuard guard(-300);  // UTC-5
    const std::time_t t = epochOf(2026, 8, 6, 4, 0, 0);
    ASSERT_EQ(utils::formatTimeInTimezone(t, "%Y-%m-%d %H:%M"), std::string("2026-08-05 23:00"));
}
