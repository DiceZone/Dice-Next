#include "test_framework.h"
#include "platform/startup_guard.h"

using dice::startup::isWithinTemp;
TEST(StartupGuard, TemporaryRootAndChildren) {
    const std::vector<std::wstring> roots{L"C:\\Users\\Tester\\AppData\\Local\\Temp\\", L"D:\\Scratch"};
    ASSERT_TRUE(isWithinTemp(L"C:\\Users\\Tester\\AppData\\Local\\Temp", roots));
    ASSERT_TRUE(isWithinTemp(L"c:/USERS/tester/AppData/Local/Temp/7zABC/dice", roots));
    ASSERT_TRUE(isWithinTemp(L"D:\\Scratch\\Rar$EXa0\\app", roots));
    ASSERT_FALSE(isWithinTemp(L"D:\\ScratchBackup\\dice", roots));
    ASSERT_FALSE(isWithinTemp(L"C:\\Users\\Tester\\Downloads\\DiceNext", roots));
    ASSERT_FALSE(isWithinTemp(L"D:\\my-temp-project", roots));
}
TEST(StartupGuard, ExtendedPathsAndTraversal) {
    const std::vector<std::wstring> roots{L"C:\\Temp"};
    ASSERT_TRUE(isWithinTemp(L"\\\\?\\C:\\Temp\\.\\zip\\..\\app", roots));
    ASSERT_FALSE(isWithinTemp(L"C:\\Temp\\..\\DiceNext", roots));
    ASSERT_FALSE(isWithinTemp(L"C:\\Temporary\\DiceNext", roots));
    ASSERT_FALSE(isWithinTemp(L"Temp\\DiceNext", roots));
}
TEST(StartupGuard, UncRootsAndEmptyFallbacks) {
    const std::vector<std::wstring> roots{L"\\\\server\\share\\temp", L"", L"C:\\", L"\\\\server\\share"};
    ASSERT_TRUE(isWithinTemp(L"\\\\?\\UNC\\SERVER\\share\\Temp\\dice", roots));
    ASSERT_FALSE(isWithinTemp(L"\\\\server\\share\\temporary\\dice", roots));
    ASSERT_FALSE(isWithinTemp(L"C:\\Program Files\\DiceNext", roots));
    ASSERT_FALSE(isWithinTemp(L"", roots));
}
