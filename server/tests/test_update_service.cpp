#include "test_framework.h"
#include "../src/service/update_service.h"

using namespace dice::update;

namespace {

std::string validManifest() {
    return R"json({
        "schema": 1,
        "repository": "DiceZone/Dice-Next",
        "tag": "v3.0.0-beta.900",
        "version": "3.0.0",
        "build": 900,
        "prerelease": true,
        "published_at": "2026-08-27T00:00:00Z",
        "release_url": "https://github.com/DiceZone/Dice-Next/releases/tag/v3.0.0-beta.900",
        "assets": [
            {
                "os": "windows",
                "arch": "amd64",
                "name": "DiceNext-beta-3.0.0(900)-windows-amd64.zip",
                "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                "size": 123456
            },
            {
                "os": "linux",
                "arch": "arm64",
                "name": "DiceNext-beta-3.0.0(900)-linux-arm64.tar.gz",
                "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
                "size": 654321
            }
        ]
    })json";
}

}  // namespace

TEST(UpdateManifest, ParsesAndSelectsExactPlatformAsset) {
    ReleaseManifest manifest;
    std::string error;
    ASSERT_TRUE(parseReleaseManifest(validManifest(), manifest, error));
    ASSERT_EQ(manifest.tag, std::string("v3.0.0-beta.900"));
    ASSERT_EQ(manifest.build, 900);

    const ReleaseAsset* windows = selectAsset(manifest, "windows", "amd64");
    ASSERT_TRUE(windows != nullptr);
    ASSERT_EQ(windows->size, static_cast<std::uint64_t>(123456));
    ASSERT_TRUE(selectAsset(manifest, "windows", "arm64") == nullptr);
}

TEST(UpdateManifest, RejectsWrongRepositoryAndDigest) {
    ReleaseManifest manifest;
    std::string error;
    std::string wrongRepository = validManifest();
    const std::string official = "DiceZone/Dice-Next";
    wrongRepository.replace(wrongRepository.find(official), official.size(), "someone/fork");
    ASSERT_FALSE(parseReleaseManifest(wrongRepository, manifest, error));

    std::string wrongDigest = validManifest();
    const std::string digest =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    wrongDigest.replace(wrongDigest.find(digest), digest.size(), "not-a-sha256");
    ASSERT_FALSE(parseReleaseManifest(wrongDigest, manifest, error));

    std::string wrongTag = validManifest();
    const std::string tag = "v3.0.0-beta.900";
    wrongTag.replace(wrongTag.find(tag), tag.size(), "v3.0.0-beta.901");
    ASSERT_FALSE(parseReleaseManifest(wrongTag, manifest, error));

    auto duplicateAsset = nlohmann::json::parse(validManifest());
    duplicateAsset["assets"].push_back(duplicateAsset["assets"][0]);
    ASSERT_FALSE(parseReleaseManifest(duplicateAsset.dump(), manifest, error));
}

TEST(UpdateVersion, ComparesSemanticVersionBeforeBuild) {
    ASSERT_TRUE(compareRelease("3.0.0", 899, "3.0.0", 900) < 0);
    ASSERT_TRUE(compareRelease("3.0.1", 1, "3.0.0", 9999) > 0);
    ASSERT_EQ(compareRelease("3.1.0", 5, "3.1.0", 5), 0);
}

TEST(UpdateArchive, RejectsAbsoluteAndTraversalEntries) {
    ASSERT_TRUE(archiveEntrySafe("DiceNext-beta/app/dice-next-core.exe"));
    ASSERT_TRUE(archiveEntrySafe("DiceNext-beta/web/dist/index.html"));
    ASSERT_FALSE(archiveEntrySafe("../outside.exe"));
    ASSERT_FALSE(archiveEntrySafe("DiceNext-beta/../../outside.exe"));
    ASSERT_FALSE(archiveEntrySafe("C:/Windows/System32/file.dll"));
    ASSERT_FALSE(archiveEntrySafe("/absolute/path"));
    ASSERT_FALSE(archiveEntrySafe("DiceNext-beta\\..\\outside.exe"));
}

TEST(UpdateMirror, PrefixesFullGithubUrl) {
    const std::string original =
        "https://github.com/DiceZone/Dice-Next/releases/latest/download/update-manifest.json";
    ASSERT_EQ(buildMirroredUrl(original, ""), original);
    ASSERT_EQ(buildMirroredUrl(original, "https://ghproxy.example"),
        std::string("https://ghproxy.example/") + original);
    ASSERT_EQ(buildMirroredUrl(original, "https://ghproxy.example/"),
        std::string("https://ghproxy.example/") + original);
}