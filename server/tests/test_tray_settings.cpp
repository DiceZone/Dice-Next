#include "test_framework.h"
#include "platform/tray_settings.h"

TEST(TraySettings, DefaultAndActualPort) {
    ASSERT_EQ(dice::tray_settings::tooltip("", 18088), "Dice!Next(18088)");
    ASSERT_EQ(dice::tray_settings::tooltip("希亚骰", 18089), "希亚骰(18089)");
}
TEST(TraySettings, UnicodeLimitAndWhitespace) {
    std::string text, error;
    ASSERT_TRUE(dice::tray_settings::normalize("  希亚骰  ", text, error));
    ASSERT_EQ(text, "希亚骰");
    ASSERT_TRUE(dice::tray_settings::normalize(" \t\r\n　", text, error));
    ASSERT_EQ(text, "");
    ASSERT_TRUE(dice::tray_settings::normalize("一二三四五六七八九十", text, error));
    ASSERT_FALSE(dice::tray_settings::normalize("一二三四五六七八九十一", text, error));
    ASSERT_TRUE(dice::tray_settings::normalize("🎲🎲🎲🎲🎲🎲🎲🎲🎲🎲", text, error));
    ASSERT_FALSE(dice::tray_settings::normalize("🎲🎲🎲🎲🎲🎲🎲🎲🎲🎲🎲", text, error));
}
TEST(TraySettings, InvalidTextDoesNotReplacePrevious) {
    std::string text = "原文字", error;
    for (const auto& value : {std::string("ab\ncd"), std::string("ab\0cd", 5), std::string("\xc0\x80"),
         std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8")}) {
        ASSERT_FALSE(dice::tray_settings::normalize(value, text, error));
        ASSERT_EQ(text, "原文字");
        ASSERT_FALSE(error.empty());
    }
}
