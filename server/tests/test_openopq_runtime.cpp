#include "test_framework.h"
#include "../src/service/openopq_runtime.h"
#include <openssl/evp.h>
#include <array>
#include <stdexcept>

using namespace dice::openopq;

namespace {
// 固定测试密钥（私钥 = 字节 0..31）签的清单，正式公钥不会接受它。
const char* kTestPublicKey = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
const char* kPayload = R"({"schema":1,"version":"v1.3.0-rc.1","qq_version":"QQNT-v7.0.2-53644","sign_url":"https://sign-v1.dice.zone/k/{key}/sign","cdn_lists":["https://cdn.example.invalid/a"],"assets":[{"os":"windows","arch":"amd64","url":"https://cloud.dice.zone/openopq/v1.3.0-rc.1/windows-amd64/openopq.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":3627896}]})";
const char* kSignature = "QdxoCAWqcYgOXo4UCP9UU/R5Jo+Fj6CDvG5d/WwIQN+ibzvDn7y1PbwbBuuGViutNPAqWjgs4OfvKDmaiC7QBg==";

std::string envelope(const std::string& payload, const std::string& signature) {
    return nlohmann::json{{"payload", payload}, {"signature", signature}}.dump();
}

// 只使用公开的测试私钥，让格式错误的样本也通过签名层，真正覆盖字段校验。
std::string signedFixture(const nlohmann::json& doc) {
    std::array<unsigned char, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(i);
    const auto payload = doc.dump();
    auto* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    auto* ctx = EVP_MD_CTX_new();
    std::array<unsigned char, 64> signature{};
    std::size_t size = signature.size();
    const bool ok = key && ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(ctx, signature.data(), &size,
                       reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok) throw std::runtime_error("Cannot sign test manifest");
    std::array<unsigned char, 89> encoded{};
    const int length = EVP_EncodeBlock(encoded.data(), signature.data(), static_cast<int>(size));
    return envelope(payload, std::string(reinterpret_cast<const char*>(encoded.data()), length));
}
}  // namespace

TEST(OpenOpqRuntime, AcceptsManifestSignedByTheKey) {
    Manifest manifest;
    std::string error;
    ASSERT_TRUE(parseSignedManifest(envelope(kPayload, kSignature), kTestPublicKey, manifest, error));
    ASSERT_EQ(manifest.version, std::string("v1.3.0-rc.1"));
    ASSERT_EQ(manifest.qqVersion, std::string("QQNT-v7.0.2-53644"));
    ASSERT_EQ(manifest.assets.size(), std::size_t{1});
    const Asset* asset = assetFor(manifest, "windows", "amd64");
    ASSERT_TRUE(asset != nullptr);
    ASSERT_EQ(asset->size, std::uint64_t{3627896});
    ASSERT_TRUE(assetFor(manifest, "linux", "arm64") == nullptr);
}

TEST(OpenOpqRuntime, RejectsTamperedOrForeignManifest) {
    Manifest manifest;
    std::string error;
    std::string tampered = kPayload;
    tampered.replace(tampered.find("3627896"), 7, "3627897");
    ASSERT_FALSE(parseSignedManifest(envelope(tampered, kSignature), kTestPublicKey, manifest, error));
    // 正式公钥不认测试密钥签的清单
    ASSERT_FALSE(parseSignedManifest(envelope(kPayload, kSignature),
                                     "eQKl/N6UbaUw8g4OW9R+UfiH40rXSvcVFmHrC6xE3OI=", manifest, error));
    ASSERT_FALSE(parseSignedManifest("{\"payload\":\"{}\"}", kTestPublicKey, manifest, error));
    ASSERT_FALSE(parseSignedManifest("not json", kTestPublicKey, manifest, error));
}

TEST(OpenOpqRuntime, RejectsSignedMalformedFieldsWithoutChangingOutput) {
    const auto valid = nlohmann::json::parse(kPayload);
    Manifest manifest;
    std::string error;
    ASSERT_TRUE(parseSignedManifest(signedFixture(valid), kTestPublicKey, manifest, error));
    for (const auto* field : {"schema", "version", "qq_version", "sign_url", "cdn_lists", "assets"}) {
        for (const auto& invalid : nlohmann::json::array({nullptr, false, 42, nlohmann::json::object()})) {
            auto doc = valid;
            doc[field] = invalid;
            ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
            ASSERT_FALSE(error.empty());
            ASSERT_EQ(manifest.version, std::string("v1.3.0-rc.1"));
        }
        auto doc = valid;
        doc.erase(field);
        if (std::string(field) != "cdn_lists")
            ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
    }
    for (const auto* field : {"os", "arch", "url", "sha256", "size"}) {
        for (const auto& invalid : nlohmann::json::array({nullptr, false, -1, 0, 1.5, nlohmann::json::object()})) {
            auto doc = valid;
            doc["assets"][0][field] = invalid;
            ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
            ASSERT_FALSE(error.empty());
        }
    }
    for (const auto& invalidSize : nlohmann::json::array({"3627896", 134217729, 18446744073709551615ull})) {
        auto doc = valid;
        doc["assets"][0]["size"] = invalidSize;
        ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
    }
    for (const auto* invalidVersion : {".", "..", "../escape", "a/b", "a\\b"}) {
        auto doc = valid;
        doc["version"] = invalidVersion;
        ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
    }
    for (const auto& assets : {nlohmann::json::array(), nlohmann::json::array({nullptr})}) {
        auto doc = valid;
        doc["assets"] = assets;
        ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
    }
    auto doc = valid;
    doc["schema"] = "1";
    ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
    doc = valid;
    doc["cdn_lists"] = nlohmann::json::array({nullptr});
    ASSERT_FALSE(parseSignedManifest(signedFixture(doc), kTestPublicKey, manifest, error));
    ASSERT_FALSE(parseSignedManifest(std::string(256 * 1024 + 1, ' '), kTestPublicKey, manifest, error));
}

TEST(OpenOpqRuntime, KeyAndAccountFormats) {
    ASSERT_TRUE(validApiKey("bdc_diceflats_0123456789abcdef"));
    ASSERT_FALSE(validApiKey(""));
    ASSERT_FALSE(validApiKey("sk-0123456789abcdef"));
    ASSERT_FALSE(validApiKey("bdc_abc/../../x"));
    ASSERT_FALSE(validApiKey("bdc_has space here"));
    ASSERT_TRUE(validAccount("123456789"));
    ASSERT_FALSE(validAccount("0123456"));
    ASSERT_FALSE(validAccount("12ab5678"));
    ASSERT_FALSE(validAccount("1234"));
}

TEST(OpenOpqRuntime, ConfigSignsWithKeyAndEnablesOneBot) {
    Manifest manifest;
    std::string error;
    ASSERT_TRUE(parseSignedManifest(envelope(kPayload, kSignature), kTestPublicKey, manifest, error));
    const auto config = buildConfig(manifest, "bdc_diceflats_0123456789abcdef", "123456789");
    ASSERT_EQ(config["FakeSignApi"].get<std::string>(),
              std::string("https://sign-v1.dice.zone/k/bdc_diceflats_0123456789abcdef/sign"));
    ASSERT_EQ(config["Version"].get<std::string>(), std::string("QQNT-v7.0.2-53644"));
    ASSERT_EQ(config["Protocol"].get<std::string>(), std::string("onebot11"));
    // 默认正向：v1.3.0-rc.1 没有反向 WS，配置里不该有连回 DN 的项
    ASSERT_FALSE(config["OneBot"].contains("reverse_ws"));
    ASSERT_EQ(config["Accounts"][0].get<std::string>(), std::string("123456789"));
}

TEST(OpenOpqRuntime, ReverseTransportWritesBackConnection) {
    Manifest manifest;
    std::string error;
    ASSERT_TRUE(parseSignedManifest(envelope(kPayload, kSignature), kTestPublicKey, manifest, error));
    const auto config = buildConfig(manifest, "bdc_diceflats_0123456789abcdef", "123456789", 40123, "tok");
    const auto& reverse = config["OneBot"]["reverse_ws"][0];
    ASSERT_EQ(reverse["url"].get<std::string>(), std::string("ws://127.0.0.1:40123/"));
    ASSERT_EQ(reverse["token"].get<std::string>(), std::string("tok"));
    ASSERT_TRUE(validTransport("forward_ws"));
    ASSERT_TRUE(validTransport("reverse_ws"));
    ASSERT_FALSE(validTransport("http"));
}

TEST(OpenOpqRuntime, PicksDistinctLoopbackPortAndToken) {
    const int port = pickFreeLoopbackPort();
    ASSERT_TRUE(port > 0 && port < 65536);
    const auto a = randomToken();
    const auto b = randomToken();
    ASSERT_EQ(a.size(), std::size_t{32});
    ASSERT_NE(a, b);
}
