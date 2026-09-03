#pragma once

#include "../config/config_manager.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace dice::update {

struct ReleaseAsset {
    std::string os;
    std::string arch;
    std::string name;
    std::string sha256;
    std::uint64_t size = 0;
};

struct ReleaseManifest {
    int schema = 0;
    std::string repository;
    std::string tag;
    std::string version;
    int build = 0;
    bool prerelease = false;
    std::string publishedAt;
    std::string releaseUrl;
    std::vector<ReleaseAsset> assets;
};

bool parseReleaseManifest(const std::string& text, ReleaseManifest& manifest, std::string& error);
int compareRelease(const std::string& leftVersion, int leftBuild,
                   const std::string& rightVersion, int rightBuild);
const ReleaseAsset* selectAsset(const ReleaseManifest& manifest,
                                const std::string& os, const std::string& arch);
bool archiveEntrySafe(const std::string& entry);
std::vector<std::string> missingWindowsPackageComponents(
    const std::filesystem::path& packageRoot);
std::string buildMirroredUrl(const std::string& originalUrl, const std::string& mirror);
std::vector<std::string> githubAssetNameCandidates(const std::string& manifestName);
std::string currentOs();
std::string currentArch();

struct ContainerDetectionInput {
    std::string diceNextMarker;
    std::string dotnetMarker;
    std::string standardMarker;
    std::string systemdMarker;
    std::string kubernetesServiceHost;
    std::string windowsSandboxMount;
    bool dockerEnvFile = false;
    bool containerEnvFile = false;
    std::string cgroup;
    std::string mountInfo;
};

struct ContainerEnvironment {
    bool detected = false;
    std::string type;
    std::string evidence;
};

ContainerEnvironment detectContainerEnvironment(const ContainerDetectionInput& input);
ContainerEnvironment detectContainerEnvironment();

class UpdateService {
public:
    using Json = nlohmann::json;
    using NotifyCallback = std::function<void(const std::string& event, const std::string& message)>;
    using CancellationCheck = std::function<bool()>;
    using FetchCallback = std::function<bool(
        const std::string& url, const std::filesystem::path& output,
        std::uint64_t maxBytes, int timeoutSeconds, std::string& error,
        const CancellationCheck& cancelled)>;

    UpdateService(ConfigManager& config, std::function<void()> restart,
                  NotifyCallback notify = {},
                  ContainerEnvironment container = detectContainerEnvironment(),
                  FetchCallback fetch = {});
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    Json status() const;
    bool updateSettings(const Json& values, std::string& error);
    bool requestCheck(bool force, std::string& error);
    bool requestDownload(std::string& error);
    bool requestInstall(std::string& error);
    void tick();

private:
    enum class Job { none, check, download, install };

    struct Settings {
        bool autoCheck = true;
        int intervalHours = 6;
        std::string action = "notify";
        std::string source = "auto";
        std::string customMirror;
    };

    struct Source {
        std::string prefix;
        std::string label;
        long long latencyMs = 0;
    };

    struct ProbeResult {
        bool ok = false;
        Source source;
        ReleaseManifest manifest;
        std::string error;
    };

    Settings settings() const;
    bool downloadSupported() const;
    bool installSupported() const;
    std::string containerUpdateError() const;
    bool isBusyLocked() const;
    bool queueJobLocked(Job job, const std::string& phase, std::string& error);
    void workerLoop();
    void doCheck(bool force);
    void doDownload();
    void doInstall();
    void processInstallResult();
    void emitNotification(const std::string& event, const std::string& message) const;

    std::vector<Source> configuredSources(const Settings& settings) const;
    ProbeResult probeManifest(const Source& source,
                              const CancellationCheck& cancelled = {}) const;
    std::vector<ProbeResult> raceManifestSources(const std::vector<Source>& sources) const;
    bool downloadAsset(const ReleaseManifest& manifest, const ReleaseAsset& asset,
                       const std::vector<Source>& sources, std::filesystem::path& archive,
                       std::string& usedSource, std::string& error);
    bool prepareWindowsStage(const std::filesystem::path& archive,
                             const ReleaseManifest& manifest, std::string& error);

    static bool fetchToFile(const std::string& url, const std::filesystem::path& output,
                            std::uint64_t maxBytes, int timeoutSeconds, std::string& error,
                            const CancellationCheck& cancelled = {});
    static bool sha256File(const std::filesystem::path& file, std::string& digest,
                           std::string& error,
                           const CancellationCheck& cancelled = {});
    static std::string sourceLabel(const std::string& prefix);

    ConfigManager& config_;
    std::function<void()> restart_;
    NotifyCallback notify_;
    ContainerEnvironment container_;
    FetchCallback fetch_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;

    Job job_ = Job::none;
    bool forceCheck_ = false;
    std::atomic<bool> stopping_{false};

    std::string phase_ = "idle";
    std::string error_;
    std::string activeSource_;
    std::uint64_t downloadedBytes_ = 0;
    std::uint64_t totalBytes_ = 0;
    std::int64_t checkedAt_ = 0;
    bool hasLatest_ = false;
    bool updateAvailable_ = false;
    bool automaticDownload_ = false;
    bool installResultProcessed_ = false;
    ReleaseManifest latest_;
    std::vector<Source> sourceOrder_;
    std::int64_t sourceCacheUntil_ = 0;
    std::thread worker_;
};

}  // namespace dice::update
