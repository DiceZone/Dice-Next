#pragma once
// ─── Dice!Next — version & build info ─────────────────────────
// versionString()/botBanner() are implemented in version.cpp.  buildNumber()
// and buildTime() are implemented by generated/version_build.cpp, which is
// rebuilt on every CMake build so the executable always embeds the build value.

#include <string>

namespace dice {

/// Semantic version, e.g. "3.0.0" (only changes when the maintainer decides).
std::string versionString();

/// Auto-incrementing build number (the (NNN) part). Resets on minor/major bump.
int buildNumber();

/// Build timestamp "YYYY-MM-DD HH:MM:SS".
std::string buildTime();

/// Compiler that built this binary, e.g. "MSVC 19.44" / "GNUC 11.4.0".
std::string compilerString();

/// Human display name for a platform id ("onebot_v11" → "OnebotV11").
std::string platformDisplay(const std::string& platform);

/// The full `.bot` banner, e.g.:
///   Dice!Next By DiceZone/Shia Ver 3.0.0(001)
///   [MSVC 19.44 2026-06-14 06:48:32 For Adapter/OnebotV11]
std::string botBanner(const std::string& platform);

}  // namespace dice
