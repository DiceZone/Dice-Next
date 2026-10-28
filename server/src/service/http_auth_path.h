#pragma once
#include <string>
#include <string_view>

namespace dice {
// Drogon's registered routes are case-insensitive. Authorization must compare
// paths the same way; do not mutate the request or its case-sensitive IDs.
inline std::string authRoutePath(std::string_view path) {
    std::string normalized(path);
    for (char& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return normalized;
}
} // namespace dice
