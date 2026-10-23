#include "test_framework.h"

#include <brotli/decode.h>
#include <drogon/utils/Utilities.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The new error is provided by the upstream block-switch fix. Check the linked
// decoder as well as its headers: an old DLL with new headers must not pass.
TEST(BrotliSecurity, HeadersAndRuntimeIncludeBlockSwitchFix) {
    static_assert(BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH == -17);
    ASSERT_EQ(std::string(BrotliDecoderErrorString(
                  BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH)),
              std::string("_ERROR_FORMAT_BLOCK_SWITCH"));
}

TEST(BrotliSecurity, HttpUtilityPreservesLargeAndBinaryBodies) {
    std::string body;
    for (int i = 0; i < 4096; ++i) {
        body += "Dice!Next: 合法的 Brotli 响应必须继续正常解析。\n";
        body.push_back('\0');
        body.push_back(static_cast<char>(i % 256));
    }
    const auto compressed = drogon::utils::brotliCompress(body.data(), body.size());
    ASSERT_FALSE(compressed.empty());
    ASSERT_EQ(drogon::utils::brotliDecompress(compressed.data(), compressed.size()), body);
}

TEST(BrotliSecurity, RejectsInvalidAndTruncatedStreams) {
    const std::array<uint8_t, 4> invalid{0xff, 0xff, 0xff, 0xff};
    std::array<uint8_t, 128> output{};
    size_t outputSize = output.size();
    ASSERT_EQ(BrotliDecoderDecompress(invalid.size(), invalid.data(),
                                     &outputSize, output.data()),
              BROTLI_DECODER_RESULT_ERROR);

    const std::string body(8192, 'x');
    auto compressed = drogon::utils::brotliCompress(body.data(), body.size());
    ASSERT_TRUE(compressed.size() > 1);
    compressed.pop_back();
    std::vector<uint8_t> decoded(body.size());
    outputSize = decoded.size();
    ASSERT_EQ(BrotliDecoderDecompress(compressed.size(),
                  reinterpret_cast<const uint8_t*>(compressed.data()),
                  &outputSize, decoded.data()),
              BROTLI_DECODER_RESULT_ERROR);
}
