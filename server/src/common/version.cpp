#include "version.h"

#ifndef DICE_VERSION_STRING
#define DICE_VERSION_STRING "3.0.0"   // fallback; normally set by CMake
#endif

namespace dice {

std::string versionString() { return DICE_VERSION_STRING; }

std::string compilerString() {
#if defined(_MSC_VER)
    // _MSC_VER e.g. 1944 → "MSVC 19.44"
    int v = _MSC_VER;
    std::string minor = std::to_string(v % 100);
    if (minor.size() < 2) minor = "0" + minor;
    return "MSVC " + std::to_string(v / 100) + "." + minor;
#elif defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return "GNUC " + std::to_string(__GNUC__) + "." +
           std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
    return "Unknown";
#endif
}

std::string platformDisplay(const std::string& p) {
    if (p == "onebot_v11") return "OnebotV11";
    if (p == "milky")      return "Milky";
    if (p == "discord")    return "Discord";
    return p.empty() ? "Unknown" : p;
}

std::string botBanner(const std::string& platform) {
    std::string num = std::to_string(buildNumber());
    while (num.size() < 3) num = "0" + num;   // zero-pad to 3 digits → (001)
    return "Dice!Next By DiceZone/Shia Ver " + versionString() + "(" + num + ")\n[" +
           compilerString() + " " + buildTime() + " For Adapter/" +
           platformDisplay(platform) + "]\nOneDice V1 Compatible";
}

}  // namespace dice
