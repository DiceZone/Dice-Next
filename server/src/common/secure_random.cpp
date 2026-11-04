#include "secure_random.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <thread>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <sys/random.h> // Darwin declares getentropy here, not in unistd.h.
#else
#include <sys/random.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define DICE_SECURE_RANDOM_X86 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace dice::secure_random {
namespace {
std::atomic<Source> selected{Source::System};
std::atomic<int> lastProvider{static_cast<int>(Provider::System)};
std::atomic<std::uint64_t> fallbackCount{0};
std::atomic<FallbackObserver> observer{nullptr};
std::atomic<bool> failSystem{false}, failOpenssl{false}, failHardware{false};

bool systemWord(std::uint64_t& value) noexcept {
    if (failSystem.load(std::memory_order_relaxed)) return false;
#ifdef _WIN32
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    int result;
    do { result = getentropy(&value, sizeof(value)); } while (result != 0 && errno == EINTR);
    return result == 0;
#else
    // getrandom is missing on kernels before 3.17 (e.g. CentOS 7) and may be
    // blocked by old container seccomp profiles; OpenSSL then reads /dev/urandom.
    auto* output = reinterpret_cast<unsigned char*>(&value);
    std::size_t offset = 0;
    while (offset < sizeof(value)) {
        const auto count = getrandom(output + offset, sizeof(value) - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
#endif
}

bool opensslWord(std::uint64_t& value) noexcept {
    if (failOpenssl.load(std::memory_order_relaxed)) return false;
    return RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) == 1;
}

#ifdef DICE_SECURE_RANDOM_X86
bool cpuHasRdseed() noexcept {
#ifdef _MSC_VER
    int regs[4]{};
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuidex(regs, 7, 0);
    return (static_cast<unsigned>(regs[1]) & (1u << 18)) != 0;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
    return (b & (1u << 18)) != 0;
#endif
}
#ifndef _MSC_VER
__attribute__((target("rdseed")))
#endif
bool rdseedStep(std::uint64_t& value) noexcept {
    // RDSEED may report "not ready" briefly under contention; retry a little,
    // then let the caller fall back instead of spinning.
    for (int attempt = 0; attempt < 32; ++attempt) {
#ifdef _MSC_VER
        unsigned __int64 out = 0;
#else
        unsigned long long out = 0;
#endif
        if (_rdseed64_step(&out)) { value = static_cast<std::uint64_t>(out); return true; }
        _mm_pause();
    }
    return false;
}
#endif

bool hardwareWord(std::uint64_t& value) noexcept {
    if (failHardware.load(std::memory_order_relaxed) || !hardwareAvailable()) return false;
#ifdef DICE_SECURE_RANDOM_X86
    if (!rdseedStep(value)) return false;
    // Some CPU/firmware defects return all-zero or all-one words while claiming
    // success; treat them as unavailable for this word.
    return value != 0 && value != ~std::uint64_t{0};
#else
    (void)value;
    return false;
#endif
}

std::uint64_t splitmix(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

// Last resort only: a 32-byte per-thread xoshiro256** state, seeded once per
// thread from clocks, addresses and a process counter (similar in spirit to
// SealDice's PCG seed). Statistically uniform, but not a secure source.
std::uint64_t pseudoWord() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    thread_local std::uint64_t s[4]{};
    thread_local bool seeded = false;
    if (!seeded) {
        int local = 0;
        std::uint64_t seed = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        seed ^= rotl(static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()), 17);
        seed ^= rotl(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&local)), 29);
        seed ^= rotl(static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())), 41);
        seed ^= counter.fetch_add(0x632BE59BD9B4E019ull, std::memory_order_relaxed);
        for (auto& word : s) word = splitmix(seed);
        seeded = true;
    }
    const std::uint64_t result = rotl(s[1] * 5, 7) * 9;
    const std::uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);
    return result;
}

Provider expected(Source source) noexcept {
    switch (source) {
    case Source::OpenSSL: return Provider::OpenSSL;
    case Source::Hardware: return Provider::Hardware;
    default: return Provider::System;
    }
}

std::uint64_t report(Source requested, Provider used, std::uint64_t value) noexcept {
    if (used != expected(requested)) fallbackCount.fetch_add(1, std::memory_order_relaxed);
    const int now = static_cast<int>(used);
    if (lastProvider.load(std::memory_order_relaxed) != now) {
        const int before = lastProvider.exchange(now, std::memory_order_relaxed);
        if (before != now) {
            if (auto notify = observer.load()) notify(requested, static_cast<Provider>(before), used);
        }
    }
    return value;
}
} // namespace

void setSource(Source value) noexcept { selected.store(value); }
Source source() noexcept { return selected.load(); }

Source parseSource(const std::string& value) noexcept {
    if (value == "openssl") return Source::OpenSSL;
    if (value == "hardware") return Source::Hardware;
    return Source::System;
}
bool validSourceName(const std::string& value) noexcept {
    return value == "system" || value == "openssl" || value == "hardware";
}
const char* sourceName(Source value) noexcept {
    switch (value) {
    case Source::OpenSSL: return "openssl";
    case Source::Hardware: return "hardware";
    default: return "system";
    }
}
const char* providerName(Provider value) noexcept {
    switch (value) {
    case Provider::OpenSSL: return "openssl";
    case Provider::Hardware: return "hardware";
    case Provider::Pseudo: return "pseudo";
    default: return "system";
    }
}

bool hardwareAvailable() noexcept {
#ifdef DICE_SECURE_RANDOM_X86
    static const bool available = cpuHasRdseed();
    return available;
#else
    return false;
#endif
}

Status status() noexcept {
    return {selected.load(), static_cast<Provider>(lastProvider.load()), fallbackCount.load()};
}
void setFallbackObserver(FallbackObserver value) noexcept { observer.store(value); }
void setFailureInjectionForTest(FailureInjection injection) noexcept {
    failSystem.store(injection.system);
    failOpenssl.store(injection.openssl);
    failHardware.store(injection.hardware);
}

std::uint64_t word() noexcept {
    const Source want = selected.load(std::memory_order_relaxed);
    std::uint64_t base = 0;
    Provider baseProvider = Provider::Pseudo;
    if (want == Source::OpenSSL) {
        if (opensslWord(base)) baseProvider = Provider::OpenSSL;
        else if (systemWord(base)) baseProvider = Provider::System;
    } else {
        if (systemWord(base)) baseProvider = Provider::System;
        else if (opensslWord(base)) baseProvider = Provider::OpenSSL;
    }
    if (want == Source::Hardware) {
        std::uint64_t hw = 0;
        if (hardwareWord(hw))
            return report(want, Provider::Hardware, baseProvider == Provider::Pseudo ? hw : base ^ hw);
    }
    if (baseProvider == Provider::Pseudo) base = pseudoWord();
    return report(want, baseProvider, base);
}
} // namespace dice::secure_random
