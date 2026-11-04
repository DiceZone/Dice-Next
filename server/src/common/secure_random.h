#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace dice::secure_random {
// Requested source. Hardware mixes the CPU entropy source (x86-64 RDSEED) into
// the operating-system CSPRNG with XOR, so it is never weaker than System.
enum class Source { System, OpenSSL, Hardware };
// Source that actually produced the most recent word.
enum class Provider { System, OpenSSL, Hardware, Pseudo };

void setSource(Source source) noexcept;
Source source() noexcept;
Source parseSource(const std::string& value) noexcept;   // unknown → System
const char* sourceName(Source source) noexcept;           // "system" / "openssl" / "hardware"
const char* providerName(Provider provider) noexcept;     // adds "pseudo"
bool validSourceName(const std::string& value) noexcept;
// CPU exposes RDSEED. Checked once; false on non-x86-64 builds.
bool hardwareAvailable() noexcept;

struct Status {
    Source requested = Source::System;
    Provider active = Provider::System;   // provider of the latest word
    std::uint64_t fallbacks = 0;          // words produced by a lower-priority provider
};
Status status() noexcept;
// Called only when the active provider changes (fallback or recovery), from the
// rolling thread. Keep it cheap and never call word() from it.
using FallbackObserver = void (*)(Source requested, Provider previous, Provider current);
void setFallbackObserver(FallbackObserver observer) noexcept;

// Never throws. Order: requested source → the other secure source → per-thread
// xoshiro256** seeded from clocks/addresses as a last resort, so rolling keeps
// working even when the OS or OpenSSL entropy interface is unavailable.
std::uint64_t word() noexcept;

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

// Test hooks: force individual providers to fail without touching the OS.
struct FailureInjection { bool system = false, openssl = false, hardware = false; };
void setFailureInjectionForTest(FailureInjection injection) noexcept;
} // namespace dice::secure_random
