#pragma once

// In-process ZIP for backup/restore (and rule-pack extract).  Android and
// many Linux images have no `zip`/`unzip`; Windows previously shelled out to
// tar.exe.  miniz is already a project dependency (image archives).

#include "../common/subprocess.h"
#include <filesystem>
#include <string>
#include <vector>

namespace dice::backup {

dice::proc::Result zipList(const std::filesystem::path& archive, std::size_t outputLimit = 0);
dice::proc::Result zipCreate(const std::filesystem::path& stage,
                             const std::filesystem::path& archive,
                             const std::vector<std::filesystem::path>& members);
dice::proc::Result zipExtract(const std::filesystem::path& archive,
                              const std::filesystem::path& destination);
dice::proc::Result zipRead(const std::filesystem::path& archive, const std::string& innerName,
                           std::size_t outputLimit = 0);

} // namespace dice::backup
