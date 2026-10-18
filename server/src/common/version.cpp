#include "version.h"

#ifndef DICE_VERSION_STRING
#define DICE_VERSION_STRING "3.0.0"   // fallback; normally set by CMake
#endif

#ifndef DICE_PRERELEASE
#define DICE_PRERELEASE 1
#endif

namespace dice {

std::string versionString() { return DICE_VERSION_STRING; }
bool isPrerelease() { return DICE_PRERELEASE != 0; }
std::string releaseTag() {
    return "v" + versionString() +
        (isPrerelease() ? "-beta." + std::to_string(buildNumber()) : "");
}

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

std::string botBanner(const std::string& platform, const std::string& defaultLocale,
                      const std::vector<std::string>& expressionEngines) {
    std::string num = std::to_string(buildNumber());
    while (num.size() < 3) num = "0" + num;   // zero-pad to 3 digits → (001)
    const std::string author = defaultLocale == "zh" || defaultLocale.rfind("zh-", 0) == 0 || defaultLocale.rfind("zh_", 0) == 0
        ? "希亚" : "ShiaNyaa";
    std::string compatibility;
    for (const auto& engine : expressionEngines) {
        const std::string name = engine == "onedice" ? "OneDice V1" : engine == "dicescript" ? "DiceScript" : "";
        if (name.empty()) continue;
        if (!compatibility.empty()) compatibility += " & ";
        compatibility += name;
    }
    compatibility = compatibility.empty() ? "Dice!Next Native" : compatibility + " Compatible";
    return "Dice!Next By DiceZone/" + author + " Ver " + versionString() + "(" + num + ")\n[" +
           compilerString() + " " + buildTime() + " For Adapter/" +
           platformDisplay(platform) + "]\n" + compatibility;
}

}  // namespace dice
