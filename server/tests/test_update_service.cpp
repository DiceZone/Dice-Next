#include "test_framework.h"
#include "../src/common/subprocess.h"
#include "../src/common/version.h"
#include "../src/common/update_schedule.h"
#include "../src/common/utils.h"
#include "../src/service/update_service.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <regex>
#include <thread>
#include <openssl/evp.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

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

class DownloadFixture {
public:
    DownloadFixture() : root(std::filesystem::temp_directory_path() /
        ("dice_next_download_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(root);
        current_ = std::make_unique<ScopedCurrentPath>(root);
    }
    ~DownloadFixture() {
        current_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
private:
    std::unique_ptr<ScopedCurrentPath> current_;
};

std::string downloadManifest() {
    auto manifest = nlohmann::json::parse(validManifestForCurrentPlatform());
    manifest["version"] = "99.0.0";
    manifest["tag"] = "v99.0.0-beta.900";
    auto& asset = manifest["assets"][0];
    asset["name"] = "test-update.zip";
    asset["size"] = 3;
    asset["sha256"] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"; // abc
    return manifest.dump();
}

void writeResponse(const std::filesystem::path& output, const std::string& text) {
    std::filesystem::create_directories(output.parent_path());
    std::ofstream(output, std::ios::binary) << text;
}

DownloadPolicy quickDownloadPolicy() {
    return {std::chrono::milliseconds(80), std::chrono::milliseconds(500), std::chrono::milliseconds(5)};
}

bool downloadFinished(UpdateService& service) {
    const auto phase = service.status().value("phase", std::string());
    return phase == "error" || phase == "cancelled" || phase == "downloaded" || phase == "staged";
}

class ScheduledUpdateFixture : public DownloadFixture {
public:
    ScheduledUpdateFixture() : timezone_(dice::utils::timezoneOffsetMinutes()) {
#if defined(_WIN32)
        wchar_t value[32768]{};
        if (GetEnvironmentVariableW(L"DICENEXT_MANAGED", value, 32768) > 0) previous_ = value;
        SetEnvironmentVariableW(L"DICENEXT_MANAGED", L"1");
        dice::utils::setTimezoneOffset(480);
        writeResponse(root / "dice-next.exe", "test fixture, never executed");
        writeResponse(root / "app" / "dice-next-core.exe", "test fixture, never executed");
#else
        for (const auto* name : {"DICENEXT_MANAGED", "DICENEXT_MANAGER_PID"}) {
            const char* value = std::getenv(name);
            environment_.push_back({name, value ? std::optional<std::string>(value) : std::nullopt});
        }
        ::setenv("DICENEXT_MANAGED", "1", 1);
        ::setenv("DICENEXT_MANAGER_PID", std::to_string(::getppid()).c_str(), 1);
        for (const auto* file : {"dice-next", "dice-next-server"}) {
            writeResponse(root / file, "test fixture, never executed");
            std::filesystem::permissions(root / file, std::filesystem::perms::owner_all);
        }
        dice::utils::setTimezoneOffset(480);
#endif
        writeResponse(root / "updates" / "pending" / kUpdateHoldFile, "held");
        writeResponse(root / "updates" / "pending" / "update.json", nlohmann::json{
            {"schema", 1}, {"tag", "v99.0.0-beta.900"}, {"version", "99.0.0"}, {"build", 900},
            {"install_plan_enabled", true}
        }.dump());
    }
    ~ScheduledUpdateFixture() {
#if defined(_WIN32)
        SetEnvironmentVariableW(L"DICENEXT_MANAGED", previous_.empty() ? nullptr : previous_.c_str());
#else
        for (const auto& [name, value] : environment_) {
            if (value) ::setenv(name.c_str(), value->c_str(), 1); else ::unsetenv(name.c_str());
        }
#endif
        dice::utils::setTimezoneOffset(timezone_);
    }
    void configure(dice::ConfigManager& config) {
        config.set<bool>("update/auto_check", false);
        config.set<std::string>("update/source", "direct");
        config.set<std::string>("update/auto_action", "install");
        config.set<bool>("update/scheduled_install", true);
        config.set<std::string>("update/install_time", "04:00");
    }
private:
    int timezone_;
#if defined(_WIN32)
    std::wstring previous_;
#else
    std::vector<std::pair<std::string, std::optional<std::string>>> environment_;
#endif
};

}  // namespace

TEST(UpdateSchedule, ValidatesTimeAndUsesNextServerLocalOccurrence) {
    for (const auto* time : {"00:00", "04:00", "23:59"}) ASSERT_TRUE(validInstallTime(time));
    for (const auto* time : {"4:00", "24:00", "04:60", "-1:00", "04:00:00", "０４:００", ""})
        ASSERT_FALSE(validInstallTime(time));
    ASSERT_EQ(nextInstallTime(86400 + 19 * 3600, 480, "04:00"), 86400 + 20 * 3600);
    ASSERT_EQ(nextInstallTime(86400 + 20 * 3600, 480, "04:00"), 2 * 86400 + 20 * 3600);
    ASSERT_EQ(nextInstallTime(86400 + 21 * 3600, 480, "04:00"), 2 * 86400 + 20 * 3600);
    ASSERT_EQ(nextInstallTime(86400 + 7 * 3600, -210, "04:00"), 86400 + 7 * 3600 + 1800);
    ASSERT_EQ(nextInstallTime(86400, 345, "00:00"), 2 * 86400 - 345 * 60);
    ASSERT_EQ(nextInstallTime(-60, 0, "00:00"), 0);
    ASSERT_EQ(nextInstallTime(100, 0, "24:00"), 0);
}

TEST(UpdateSchedule, LauncherGateHoldsNewPackagesButPreservesLegacyBehavior) {
    DownloadFixture fixture;
    const auto stage = fixture.root / "updates" / "pending";
    std::filesystem::create_directories(stage);
    ASSERT_FALSE(pendingUpdateHeld(stage));
    writeResponse(stage / kUpdateHoldFile, "held");
    ASSERT_TRUE(pendingUpdateHeld(stage));
    ASSERT_FALSE(pendingUpdateMayApply(stage));
    writeResponse(stage / kUpdateInstallOnRestartFile, "automatic installation enabled");
    ASSERT_TRUE(pendingUpdateMayApply(stage));
    std::filesystem::remove(stage / kUpdateInstallOnRestartFile);
    ASSERT_FALSE(pendingUpdateMayApply(stage));
    std::filesystem::remove(stage / kUpdateHoldFile);
    ASSERT_FALSE(pendingUpdateHeld(stage));
    ASSERT_TRUE(pendingUpdateMayApply(stage));
}

TEST(UpdateSchedule, DefaultsAreOptInAndInvalidSettingsAreRejected) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    UpdateService service(config, [] {}, {}, ContainerEnvironment{true, "docker", "test"});
    ASSERT_FALSE(service.status()["settings"]["scheduledInstall"].get<bool>());
    ASSERT_EQ(service.status()["settings"]["installTime"].get<std::string>(), "04:00");
    ASSERT_FALSE(service.status()["scheduledInstallSupported"].get<bool>());
    std::string error;
    for (const auto& values : {nlohmann::json{{"installTime", "24:00"}},
                              nlohmann::json{{"installTime", 400}},
                              nlohmann::json{{"scheduledInstall", "true"}},
                              nlohmann::json{{"scheduledInstall", true}}}) {
        ASSERT_FALSE(service.updateSettings(values, error));
        ASSERT_FALSE(error.empty());
    }
    ASSERT_EQ(service.status()["settings"]["installTime"].get<std::string>(), "04:00");
}

TEST(UpdateSchedule, PersistsAcrossRestartAndInstallsOnceAtDeadlineEvenWithoutAutoCheck) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    ASSERT_TRUE(config.save());
    std::atomic<std::int64_t> clock{86400 + 19 * 3600};
    std::atomic<int> restarts{0};
    std::int64_t due = 0;
    {
        UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [&] { return clock.load(); });
        due = service.status()["scheduledInstallAt"].get<std::int64_t>();
        ASSERT_EQ(due, 86400 + 20 * 3600);
        ASSERT_EQ(service.status()["phase"].get<std::string>(), "scheduled");
        service.tick();
        ASSERT_EQ(restarts.load(), 0);
        ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
    }
    clock += 120;
    dice::ConfigManager restored((fixture.root / "config").string());
    ASSERT_TRUE(restored.load());
    UpdateService service(restored, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [&] { return clock.load(); });
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), due);
    clock = due - 1;
    service.tick();
    ASSERT_EQ(restarts.load(), 0);
    clock = due;
    service.tick();
    ASSERT_TRUE(waitUntil([&] { return restarts.load() == 1; }));
    ASSERT_FALSE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
    service.tick();
    ASSERT_EQ(restarts.load(), 1);
}

TEST(UpdateSchedule, CancellingOrChangingStrategyHoldsPackageAndReschedulingUsesNewTime) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<std::int64_t> clock{86400 + 19 * 3600};
    std::atomic<int> restarts{0};
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [&] { return clock.load(); });
    std::string error;
    ASSERT_TRUE(service.updateSettings({{"installTime", "05:15"}}, error));
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 86400 + 21 * 3600 + 15 * 60);
    ASSERT_TRUE(service.updateSettings({{"scheduledInstall", false}}, error));
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 0);
    clock = 3 * 86400;
    service.tick();
    ASSERT_EQ(restarts.load(), 0);
    ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
    ASSERT_EQ(service.status()["phase"].get<std::string>(), "staged");
    ASSERT_TRUE(pendingUpdateMayApply(fixture.root / "updates" / "pending"));
    ASSERT_TRUE(service.updateSettings({{"scheduledInstall", true}}, error));
    ASSERT_TRUE(service.status()["scheduledInstallAt"].get<std::int64_t>() > clock.load());
    ASSERT_TRUE(service.updateSettings({{"autoAction", "notify"}}, error));
    ASSERT_FALSE(pendingUpdateMayApply(fixture.root / "updates" / "pending"));
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 0);
    service.tick();
    ASSERT_EQ(restarts.load(), 0);
}

TEST(UpdateSchedule, TimezoneChangeReschedulesAndManualInstallCanBypassDeadline) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<int> restarts{0};
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [] { return 86400 + 19 * 3600; });
    dice::utils::setTimezoneOffset(0);
    service.tick();
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 2 * 86400 + 4 * 3600);
    std::string error;
    ASSERT_TRUE(service.requestInstall(error));
    ASSERT_FALSE(service.requestInstall(error));
    ASSERT_TRUE(waitUntil([&] { return restarts.load() == 1; }));
    ASSERT_FALSE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
}

TEST(UpdateSchedule, ChecksDoNotRedownloadAnAlreadyScheduledRelease) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<int> downloads{0};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck&) {
        if (url.find("update-manifest.json") != std::string::npos) {
            writeResponse(output, downloadManifest());
            return true;
        }
        ++downloads;
        return false;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, {}, [] { return 86400 + 19 * 3600; });
    const auto due = service.status()["scheduledInstallAt"].get<std::int64_t>();
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["checkedAt"].get<std::int64_t>() > 0; }));
    ASSERT_EQ(downloads.load(), 0);
    ASSERT_EQ(service.status()["phase"].get<std::string>(), "scheduled");
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), due);
}

TEST(UpdateSchedule, CorruptMetadataAndStalePackagesNeverAutoInstall) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<int> restarts{0};
    writeResponse(fixture.root / "updates" / "pending" / "update.json", "not json");
    {
        UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{});
        service.tick();
        ASSERT_EQ(service.status()["phase"].get<std::string>(), "error");
        ASSERT_EQ(restarts.load(), 0);
        ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
    }
    writeResponse(fixture.root / "updates" / "pending" / "update.json", nlohmann::json{
        {"tag", dice::releaseTag()}, {"version", dice::versionString()}, {"build", dice::buildNumber()}
    }.dump());
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{});
    service.tick();
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 0);
    ASSERT_EQ(restarts.load(), 0);
}

TEST(UpdateSchedule, InterruptedOrCancelledPreparationIsNotAutomaticallyArmedOnRestart) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    writeResponse(fixture.root / "updates" / "pending" / "update.json", nlohmann::json{
        {"tag", "v99.0.0-beta.900"}, {"version", "99.0.0"}, {"build", 900},
        {"install_plan_enabled", false}
    }.dump());
    std::atomic<int> restarts{0};
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{});
    service.tick();
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 0);
    ASSERT_EQ(service.status()["phase"].get<std::string>(), "staged");
    ASSERT_EQ(restarts.load(), 0);
    ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
}

TEST(UpdateSchedule, EveryStartupCanApplyScheduledPackageAndChangingStrategyRevokesPermission) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    const auto pending = fixture.root / "updates" / "pending";
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, {}, {}, [] { return 86400 + 19 * 3600; });
    ASSERT_TRUE(service.status()["scheduledInstallAt"].get<std::int64_t>() > 86400 + 19 * 3600);
    ASSERT_TRUE(pendingUpdateHeld(pending));
    ASSERT_TRUE(pendingUpdateMayApply(pending)); // Launcher applies even before the proactive restart time.
    std::string error;
    ASSERT_TRUE(service.updateSettings({{"autoAction", "download"}}, error));
    ASSERT_FALSE(pendingUpdateMayApply(pending));
    ASSERT_TRUE(service.updateSettings({{"autoAction", "install"}}, error));
    ASSERT_TRUE(pendingUpdateMayApply(pending));
    ASSERT_TRUE(service.updateSettings({{"autoAction", "notify"}}, error));
    ASSERT_FALSE(pendingUpdateMayApply(pending));
}

TEST(UpdateSchedule, ExplicitScheduleAlsoHoldsLegacyStagedPackage) {
    ScheduledUpdateFixture fixture;
    std::filesystem::remove(fixture.root / "updates" / "pending" / kUpdateHoldFile);
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<bool>("update/auto_check", false);
    UpdateService service(config, [] {}, {}, ContainerEnvironment{});
    std::string error;
    ASSERT_TRUE(service.updateSettings({{"autoAction", "install"}, {"scheduledInstall", true}}, error));
    ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
    ASSERT_TRUE(service.status()["scheduledInstallAt"].get<std::int64_t>() > 0);
}

TEST(UpdateSchedule, VerifiedManagedDownloadHonorsScheduledImmediateAndDownloadOnlyModes) {
    for (const auto* mode : {"scheduled", "immediate", "download"}) {
        ScheduledUpdateFixture fixture;
        const auto pending = fixture.root / "updates" / "pending";
        std::filesystem::remove(pending / kUpdateHoldFile);
        std::filesystem::remove(pending / "update.json");
        std::filesystem::remove(pending);
        const auto package = fixture.root / "package";
        for (const auto* file : {
#if defined(_WIN32)
                                 "dice-next.exe", "app/dice-next-core.exe", "app/msvcp140.dll",
                                 "app/vcruntime140.dll", "app/vcruntime140_1.dll", "i18n/zh-Hans.json",
#else
                                 "dice-next", "dice-next-server", "start.sh", "i18n/zh-Hans.json",
#endif
                                 "web/dist/index.html", "docs/roadmap.md", "install-on-restart"}) {
            writeResponse(package / file, "test package, never executed");
#ifndef _WIN32
            std::filesystem::permissions(package / file, std::filesystem::perms::owner_all);
#endif
        }
#if defined(_WIN32)
        const auto archive = fixture.root / "fixture.zip";
        const auto packed = dice::proc::run(dice::proc::systemTool("tar.exe"),
            {"-a", "-cf", archive.string(), "package"}, 4096, true, fixture.root);
#else
        const auto archive = fixture.root / "fixture.tar.gz";
        const auto packed = dice::proc::run("/usr/bin/tar", {"-czf", archive.string(), "package"}, 4096, true, fixture.root);
#endif
        ASSERT_TRUE(packed.ok());
        std::ifstream input(archive, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int length = 0;
        ASSERT_EQ(EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr), 1);
        std::ostringstream hex;
        for (unsigned int i = 0; i < length; ++i) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
        auto manifest = nlohmann::json::parse(downloadManifest());
        manifest["assets"][0]["size"] = bytes.size();
        manifest["assets"][0]["sha256"] = hex.str();
        dice::ConfigManager config((fixture.root / "config").string());
        fixture.configure(config);
        const bool scheduled = std::string(mode) == "scheduled";
        const bool immediate = std::string(mode) == "immediate";
        config.set<bool>("update/scheduled_install", scheduled);
        if (!scheduled && !immediate) config.set<std::string>("update/auto_action", "download");
        std::atomic<int> restarts{0};
        auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                         int, std::string&, const UpdateService::CancellationCheck&) {
            writeResponse(output, url.find("update-manifest.json") != std::string::npos ? manifest.dump() : bytes);
            return true;
        };
        UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, fetch, {}, [] { return 86400 + 19 * 3600; });
        std::string error;
        ASSERT_TRUE(service.requestCheck(true, error));
        ASSERT_TRUE(waitUntil([&] {
            const auto phase = service.status()["phase"].get<std::string>();
            return phase == "scheduled" || phase == "staged" || phase == "error" || restarts.load() > 0;
        }, std::chrono::seconds(5)));
        ASSERT_TRUE(service.status()["error"].get<std::string>().empty());
        ASSERT_TRUE(service.status()["pending"].get<bool>());
        ASSERT_EQ(restarts.load(), immediate ? 1 : 0);
        ASSERT_EQ(pendingUpdateHeld(pending), !immediate);
        ASSERT_EQ(pendingUpdateMayApply(pending), scheduled || immediate);
        ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), scheduled ? 86400 + 20 * 3600 : 0);
        if (!immediate) ASSERT_EQ(service.status()["phase"].get<std::string>(), scheduled ? "scheduled" : "staged");
    }
}

TEST(UpdateSchedule, OverduePlanSurvivesRestartWithoutMovingToTomorrow) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<std::int64_t> clock{86400 + 19 * 3600};
    std::atomic<int> restarts{0};
    std::int64_t due = 0;
    {
        UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [&] { return clock.load(); });
        due = service.status()["scheduledInstallAt"].get<std::int64_t>();
    }
    clock = due + 120;
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [&] { return clock.load(); });
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), due);
    service.tick();
    ASSERT_TRUE(waitUntil([&] { return restarts.load() == 1; }));
}

TEST(UpdateSchedule, FailedRestartKeepsPackageHeldAndDoesNotRepeatAutomatically) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<std::int64_t> clock{86400 + 19 * 3600};
    std::atomic<int> restarts{0};
    UpdateService service(config, [&] { ++restarts; throw std::runtime_error("test restart failure"); }, {},
                          ContainerEnvironment{}, {}, {}, [&] { return clock.load(); });
    clock = service.status()["scheduledInstallAt"].get<std::int64_t>();
    service.tick();
    ASSERT_TRUE(waitUntil([&] { return service.status()["phase"].get<std::string>() == "error"; }));
    ASSERT_EQ(restarts.load(), 1);
    ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
    service.tick();
    ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 0);
    ASSERT_EQ(restarts.load(), 1);
}

TEST(UpdateSchedule, ExplicitRestartInstallsEarlyOnlyWhenAutomaticInstallationIsEnabled) {
    for (const auto* action : {"install", "download", "notify"}) {
        ScheduledUpdateFixture fixture;
        dice::ConfigManager config((fixture.root / "config").string());
        fixture.configure(config);
        config.set<std::string>("update/auto_action", action);
        std::atomic<int> restarts{0};
        UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{}, {}, {}, [] { return 86400 + 19 * 3600; });
        const bool install = std::string(action) == "install";
        if (install) ASSERT_TRUE(service.status()["scheduledInstallAt"].get<std::int64_t>() > 86400 + 19 * 3600);
        std::string error;
        ASSERT_TRUE(service.requestManualRestart(error));
        ASSERT_TRUE(waitUntil([&] { return restarts.load() == 1; }));
        ASSERT_EQ(pendingUpdateHeld(fixture.root / "updates" / "pending"), !install);
        ASSERT_EQ(restarts.load(), 1);
        if (install) ASSERT_EQ(service.status()["scheduledInstallAt"].get<std::int64_t>(), 0);
    }
}

TEST(UpdateSchedule, ExplicitRestartInsideContainerIsStillAnOrdinaryRestart) {
    ScheduledUpdateFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    fixture.configure(config);
    std::atomic<int> restarts{0};
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{true, "docker", "test"});
    std::string error;
    ASSERT_TRUE(service.requestManualRestart(error));
    ASSERT_EQ(restarts.load(), 1);
    ASSERT_TRUE(pendingUpdateHeld(fixture.root / "updates" / "pending"));
}

TEST(UpdateSchedule, ExplicitRestartWithoutAReadyPackageIsStillAnOrdinaryRestart) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/auto_action", "install");
    std::atomic<int> restarts{0};
    UpdateService service(config, [&] { ++restarts; }, {}, ContainerEnvironment{});
    std::string error;
    ASSERT_TRUE(service.requestManualRestart(error));
    ASSERT_EQ(restarts.load(), 1);
    ASSERT_FALSE(service.status()["pending"].get<bool>());
}

TEST(UpdateService, CurrentVersionReportsItsActualReleaseChannel) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    UpdateService service(config, [] {}, {}, ContainerEnvironment{});
    const auto status = service.status();
    ASSERT_EQ(status["current"]["prerelease"].get<bool>(), dice::isPrerelease());
    ASSERT_EQ(status["current"]["tag"].get<std::string>(), "v" + dice::versionString() +
        (dice::isPrerelease() ? "-beta." + std::to_string(dice::buildNumber()) : ""));
    ASSERT_TRUE(status["cancelSupported"].get<bool>());
    ASSERT_FALSE(status["canCancel"].get<bool>());
    std::string error;
    ASSERT_FALSE(service.requestCancelDownload(error));
}

TEST(UpdateService, CancelDownloadStopsTransferAndAllowsRetryWithoutRestart) {
    DownloadFixture fixture;
    writeResponse(fixture.root / "updates" / "downloads" / "previous-verified.zip", "abc");
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    std::atomic<int> attempts{0};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> releaseTransfer{false};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck& shouldCancel) {
        if (url.find("update-manifest.json") != std::string::npos) {
            writeResponse(output, downloadManifest());
            return true;
        }
        const int attempt = ++attempts;
        writeResponse(output, "a");
        if (attempt > 1) return false;
        while (!shouldCancel()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cancelled.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!releaseTransfer.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return false;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch);
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["downloadedBytes"].get<int>() == 1; }));
    ASSERT_FALSE(service.requestDownload(error));
    ASSERT_EQ(service.status()["downloadedBytes"].get<int>(), 1);
    ASSERT_TRUE(service.requestCancelDownload(error));
    ASSERT_TRUE(waitUntil([&] { return cancelled.load(); }));
    EXPECT_EQ(service.status()["phase"].get<std::string>(), std::string("cancelling"));
    EXPECT_FALSE(service.requestCheck(true, error));
    EXPECT_FALSE(service.requestDownload(error));
    releaseTransfer.store(true);
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_EQ(service.status()["phase"].get<std::string>(), std::string("cancelled"));
    ASSERT_EQ(service.status()["error"].get<std::string>(), std::string());
    ASSERT_FALSE(service.status()["canCancel"].get<bool>());
    ASSERT_EQ(std::distance(std::filesystem::directory_iterator(fixture.root / "updates" / "downloads"),
        std::filesystem::directory_iterator()), 1);
    ASSERT_TRUE(std::filesystem::is_regular_file(fixture.root / "updates" / "downloads" / "previous-verified.zip"));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_EQ(attempts.load(), 2);
}

TEST(UpdateService, StalledSourceIsCancelledBeforeTryingTheNextMirror) {
    DownloadFixture fixture;
    writeResponse(fixture.root / "update-mirrors.json", R"({"mirrors":["https://backup.example"]})");
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "auto");
    std::atomic<bool> stalledCancelled{false};
    std::atomic<bool> fallbackStarted{false};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck& shouldCancel) {
        const bool mirror = url.rfind("https://backup.example", 0) == 0;
        if (url.find("update-manifest.json") != std::string::npos) {
            if (mirror) return false; // Include failed probe sources as download fallbacks.
            writeResponse(output, downloadManifest());
            return true;
        }
        if (!mirror) {
            writeResponse(output, "a");
            while (!shouldCancel()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            stalledCancelled.store(true);
            return false;
        }
        fallbackStarted.store(stalledCancelled.load());
        writeResponse(output, "bad"); // A reachable mirror must still pass the checksum.
        return true;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, quickDownloadPolicy());
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_TRUE(stalledCancelled.load());
    ASSERT_TRUE(fallbackStarted.load());
    const auto status = service.status();
    ASSERT_EQ(status["phase"].get<std::string>(), std::string("error"));
    ASSERT_TRUE(status["error"].get<std::string>().find("download stalled") != std::string::npos);
    ASSERT_TRUE(status["error"].get<std::string>().find("SHA-256 mismatch") != std::string::npos);
    ASSERT_TRUE(std::filesystem::is_empty(fixture.root / "updates" / "downloads"));
    ASSERT_TRUE(service.requestCheck(true, error));
}

TEST(UpdateService, DownloadUsesChangedSourceAndRecoversAfterAllSourcesTimeout) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    std::atomic<int> customAttempts{0};
    std::atomic<int> directAttempts{0};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck& shouldCancel) {
        if (url.find("update-manifest.json") != std::string::npos) {
            writeResponse(output, downloadManifest());
            return true;
        }
        if (url.rfind("https://custom.example", 0) == 0) ++customAttempts;
        else ++directAttempts;
        while (!shouldCancel()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return false;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, quickDownloadPolicy());
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    ASSERT_TRUE(service.updateSettings({{"source", "custom"}, {"customMirror", "https://custom.example"}}, error));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_EQ(service.status()["phase"].get<std::string>(), std::string("error"));
    ASSERT_EQ(customAttempts.load(), 1);
    ASSERT_EQ(directAttempts.load(), 0);
    ASSERT_TRUE(service.updateSettings({{"source", "direct"}}, error));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_EQ(directAttempts.load(), 1);
}

TEST(UpdateService, ContinuingProgressDoesNotTriggerIdleTimeout) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    std::atomic<bool> unexpectedlyCancelled{false};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck& shouldCancel) {
        if (url.find("update-manifest.json") != std::string::npos) {
            writeResponse(output, downloadManifest());
            return true;
        }
        for (int i = 1; i <= 6; ++i) {
            writeResponse(output, std::string(i, 'a'));
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (shouldCancel()) { unexpectedlyCancelled.store(true); return false; }
        }
        return true;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, quickDownloadPolicy());
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_FALSE(unexpectedlyCancelled.load());
    ASSERT_TRUE(service.status()["error"].get<std::string>().find("SHA-256 mismatch") != std::string::npos);
}

TEST(UpdateService, AttemptDeadlineStopsEvenAContinuouslyProgressingTransfer) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    auto policy = quickDownloadPolicy();
    policy.idleTimeout = std::chrono::seconds(1);
    policy.attemptTimeout = std::chrono::milliseconds(100);
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck& shouldCancel) {
        if (url.find("update-manifest.json") != std::string::npos) {
            writeResponse(output, downloadManifest());
            return true;
        }
        int bytes = 0;
        while (!shouldCancel()) {
            writeResponse(output, std::string(++bytes, 'a'));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, policy);
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_TRUE(service.status()["error"].get<std::string>().find("attempt timed out") != std::string::npos);
    ASSERT_TRUE(std::filesystem::is_empty(fixture.root / "updates" / "downloads"));
}

TEST(UpdateService, TransferExceptionRemovesPartialAndAllowsRetry) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck&) -> bool {
        if (url.find("update-manifest.json") != std::string::npos) {
            writeResponse(output, downloadManifest());
            return true;
        }
        writeResponse(output, "a");
        throw std::runtime_error("simulated transport exception");
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, quickDownloadPolicy());
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    for (int attempt = 0; attempt < 2; ++attempt) {
        ASSERT_TRUE(service.requestDownload(error));
        ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
        ASSERT_TRUE(service.status()["error"].get<std::string>().find("transport exception") != std::string::npos);
        ASSERT_TRUE(std::filesystem::is_empty(fixture.root / "updates" / "downloads"));
    }
}

TEST(UpdateService, ChangedSettingsDuringCheckCannotRestoreAnExcludedSource) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    std::atomic<bool> probeStarted{false}, releaseProbe{false}, usedCustom{false};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck& shouldCancel) {
        if (url.find("update-manifest.json") != std::string::npos) {
            probeStarted.store(true);
            while (!releaseProbe.load() && !shouldCancel()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            writeResponse(output, downloadManifest());
            return true;
        }
        usedCustom.store(url.rfind("https://custom.example", 0) == 0);
        return false;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, quickDownloadPolicy());
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return probeStarted.load(); }));
    ASSERT_TRUE(service.updateSettings({{"source", "custom"}, {"customMirror", "https://custom.example"}}, error));
    releaseProbe.store(true);
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    ASSERT_TRUE(service.requestDownload(error));
    ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
    ASSERT_TRUE(usedCustom.load());
}

#if !defined(_WIN32)
TEST(UpdateService, VerifiedDownloadAndCacheReuseStillWork) {
    DownloadFixture fixture;
    dice::ConfigManager config((fixture.root / "config").string());
    config.set<std::string>("update/source", "direct");
    std::atomic<int> transfers{0};
    auto fetch = [&](const std::string& url, const std::filesystem::path& output, std::uint64_t,
                     int, std::string&, const UpdateService::CancellationCheck&) {
        if (url.find("update-manifest.json") != std::string::npos) writeResponse(output, downloadManifest());
        else { ++transfers; writeResponse(output, "abc"); }
        return true;
    };
    UpdateService service(config, [] {}, {}, ContainerEnvironment{}, fetch, quickDownloadPolicy());
    std::string error;
    ASSERT_TRUE(service.requestCheck(true, error));
    ASSERT_TRUE(waitUntil([&] { return service.status()["updateAvailable"].get<bool>(); }));
    for (int attempt = 0; attempt < 2; ++attempt) {
        ASSERT_TRUE(service.requestDownload(error));
        ASSERT_TRUE(waitUntil([&] { return downloadFinished(service); }));
        ASSERT_EQ(service.status()["phase"].get<std::string>(), std::string("downloaded"));
        ASSERT_FALSE(service.status()["canCancel"].get<bool>());
    }
    ASSERT_EQ(transfers.load(), 1);
    ASSERT_EQ(service.status()["source"].get<std::string>(), std::string("local cache"));
    ASSERT_TRUE(std::filesystem::is_regular_file(fixture.root / "updates" / "downloads" / "test-update.zip"));
}
#endif

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
