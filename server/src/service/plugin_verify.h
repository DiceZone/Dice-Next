#pragma once
// ─── 可选插件签名校验 ──────────────────────────────────────
// 配置 dice/plugin_verify_key（PEM 公钥）后，上传的 JS/Lua 插件必须携带
// RSA-SHA256 签名（请求体 "signature" 字段，base64），否则拒绝安装。
// 默认未配置公钥 → 不校验（保持原有行为）。签名工具见 tools/sign-plugin.py。

#include <nlohmann/json.hpp>
#include <drogon/utils/Utilities.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <string>

namespace dice::pluginverify {

/// 校验上传字节的 RSA-SHA256 签名。未配置公钥时直接放行。
inline bool verify(const std::string& publicKeyPem, const std::string& bytes,
                   const std::string& signatureB64, std::string& err) {
    if (publicKeyPem.empty()) return true;   // 未启用
    if (signatureB64.empty()) { err = "已启用插件签名校验，但上传未附带签名"; return false; }
    BIO* bio = BIO_new_mem_buf(publicKeyPem.data(), static_cast<int>(publicKeyPem.size()));
    if (!bio) { err = "签名公钥解析失败"; return false; }
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) { err = "签名公钥无效（需 PEM 公钥）"; return false; }
    const std::string sig = drogon::utils::base64Decode(signatureB64);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx
        && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1
        && EVP_DigestVerifyUpdate(ctx, bytes.data(), bytes.size()) == 1
        && EVP_DigestVerifyFinal(ctx,
                                 reinterpret_cast<const unsigned char*>(sig.data()),
                                 sig.size()) == 1) {
        ok = true;
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    if (!ok) { err = "插件签名校验失败"; return false; }
    return true;
}

}  // namespace dice::pluginverify
