#pragma once

// QQ 机器人 API v2 Webhook 的 Ed25519 签名。
// 种子算法与官方文档一致：把 Bot Secret 反复拼接，直到不少于 32 字节，再取前 32 字节。
// https://bot.q.qq.com/wiki/develop/api-v2/dev-prepare/interface-framework/sign.html
// https://bot.q.qq.com/wiki/develop/api-v2/dev-prepare/event-emit/webhook.html

#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <string>

namespace dice::qqwebhook {

inline bool isWebhook(std::string mode) {
    for (char& ch : mode) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return mode == "webhook";
}

inline std::string seedFromSecret(const std::string& secret) {
    std::string seed = secret;
    while (seed.size() < 32) {
        if (seed.empty()) return {};
        seed += seed;
    }
    seed.resize(32);
    return seed;
}

inline std::string toHex(const unsigned char* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0f];
    }
    return out;
}

inline bool fromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](unsigned char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(static_cast<unsigned char>(hex[i]));
        const int lo = nibble(static_cast<unsigned char>(hex[i + 1]));
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

inline EVP_PKEY* privateKey(const std::string& secret) {
    const std::string seed = seedFromSecret(secret);
    if (seed.size() != 32) return nullptr;
    return EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(seed.data()), seed.size());
}

inline std::string sign(const std::string& secret, const std::string& message) {
    EVP_PKEY* key = privateKey(secret);
    EVP_MD_CTX* ctx = key ? EVP_MD_CTX_new() : nullptr;
    std::array<unsigned char, 64> signature{};
    std::size_t size = signature.size();
    const bool ok = key && ctx
        && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestSign(ctx, signature.data(), &size,
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1
        && size == signature.size();
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok ? toHex(signature.data(), signature.size()) : std::string();
}

// 校验 X-Signature-Timestamp + 原始 HTTP body。签名必须是 128 位十六进制，
// 且最后一字节的高三位为 0（与官方 Go 示例的规范签名检查一致）。
inline bool verify(const std::string& secret, const std::string& timestamp,
                   const std::string& body, const std::string& signatureHex) {
    if (secret.empty() || timestamp.empty() || signatureHex.size() != 128) return false;
    std::string signature;
    if (!fromHex(signatureHex, signature) || signature.size() != 64) return false;
    if ((static_cast<unsigned char>(signature.back()) & 224) != 0) return false;
    EVP_PKEY* secretKey = privateKey(secret);
    std::array<unsigned char, 32> rawPublic{};
    std::size_t publicSize = rawPublic.size();
    EVP_PKEY* key = secretKey && EVP_PKEY_get_raw_public_key(secretKey, rawPublic.data(), &publicSize) == 1
        ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, rawPublic.data(), publicSize) : nullptr;
    EVP_PKEY_free(secretKey);
    EVP_MD_CTX* ctx = key ? EVP_MD_CTX_new() : nullptr;
    const std::string message = timestamp + body;
    const bool ok = key && ctx
        && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

inline std::string validationSignature(const std::string& secret, const std::string& eventTs,
                                       const std::string& plainToken) {
    return sign(secret, eventTs + plainToken);
}

}  // namespace dice::qqwebhook
