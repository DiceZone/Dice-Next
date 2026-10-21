#pragma once
#include "../i18n/i18n.h"
#include <cstdint>

namespace dice::outbound {
struct PreviewContext {
    I18n::Args args;
    std::string command;
    std::string family;
    std::string grade;
    std::uint32_t seed = 0;
    // Extra machine-readable facts for testing and explaining the example.
    json facts = json::object();
};

// No router, database, adapters, plugin execution or shared dice RNG. This only
// computes example data and never performs the operation described by a reply.
PreviewContext templatePreviewContext(const json &request, const I18n *i18n = nullptr);
} // namespace dice::outbound
