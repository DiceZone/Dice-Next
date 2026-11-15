#include "test_framework.h"
#include "../src/service/image_resource_service.h"
#include "../src/service/image_send.h"

#include <atomic>
#include <chrono>
#include <filesystem>
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

TEST(ImageSend, LocalPeerUsesAbsoluteFileUriNotCacheRelativePath) {
    SendEnv env;
    env.service->upload("TAROT", "a17.jpg", jpeg);
    const auto out = dice::imgsend::resolve("data/images/TAROT/a17.jpg", "onebot_v11", "", true);
    ASSERT_TRUE(out.rfind("file:///", 0) == 0);
    ASSERT_TRUE(out.find("file:////") == std::string::npos);
    ASSERT_TRUE(out.find("/file:") == std::string::npos);
    ASSERT_TRUE(out.find("base64://") == std::string::npos);
    const auto path = fromUtf8(stripFileUri(out));
    ASSERT_TRUE(fs::is_regular_file(path));
    ASSERT_TRUE(fs::equivalent(path, env.service->checkedPath("TAROT/a17.jpg")));
    ASSERT_TRUE(dice::imgsend::resolve("file://data/images/TAROT/a17.jpg", "onebot_v11", "", true)
        .rfind("file:///", 0) == 0);
    ASSERT_EQ(dice::imgsend::resolve("data/images/TAROT/missing.jpg", "onebot_v11", "", true), std::string());
    ASSERT_EQ(dice::imgsend::resolve("/file:///C:/TAROT/a17.png", "onebot_v11", "", true), std::string());
}
