#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace dice::update {
// Read gzip/tar directly: shell tools' line-oriented listings cannot safely
// validate PAX paths, symlinks, duplicate files or special-device entries.
bool extractPosixUpdate(const std::filesystem::path& archive, const std::filesystem::path& output,
                        std::string& error, const std::function<bool()>& cancelled = {});
}
