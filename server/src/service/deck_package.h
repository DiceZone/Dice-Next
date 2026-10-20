#pragma once
#include "image_resource_service.h"
#include "image_archive.h"
#include <memory>
namespace dice::image_resources {
struct DeckDownload { std::shared_ptr<TemporaryDirectory> temporary; std::filesystem::path file; std::string name; };
using DeckDependencyLookup = std::function<std::optional<std::filesystem::path>(const std::string&)>;
DeckDownload exportDeck(Service&, const std::filesystem::path& source, const std::string& filename, DeckDependencyLookup lookup = {});
nlohmann::json importDeckPackage(Service&, std::string_view bytes, bool overwrite=false, bool inspect=false);
}
