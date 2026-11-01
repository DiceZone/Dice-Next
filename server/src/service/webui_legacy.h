#pragma once

#include <string>

namespace dice::webui {
// The V1/V2 dual bundle ended at build 962; web/dist now holds the only UI.
// Old /v1/ and /v2/ bookmarks return to the root. The Location is relative so
// a reverse-proxy prefix survives, and an absent fragment keeps the hash route.
inline bool isLegacyEntry(const std::string& path) {
    for (const char* version : {"/v1", "/v2"})
        for (const char* suffix : {"", "/", "/index.html"})
            if (path == std::string(version) + suffix) return true;
    return false;
}

inline std::string legacyEntryLocation(const std::string& path, const std::string& query) {
    // "/v1" sits in the root directory; "/v1/" and "/v1/index.html" are one level down.
    const bool nested = !path.empty() && (path.back() == '/' || path.size() > 3);
    std::string location = nested ? "../" : "./";
    if (!query.empty()) location += "?" + query;
    return location;
}
} // namespace dice::webui
