#include "test_framework.h"
#include "../src/core/legacy_clock_command.h"

using dice::legacy_clock::Operation;

TEST(LegacyClockCommand, ParsesOriginalCompactSyntax) {
    auto request = dice::legacy_clock::parse("+pigeon_clear=5:00");
    ASSERT_TRUE(request.operation == Operation::kAdd);
    ASSERT_EQ(request.name, std::string("pigeon_clear"));
    ASSERT_EQ(request.time, std::string("5:00"));
    ASSERT_TRUE(request.command.empty());

    request = dice::legacy_clock::parse("-pigeon_clear");
    ASSERT_TRUE(request.operation == Operation::kRemove);
    ASSERT_EQ(request.name, std::string("pigeon_clear"));
}

TEST(LegacyClockCommand, ParsesScopedPluginCommand) {
    auto request = dice::legacy_clock::parse("+ 本群早报 07:30 .dailynews");
    ASSERT_TRUE(request.operation == Operation::kAdd);
    ASSERT_EQ(request.name, std::string("本群早报"));
    ASSERT_EQ(request.time, std::string("07:30"));
    ASSERT_EQ(request.command, std::string(".dailynews"));

    request = dice::legacy_clock::parse("+本群求签=8:05 求签 月老");
    ASSERT_TRUE(request.operation == Operation::kAdd);
    ASSERT_EQ(request.name, std::string("本群求签"));
    ASSERT_EQ(request.time, std::string("8:05"));
    ASSERT_EQ(request.command, std::string("求签 月老"));
}

TEST(LegacyClockCommand, NormalizesAndRejectsTimes) {
    ASSERT_EQ(dice::legacy_clock::normalizeDailyTime("7:03").value_or(""), std::string("07:03"));
    ASSERT_EQ(dice::legacy_clock::normalizeDailyTime("23:59").value_or(""), std::string("23:59"));
    ASSERT_FALSE(dice::legacy_clock::normalizeDailyTime("24:00").has_value());
    ASSERT_FALSE(dice::legacy_clock::normalizeDailyTime("7:3").has_value());
}