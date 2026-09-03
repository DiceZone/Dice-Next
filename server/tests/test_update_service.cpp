#include "test_framework.h"
#include "../src/common/subprocess.h"
#include "../src/service/update_service.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <thread>

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

std::string validManifestForCurrentPlatform() {
    auto manifest = nlohmann::json::parse(validManifest());
    auto asset = manifest["assets"][0];
    manifest["assets"] = nlohmann::json::array({std::move(asset)});
    manifest["assets"][0]["os"] = currentOs();
    manifest["assets"][0]["arch"] = currentArch();
    return manifest.dump();
}

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& next)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }
    ~ScopedCurrentPath() {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
    }

private:
    std::filesystem::path previous_;
};

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
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

TEST(UpdateArchive, AcceptsCurrentAppRuntimeAndLegacyLibLayouts) {
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_update_package_" + std::to_string(nonce));
    const auto touch = [&](const fs::path& relative) {
        fs::create_directories((root / relative).parent_path());
        std::ofstream(root / relative, std::ios::binary) << 'x';
    };

    touch("dice-next.exe");
    touch(fs::path("app") / "dice-next-core.exe");
    touch(fs::path("web") / "dist" / "index.html");
    touch(fs::path("docs") / "roadmap.md");
    fs::create_directories(root / "i18n");
    touch(fs::path("app") / "msvcp140.dll");
    touch(fs::path("app") / "vcruntime140.dll");
    touch(fs::path("app") / "vcruntime140_1.dll");
    ASSERT_TRUE(missingWindowsPackageComponents(root).empty());

    fs::remove(root / "app" / "msvcp140.dll");
    ASSERT_FALSE(missingWindowsPackageComponents(root).empty());
    fs::create_directories(root / "lib");
    ASSERT_TRUE(missingWindowsPackageComponents(root).empty());

    std::error_code ec;
    fs::remove_all(root, ec);
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

TEST(UpdateAssetName, RecoversGithubNormalizedLegacyParentheses) {
    const auto legacy = githubAssetNameCandidates(
        "DiceNext-beta-3.0.0(873)-windows-amd64.zip");
    ASSERT_EQ(legacy.size(), static_cast<std::size_t>(2));
    ASSERT_EQ(legacy[0], std::string("DiceNext-beta-3.0.0(873)-windows-amd64.zip"));
    ASSERT_EQ(legacy[1], std::string("DiceNext-beta-3.0.0.873.-windows-amd64.zip"));

    const auto safe = githubAssetNameCandidates(
        "DiceNext-beta-3.0.0-874-windows-amd64.zip");
    ASSERT_EQ(safe.size(), static_cast<std::size_t>(1));
    ASSERT_EQ(safe[0], std::string("DiceNext-beta-3.0.0-874-windows-amd64.zip"));
}

TEST(UpdateContainer, DetectsExplicitAndRuntimeFallbackSignals) {
    ContainerDetectionInput explicitMarker;
    explicitMarker.diceNextMarker = " docker ";
    auto detected = detectContainerEnvironment(explicitMarker);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("docker"));
    ASSERT_EQ(detected.evidence, std::string("DICENEXT_CONTAINER"));

    ContainerDetectionInput kubernetes;
    kubernetes.kubernetesServiceHost = "10.96.0.1";
    detected = detectContainerEnvironment(kubernetes);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("kubernetes"));

    ContainerDetectionInput podman;
    podman.containerEnvFile = true;
    detected = detectContainerEnvironment(podman);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("podman"));

    ContainerDetectionInput systemd;
    systemd.systemdMarker = "systemd-nspawn\n";
    detected = detectContainerEnvironment(systemd);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("container"));
    ASSERT_EQ(detected.evidence, std::string("/run/systemd/container"));

    ContainerDetectionInput dotnet;
    dotnet.dotnetMarker = "true";
    detected = detectContainerEnvironment(dotnet);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("container"));
    ASSERT_EQ(detected.evidence, std::string("DOTNET_RUNNING_IN_CONTAINER"));

    ContainerDetectionInput cgroup;
    cgroup.cgroup = "0::/system.slice/docker-0123456789abcdef.scope";
    detected = detectContainerEnvironment(cgroup);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("docker"));

    ContainerDetectionInput mountInfo;
    mountInfo.mountInfo =
        "36 25 0:32 / / rw,relatime - overlay overlay "
        "rw,lowerdir=/var/lib/docker/overlay2/l";
    detected = detectContainerEnvironment(mountInfo);
    ASSERT_TRUE(detected.detected);
    ASSERT_EQ(detected.type, std::string("docker"));
}

TEST(UpdateContainer, IgnoresFalseMarkersAndBlocksEveryMutationEntry) {
    ContainerDetectionInput host;
    host.diceNextMarker = "false";
    host.standardMarker = "off";
    host.cgroup = "0::/system.slice/docker-dice-next.service";
    host.mountInfo =
        "36 25 8:1 / / rw,relatime - ext4 /dev/sda1 rw\n"
        "100 36 0:45 / /var/lib/docker/containers/abc/mounts/shm rw - tmpfs shm rw";
    ASSERT_FALSE(detectContainerEnvironment(host).detected);

    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path configPath = fs::temp_directory_path() /
        ("dice_next_container_update_config_" + std::to_string(nonce));
    dice::ConfigManager config(configPath.string());
    config.set<std::string>("update/auto_action", "install");
    {
        UpdateService service(config, [] {}, {},
            ContainerEnvironment{true, "docker", "DICENEXT_CONTAINER"});
        const auto status = service.status();
        ASSERT_FALSE(status.value("downloadSupported", true));
        ASSERT_FALSE(status.value("installSupported", true));
        ASSERT_EQ(status.value("selfUpdateBlockedReason", std::string()),
            std::string("container"));
        ASSERT_TRUE(status.at("runtime").value("container", false));
        ASSERT_EQ(status.at("runtime").value("containerType", std::string()),
            std::string("docker"));
        ASSERT_EQ(status.at("settings").value("autoAction", std::string()),
            std::string("notify"));

        std::string error;
        ASSERT_FALSE(service.requestDownload(error));
        ASSERT_TRUE(error.find("disabled inside containers") != std::string::npos);
        error.clear();
        ASSERT_FALSE(service.requestInstall(error));
        ASSERT_TRUE(error.find("disabled inside containers") != std::string::npos);
        error.clear();
        ASSERT_FALSE(service.updateSettings(
            nlohmann::json{{"autoAction", "download"}}, error));
        ASSERT_TRUE(error.find("disabled inside containers") != std::string::npos);
        error.clear();
        ASSERT_FALSE(service.updateSettings(
            nlohmann::json{{"autoAction", "install"}}, error));
        ASSERT_TRUE(error.find("disabled inside containers") != std::string::npos);
    }
    std::error_code ec;
    fs::remove_all(configPath, ec);
}

TEST(UpdateService, PortableWorkerStartsAndStopsCleanly) {
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path configPath = fs::temp_directory_path() /
        ("dice_next_update_config_" + std::to_string(nonce));
    dice::ConfigManager config(configPath.string());
    {
        UpdateService service(config, [] {});
        ASSERT_EQ(service.status().value("phase", std::string()), std::string("idle"));
    }
    std::error_code ec;
    fs::remove_all(configPath, ec);
}

TEST(UpdateSubprocess, CancellationTerminatesAndReapsChildPromptly) {
    std::atomic<bool> cancel{false};
    std::thread trigger([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        cancel.store(true, std::memory_order_release);
    });
    const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
    const auto result = dice::proc::runCancellable(
        dice::proc::systemTool("ping.exe"), {"-n", "30", "127.0.0.1"},
        [&] { return cancel.load(std::memory_order_acquire); }, 1024);
#else
    const auto result = dice::proc::runCancellable(
        "sleep", {"30"}, [&] { return cancel.load(std::memory_order_acquire); }, 1024);
#endif
    trigger.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(result.cancelled);
    ASSERT_TRUE(elapsed < std::chrono::seconds(2));
}

TEST(UpdateService, StopCancelsAnActiveManifestFetch) {
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_update_cancel_" + std::to_string(nonce));
    fs::create_directories(root);
    {
        ScopedCurrentPath currentPath(root);
        dice::ConfigManager config((root / "config").string());
        config.set<std::string>("update/source", "direct");
        std::atomic<bool> started{false};
        std::atomic<bool> observedCancellation{false};
        auto fetch = [&](const std::string&, const fs::path&, std::uint64_t, int,
                         std::string& error,
                         const UpdateService::CancellationCheck& cancelled) {
            started.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                if (cancelled()) {
                    observedCancellation.store(true, std::memory_order_release);
                    error = "cancelled";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            error = "test fetch timeout";
            return false;
        };

        auto service = std::make_unique<UpdateService>(
            config, [] {}, UpdateService::NotifyCallback{}, ContainerEnvironment{}, fetch);
        std::string error;
        ASSERT_TRUE(service->requestCheck(true, error));
        ASSERT_TRUE(waitUntil([&] { return started.load(std::memory_order_acquire); }));
        const auto stoppingAt = std::chrono::steady_clock::now();
        service.reset();
        const auto stopElapsed = std::chrono::steady_clock::now() - stoppingAt;
        ASSERT_TRUE(observedCancellation.load(std::memory_order_acquire));
        ASSERT_TRUE(stopElapsed < std::chrono::seconds(1));
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

TEST(UpdateService, ManifestRaceUsesFirstSuccessAndCancelsSlowSources) {
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("dice_next_update_race_" + std::to_string(nonce));
    fs::create_directories(root);
    {
        ScopedCurrentPath currentPath(root);
        std::ofstream("update-mirrors.json", std::ios::binary)
            << R"json({"mirrors":["https://slow.example"]})json";
        dice::ConfigManager config((root / "config").string());
        config.set<std::string>("update/source", "auto");
        std::atomic<bool> slowStarted{false};
        std::atomic<bool> slowCancelled{false};
        std::mutex pathsMutex;
        std::vector<fs::path> outputPaths;
        const std::string manifest = validManifestForCurrentPlatform();
        auto fetch = [&](const std::string& url, const fs::path& output, std::uint64_t,
                         int, std::string& error,
                         const UpdateService::CancellationCheck& cancelled) {
            {
                std::lock_guard<std::mutex> lock(pathsMutex);
                outputPaths.push_back(output.filename());
            }
            if (url.rfind("https://slow.example/", 0) == 0) {
                slowStarted.store(true, std::memory_order_release);
                while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                slowCancelled.store(true, std::memory_order_release);
                error = "cancelled";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            fs::create_directories(output.parent_path());
            std::ofstream(output, std::ios::binary) << manifest;
            return true;
        };

        UpdateService service(config, [] {}, UpdateService::NotifyCallback{},
                              ContainerEnvironment{}, fetch);
        std::string error;
        const auto startedAt = std::chrono::steady_clock::now();
        ASSERT_TRUE(service.requestCheck(true, error));
        ASSERT_TRUE(waitUntil([&] {
            const auto phase = service.status().value("phase", std::string());
            return phase == "available" || phase == "up_to_date";
        }));
        const auto elapsed = std::chrono::steady_clock::now() - startedAt;
        ASSERT_TRUE(slowStarted.load(std::memory_order_acquire));
        ASSERT_TRUE(slowCancelled.load(std::memory_order_acquire));
        ASSERT_TRUE(elapsed < std::chrono::seconds(1));
        {
            std::lock_guard<std::mutex> lock(pathsMutex);
            ASSERT_EQ(outputPaths.size(), static_cast<std::size_t>(2));
            ASSERT_NE(outputPaths[0], outputPaths[1]);
            const std::regex uniqueName(R"(^manifest-[0-9]+-[0-9a-f]+-[0-9]+\.json$)");
            ASSERT_TRUE(std::regex_match(outputPaths[0].string(), uniqueName));
            ASSERT_TRUE(std::regex_match(outputPaths[1].string(), uniqueName));
        }
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
