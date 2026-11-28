#pragma once

#include <string>

#if defined(__ANDROID__)
// Opens the system package installer for a verified APK. Returns false and
// fills error when the installer cannot be started (the current app stays).
bool androidInstallApk(const std::string& path, std::string& error);
#endif
