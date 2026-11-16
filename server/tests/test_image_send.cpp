#include "test_framework.h"
#include "../src/service/image_resource_service.h"
#include "../src/service/image_send.h"
#include "../src/service/image_transcode.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace dice::image_resources;
namespace fs = std::filesystem;

namespace {
const std::string jpeg = std::string("\xff\xd8\xff\xe0", 4) + "fixture-jpeg-body";
struct SendEnv {
    fs::path folder;
    std::shared_ptr<Service> service;
    std::shared_ptr<Service> previous = current();
    SendEnv() {
        static std::atomic<int> counter{0};
        folder = fs::temp_directory_path() / ("dice-next-imgsend-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(counter++));
        service = std::make_shared<Service>(folder / "data");
        setDefault(service);
    }
    ~SendEnv() {
        setDefault(std::move(previous));
        std::error_code ec;
        fs::remove_all(folder, ec);
    }
};
}

TEST(ImageUri, StripFileUriNormalizesWindowsAndRelativeForms) {
    ASSERT_EQ(stripFileUri("data/images/TAROT/a17.jpg"), std::string("data/images/TAROT/a17.jpg"));
    ASSERT_EQ(stripFileUri("file://data/images/TAROT/a17.jpg"), std::string("data/images/TAROT/a17.jpg"));
    ASSERT_EQ(stripFileUri("file:///C:/TAROT/a17.png"), std::string("C:/TAROT/a17.png"));
    ASSERT_EQ(stripFileUri("file://C:/TAROT/a17.png"), std::string("C:/TAROT/a17.png"));
    ASSERT_EQ(stripFileUri("/file:///C:/TAROT/a17.png"), std::string("C:/TAROT/a17.png"));
    ASSERT_EQ(stripFileUri("file:///home/bot/a.png"), std::string("/home/bot/a.png"));
}

TEST(ImageUri, ToFileUriUsesThreeSlashesOnWindowsDrive) {
#ifdef _WIN32
    const auto uri = toFileUri(fromUtf8("C:/TAROT/a17.png"));
    ASSERT_TRUE(uri.rfind("file:///C:/", 0) == 0);
    ASSERT_TRUE(uri.find("file:////") == std::string::npos);
    ASSERT_TRUE(uri.find("/file:") == std::string::npos);
#else
    const auto uri = toFileUri(fromUtf8("/tmp/a17.png"));
    ASSERT_TRUE(uri.rfind("file:///", 0) == 0);
#endif
}

TEST(ImageSend, DeckManagedPathBecomesBase64NotRelativeCachePath) {
    SendEnv env;
    env.service->upload("TAROT", "a17.jpg", jpeg);
    const auto out = dice::imgsend::resolve("data/images/TAROT/a17.jpg");
    ASSERT_TRUE(out.rfind("base64://", 0) == 0);
    ASSERT_TRUE(out.find("data/images") == std::string::npos);
    ASSERT_TRUE(out.find("file:") == std::string::npos);
    ASSERT_TRUE(dice::imgsend::resolve("file://data/images/TAROT/a17.jpg").rfind("base64://", 0) == 0);
    ASSERT_EQ(dice::imgsend::resolve("data/images/TAROT/missing.jpg"), std::string());
    ASSERT_EQ(dice::imgsend::resolve("/file:///C:/TAROT/a17.png"), std::string());
}

TEST(ImageSend, LocalPeerUsesAbsoluteNativePathNotFileUri) {
    SendEnv env;
    env.service->upload("TAROT", "a17.jpg", jpeg);
    const auto out = dice::imgsend::resolve("data/images/TAROT/a17.jpg", "onebot_v11", "", true);
    ASSERT_TRUE(out.find("file:") == std::string::npos);
    ASSERT_TRUE(out.find("base64://") == std::string::npos);
    ASSERT_TRUE(out.rfind("data/images/", 0) != 0);
    const auto path = fromUtf8(out);
    ASSERT_TRUE(path.is_absolute());
    ASSERT_TRUE(fs::is_regular_file(path));
    ASSERT_TRUE(fs::equivalent(path, env.service->checkedPath("TAROT/a17.jpg")));
#ifdef _WIN32
    ASSERT_TRUE(out.find('\\') != std::string::npos);
    ASSERT_TRUE(out.find("://") == std::string::npos);
#endif
    ASSERT_TRUE(fs::equivalent(fromUtf8(dice::imgsend::resolve("file://data/images/TAROT/a17.jpg", "onebot_v11", "", true)),
        env.service->checkedPath("TAROT/a17.jpg")));
    ASSERT_EQ(dice::imgsend::resolve("data/images/TAROT/missing.jpg", "onebot_v11", "", true), std::string());
    ASSERT_EQ(dice::imgsend::resolve("/file:///C:/TAROT/a17.png", "onebot_v11", "", true), std::string());
}

TEST(ImageSend, RewritesNativePathSegmentsToBase64) {
    SendEnv env;
    env.service->upload("TAROT", "a17.jpg", jpeg);
    const auto native = dice::imgsend::resolve("data/images/TAROT/a17.jpg", "onebot_v11", "", true);
    nlohmann::json message = {
        {{"type", "image"}, {"data", {{"file", native}}}},
        {{"type", "text"}, {"data", {{"text", "大吉"}}}}
    };
    ASSERT_TRUE(dice::imgsend::messageHasLocalImage(message));
    ASSERT_TRUE(dice::imgsend::rewriteLocalImagesToBase64(message));
    ASSERT_TRUE(message[0]["data"]["file"].get<std::string>().rfind("base64://", 0) == 0);
    ASSERT_EQ(message[1]["data"]["text"].get<std::string>(), std::string("大吉"));
    ASSERT_FALSE(dice::imgsend::messageHasLocalImage(message));
    ASSERT_FALSE(dice::imgsend::rewriteLocalImagesToBase64(message));
}

TEST(ImageSend, SniffsRealFormatInsteadOfExtension) {
    using dice::image_transcode::sniff;
    using dice::image_transcode::Kind;
    ASSERT_TRUE(sniff(jpeg) == Kind::jpeg);
    ASSERT_TRUE(sniff(std::string("\x89PNG\r\n\x1a\n", 8) + "x") == Kind::png);
    ASSERT_TRUE(sniff(std::string("\x00\x00\x00\x1c""ftypavif", 12)) == Kind::avif);
    ASSERT_TRUE(dice::image_transcode::qqImageOk(Kind::jpeg));
    ASSERT_TRUE(dice::image_transcode::qqImageOk(Kind::png));
    ASSERT_FALSE(dice::image_transcode::qqImageOk(Kind::avif));
    ASSERT_TRUE(dice::image_transcode::officialMaterialOk(Kind::gif));
    ASSERT_TRUE(dice::image_transcode::officialMaterialOk(Kind::webp));
    ASSERT_FALSE(dice::image_transcode::officialMaterialOk(Kind::avif));
}

TEST(ImageSend, MaterialBytesFixesMismatchedExtensionAndKeepsMatchingJpeg) {
    using dice::image_transcode::prepareMaterialBytes;
    using dice::image_transcode::Kind;
    const auto png = std::string("\x89PNG\r\n\x1a\n", 8) + "x";
    const auto pngNamedJpg = prepareMaterialBytes(png, "card.jpg");
    ASSERT_TRUE(pngNamedJpg.has_value());
    ASSERT_EQ(pngNamedJpg->mime, std::string("image/png"));
    ASSERT_EQ(pngNamedJpg->fileName, std::string("card.png"));
    ASSERT_FALSE(pngNamedJpg->transcoded);
    ASSERT_EQ(pngNamedJpg->bytes, png);
    const auto jpegOk = prepareMaterialBytes(jpeg, "card.jpg");
    ASSERT_TRUE(jpegOk.has_value());
    ASSERT_EQ(jpegOk->mime, std::string("image/jpeg"));
    ASSERT_EQ(jpegOk->fileName, std::string("card.jpg"));
    ASSERT_FALSE(jpegOk->transcoded);
    ASSERT_TRUE(jpegOk->kind == Kind::jpeg);
}

#ifdef _WIN32
static std::string bmp1x1() {
    static const unsigned char kBytes[] = {
        0x42,0x4D,0x3A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x36,0x00,0x00,0x00,
        0x28,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
        0x01,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0xFF,0x00
    };
    return {reinterpret_cast<const char*>(kBytes), sizeof(kBytes)};
}

TEST(ImageSend, TranscodesBmpToJpegBeforeQqSend) {
    SendEnv env;
    env.service->upload("TAROT", "card.bmp", bmp1x1());
    const auto out = dice::imgsend::resolve("data/images/TAROT/card.bmp", "onebot_v11", "", true);
    ASSERT_FALSE(out.empty());
    ASSERT_TRUE(out.find("base64://") == std::string::npos);
    const auto path = fromUtf8(out);
    ASSERT_TRUE(fs::is_regular_file(path));
    ASSERT_FALSE(fs::equivalent(path, env.service->checkedPath("TAROT/card.bmp")));
    std::ifstream in(path, std::ios::binary);
    char mag[3]{};
    in.read(mag, 3);
    ASSERT_EQ(std::string(mag, 3), std::string("\xff\xd8\xff", 3));
}

TEST(ImageSend, MaterialBytesTranscodesBmpToPng) {
    const auto out = dice::image_transcode::prepareMaterialBytes(bmp1x1(), "card.bmp");
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->mime, std::string("image/png"));
    ASSERT_TRUE(out->transcoded);
    ASSERT_TRUE(dice::image_transcode::sniff(out->bytes) == dice::image_transcode::Kind::png);
}

#endif
