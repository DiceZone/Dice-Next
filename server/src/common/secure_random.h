#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace dice::secure_random {
enum class Source { System, OpenSSL };
void setSource(Source source) noexcept;
Source source() noexcept;
std::uint64_t word();
// Stateless URBG: callers do not keep an MT state or an entropy buffer.
struct Engine {
    using result_type = std::uint64_t;
    static constexpr result_type (min)() { return 0; }
    static constexpr result_type (max)() { return (std::numeric_limits<result_type>::max)(); }
    result_type operator()() { return word(); }
};
template<class Next> std::uint64_t below(std::uint64_t bound, Next next) {
    if (bound == 0) throw std::invalid_argument("random bound must be positive");
    const auto threshold = -bound % bound;
    for (;;) { const auto value = next(); if (value >= threshold) return value % bound; }
}
inline std::uint64_t below(std::uint64_t bound) { return below(bound, [] { return word(); }); }
inline long long die(long long faces) {
    if (faces < 1) throw std::invalid_argument("dice sides must be positive");
    return static_cast<long long>(below(static_cast<std::uint64_t>(faces))) + 1;
}
} // namespace dice::secure_random
