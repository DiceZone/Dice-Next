#pragma once
#include <memory>
#include <functional>
#include "deck_package.h"
namespace dice::image_resources {
class Service;
void registerRoutes(const std::shared_ptr<Service>& service);
void registerDeckPackageRoutes(const std::shared_ptr<Service>& service, std::function<void()> reload, DeckDependencyLookup lookup = {});
}
