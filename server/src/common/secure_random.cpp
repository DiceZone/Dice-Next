#include "secure_random.h"
#include <atomic>
#include <cerrno>
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

namespace dice::secure_random {
namespace { std::atomic<Source> selected{Source::System}; }
void setSource(Source value) noexcept { selected.store(value); }
Source source() noexcept { return selected.load(); }
std::uint64_t word() {
    std::uint64_t value{};
    if (source() == Source::OpenSSL) {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1)
            throw std::runtime_error("OpenSSL secure random source unavailable");
    } else {
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            throw std::runtime_error("Windows secure random source unavailable");
#elif defined(__APPLE__)
        int result;
        do { result = getentropy(&value, sizeof(value)); } while (result != 0 && errno == EINTR);
        if (result != 0) throw std::runtime_error("macOS secure random source unavailable");
#else
        auto* output = reinterpret_cast<unsigned char*>(&value);
        std::size_t offset = 0;
        while (offset < sizeof(value)) {
            const auto count = getrandom(output + offset, sizeof(value) - offset, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Linux secure random source unavailable");
            offset += static_cast<std::size_t>(count);
        }
#endif
    }
    return value;
}
} // namespace dice::secure_random
