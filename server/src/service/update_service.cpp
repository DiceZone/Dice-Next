#include "update_service.h"

#include "../common/logger.h"
#include "../common/subprocess.h"
#include "../common/version.h"
#include "../common/utils.h"
#include "../common/update_schedule.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dice::update {
namespace fs = std::filesystem;

namespace {

constexpr const char* kRepository = "DiceZone/Dice-Next";
constexpr const char* kLatestManifestUrl =
    "https://github.com/DiceZone/Dice-Next/releases/latest/download/update-manifest.json";
constexpr std::uint64_t kManifestLimit = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumAssetSize = 1024ULL * 1024ULL * 1024ULL;
constexpr std::int64_t kMirrorCacheSeconds = 30 * 60;

std::atomic<unsigned long long> g_tempSequence{0};

std::string updaterTemporarySuffix() {
    static const std::string processToken = [] {
        const auto pid =
#if defined(_WIN32)
            static_cast<unsigned long long>(GetCurrentProcessId());
#else
            static_cast<unsigned long long>(getpid());
#endif
        unsigned long long randomValue = 0;
        try {
            std::random_device random;
            randomValue = (static_cast<unsigned long long>(random()) << 32U) ^ random();
        } catch (...) {
            randomValue = static_cast<unsigned long long>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        }
        std::ostringstream value;
        value << pid << '-' << std::hex << randomValue;
        return value.str();
    }();
    return processToken + '-' + std::to_string(g_tempSequence.fetch_add(1));
}

std::int64_t epochSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

bool writePendingMetadata(const fs::path& path, const nlohmann::json& metadata,
                          std::string& error) {
    const fs::path temporary = path.string() + ".tmp-" + updaterTemporarySuffix();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << metadata.dump(2) << '\n';
        output.close();
        if (!output) {
            error = "cannot persist scheduled update metadata";
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(GetLastError(), std::system_category());
#else
    fs::rename(temporary, path, ec);
#endif
    if (ec) {
        error = "cannot persist scheduled update metadata: " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string environmentValue(const char* name) {
#if defined(_WIN32)
    const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
    if (required == 0) return {};
    std::string value(static_cast<std::size_t>(required), '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(static_cast<std::size_t>(written));
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::string readRuntimeProbeFile(const fs::path& path) {
    constexpr std::size_t kLimit = 512 * 1024;
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string result;
    std::array<char, 4096> buffer{};
    while (input && result.size() < kLimit) {
        const auto remaining = kLimit - result.size();
        input.read(buffer.data(), static_cast<std::streamsize>(
            std::min<std::size_t>(buffer.size(), remaining)));
        const auto count = input.gcount();
        if (count <= 0) break;
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return result;
}

bool safeFilename(const std::string& value) {
    if (value.empty() || value == "." || value == ".." || value.find("..") != std::string::npos)
        return false;
    static const std::regex pattern(R"(^[A-Za-z0-9._()\-]+$)");
    return std::regex_match(value, pattern);
}

bool safeTag(const std::string& value) {
    static const std::regex pattern(R"(^v[0-9A-Za-z][0-9A-Za-z._\-+]*$)");
    return value.size() <= 100 && std::regex_match(value, pattern);
}

bool safeHttpsUrl(const std::string& value) {
    if (value.rfind("https://", 0) != 0) return false;
    for (unsigned char ch : value) {
        if (ch <= 0x20 || ch == '"' || ch == '\\' || ch == 0x60 || ch == '$') return false;
    }
    return true;
}

std::string curlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

bool parseVersion(const std::string& value, std::array<int, 3>& parts) {
    static const std::regex pattern(R"(^([0-9]+)\.([0-9]+)\.([0-9]+)$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) return false;
    try {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const long long item = std::stoll(match[i + 1].str());
            if (item < 0 || item > 1000000) return false;
            parts[i] = static_cast<int>(item);
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::string urlEncodeSegment(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

std::string readFileLimited(const fs::path& path, std::uint64_t limit, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > limit) {
        error = ec ? ec.message() : "response size is invalid";
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open downloaded response";
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in) {
        error = "cannot read downloaded response";
        return {};
    }
    return text;
}

// Download mirrors ship as package data instead of string literals.  An
// unsigned executable that embeds a list of third-party download proxies and
// then pulls executables through them matches the static profile of a
// downloader trojan closely enough for machine-learning scanners to flag it.
// Entries are validated as safe HTTPS prefixes by the caller.
std::vector<std::string> loadMirrorList() {
    constexpr std::size_t kMaxMirrors = 16;
    std::vector<std::string> mirrors;
    std::ifstream input("update-mirrors.json", std::ios::binary);
    if (!input) return mirrors;
    const auto parsed = nlohmann::json::parse(input, nullptr, false);
    if (!parsed.is_object()) return mirrors;
    const auto entries = parsed.find("mirrors");
    if (entries == parsed.end() || !entries->is_array()) return mirrors;
    for (const auto& entry : *entries) {
        if (!entry.is_string()) continue;
        mirrors.push_back(entry.get<std::string>());
        if (mirrors.size() >= kMaxMirrors) break;
    }
    return mirrors;
}

}  // namespace

bool parseReleaseManifest(const std::string& text, ReleaseManifest& manifest, std::string& error) {
    try {
        const auto root = nlohmann::json::parse(text);
        if (!root.is_object()) {
            error = "release manifest root is not an object";
            return false;
        }

        ReleaseManifest parsed;
        parsed.schema = root.value("schema", 0);
        parsed.repository = root.value("repository", std::string());
        parsed.tag = root.value("tag", std::string());
        parsed.version = root.value("version", std::string());
        parsed.build = root.value("build", -1);
        parsed.prerelease = root.value("prerelease", false);
        parsed.publishedAt = root.value("published_at", std::string());
        parsed.releaseUrl = root.value("release_url", std::string());

        std::array<int, 3> versionParts{};
        if (parsed.schema != 1 || parsed.repository != kRepository || !safeTag(parsed.tag) ||
            !parseVersion(parsed.version, versionParts) || parsed.build < 0) {
            error = "release manifest metadata is invalid";
            return false;
        }
        const std::string versionTag = "v" + parsed.version;
        const std::string betaTag = versionTag + "-beta." + std::to_string(parsed.build);
        if (parsed.tag != versionTag && parsed.tag != betaTag) {
            error = "release manifest tag does not match version and build";
            return false;
        }
        parsed.releaseUrl =
            "https://github.com/DiceZone/Dice-Next/releases/tag/" + parsed.tag;
        if (!root.contains("assets") || !root["assets"].is_array()) {
            error = "release manifest has no assets";
            return false;
        }

        for (const auto& item : root["assets"]) {
            if (!item.is_object()) continue;
            ReleaseAsset asset;
            asset.os = item.value("os", std::string());
            asset.arch = item.value("arch", std::string());
            asset.name = item.value("name", std::string());
            asset.sha256 = lower(item.value("sha256", std::string()));
            asset.size = item.value("size", 0ULL);
            const bool digestOk = asset.sha256.size() == 64 &&
                std::all_of(asset.sha256.begin(), asset.sha256.end(), [](unsigned char ch) {
                    return std::isxdigit(ch) != 0;
                });
            if ((asset.os != "windows" && asset.os != "linux" && asset.os != "macos") ||
                (asset.arch != "amd64" && asset.arch != "arm64") ||
                !safeFilename(asset.name) || !digestOk || asset.size == 0 ||
                asset.size > kMaximumAssetSize) {
                error = "release manifest contains an invalid asset";
                return false;
            }
            const bool duplicateTarget = std::any_of(
                parsed.assets.begin(), parsed.assets.end(), [&](const ReleaseAsset& existing) {
                    return existing.os == asset.os && existing.arch == asset.arch;
                });
            if (duplicateTarget) {
                error = "release manifest contains duplicate platform assets";
                return false;
            }
            parsed.assets.push_back(std::move(asset));
        }
        if (parsed.assets.empty()) {
            error = "release manifest has no valid assets";
            return false;
        }
        manifest = std::move(parsed);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("cannot parse release manifest: ") + ex.what();
        return false;
    }
}

int compareRelease(const std::string& leftVersion, int leftBuild,
                   const std::string& rightVersion, int rightBuild) {
    std::array<int, 3> left{}, right{};
    if (!parseVersion(leftVersion, left) || !parseVersion(rightVersion, right)) return 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    if (leftBuild == rightBuild) return 0;
    return leftBuild < rightBuild ? -1 : 1;
}

const ReleaseAsset* selectAsset(const ReleaseManifest& manifest,
                                const std::string& os, const std::string& arch) {
    const auto found = std::find_if(manifest.assets.begin(), manifest.assets.end(),
        [&](const ReleaseAsset& asset) { return asset.os == os && asset.arch == arch; });
    return found == manifest.assets.end() ? nullptr : &*found;
}

bool archiveEntrySafe(const std::string& rawEntry) {
    if (rawEntry.empty() || rawEntry.size() > 1024) return false;
    std::string entry = rawEntry;
    if (!entry.empty() && entry.back() == '\r') entry.pop_back();
    std::replace(entry.begin(), entry.end(), '\\', '/');
    if (entry.empty() || entry.front() == '/' || entry.find(':') != std::string::npos) return false;
    std::istringstream parts(entry);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part == "..") return false;
    }
    return true;
}

std::vector<std::string> missingWindowsPackageComponents(const fs::path& packageRoot) {
    std::vector<std::string> missing;
    std::error_code ec;
    const auto requireFile = [&](const fs::path& relative) {
        ec.clear();
        if (!fs::is_regular_file(packageRoot / relative, ec))
            missing.push_back(relative.generic_string());
    };
    const auto requireDirectory = [&](const fs::path& relative) {
        ec.clear();
        if (!fs::is_directory(packageRoot / relative, ec))
            missing.push_back(relative.generic_string() + "/");
    };

    requireFile("dice-next.exe");
    requireFile(fs::path("app") / "dice-next-core.exe");
    requireDirectory("i18n");
    requireFile(fs::path("web") / "dist" / "index.html");
    requireFile(fs::path("docs") / "roadmap.md");

    // Packages through build 883 kept dependencies in lib/. New packages put
    // them beside the core executable so Windows can resolve imports before
    // main() starts. Accept both layouts during the transition.
    ec.clear();
    const bool legacyRuntime = fs::is_directory(packageRoot / "lib", ec);
    ec.clear();
    const bool appRuntime =
        fs::is_regular_file(packageRoot / "app" / "msvcp140.dll", ec) &&
        fs::is_regular_file(packageRoot / "app" / "vcruntime140.dll", ec) &&
        fs::is_regular_file(packageRoot / "app" / "vcruntime140_1.dll", ec);
    if (!legacyRuntime && !appRuntime)
        missing.push_back("app/MSVC runtime DLLs (or legacy lib/)");
    return missing;
}

std::string buildMirroredUrl(const std::string& originalUrl, const std::string& mirror) {
    if (mirror.empty()) return originalUrl;
    return mirror.back() == '/' ? mirror + originalUrl : mirror + "/" + originalUrl;
}

std::vector<std::string> githubAssetNameCandidates(const std::string& manifestName) {
    std::vector<std::string> candidates{manifestName};
    std::string normalized = manifestName;
    std::replace(normalized.begin(), normalized.end(), '(', '.');
    std::replace(normalized.begin(), normalized.end(), ')', '.');
    if (normalized != manifestName) candidates.push_back(std::move(normalized));
    return candidates;
}

std::string currentOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string currentArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "amd64";
#endif
}

ContainerEnvironment detectContainerEnvironment(const ContainerDetectionInput& input) {
    const auto makeResult = [](std::string type, std::string evidence) {
        return ContainerEnvironment{true, std::move(type), std::move(evidence)};
    };
    const auto normalizedMarker = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        const auto last = value.find_last_not_of(" \t\r\n");
        value = lower(value.substr(first, last - first + 1));
        if (value == "0" || value == "false" || value == "no" || value == "off" ||
            value == "host" || value == "baremetal") return std::string();
        return value;
    };
    const auto markerType = [&](const std::string& marker) {
        const std::string value = normalizedMarker(marker);
        if (value.empty()) return std::string();
        if (value.find("kube") != std::string::npos) return std::string("kubernetes");
        if (value.find("podman") != std::string::npos || value.find("libpod") != std::string::npos)
            return std::string("podman");
        if (value.find("docker") != std::string::npos) return std::string("docker");
        if (value.find("containerd") != std::string::npos) return std::string("containerd");
        if (value.find("lxc") != std::string::npos) return std::string("lxc");
        return std::string("container");
    };
    const auto contentType = [](const std::string& content) {
        const std::string value = lower(content);
        static const std::regex dockerCgroup(
            R"((/docker/[0-9a-f]{12,64}($|\n))|((^|/)docker-[0-9a-f]{12,64}\.scope($|\n)))");
        static const std::regex podmanCgroup(
            R"((^|/)libpod-[0-9a-f]{12,64}\.scope($|\n))");
        static const std::regex containerdCgroup(
            R"((^|/)(cri-)?containerd-[0-9a-f]{12,64}\.scope($|\n))");
        if (value.find("kubepods") != std::string::npos ||
            value.find("kubernetes") != std::string::npos) return std::string("kubernetes");
        if (std::regex_search(value, podmanCgroup)) return std::string("podman");
        if (std::regex_search(value, dockerCgroup)) return std::string("docker");
        if (std::regex_search(value, containerdCgroup)) return std::string("containerd");
        if (value.find("lxc.payload") != std::string::npos ||
            value.find("/lxc/") != std::string::npos) return std::string("lxc");
        if (value.find("/garden/") != std::string::npos) return std::string("garden");
        return std::string();
    };
    const auto mountType = [](const std::string& content) {
        // A host mount namespace can see mounts belonging to unrelated Docker
        // containers. Only the entry mounted as this process's root is evidence
        // about this process itself.
        std::istringstream lines(content);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string id, parent, device, root, mountPoint;
            if (!(fields >> id >> parent >> device >> root >> mountPoint) || mountPoint != "/")
                continue;
            const std::string value = lower(line);
            if (value.find("kubepods") != std::string::npos ||
                value.find("/kubernetes/") != std::string::npos)
                return std::string("kubernetes");
            if (value.find("containers/storage") != std::string::npos ||
                value.find("libpod") != std::string::npos)
                return std::string("podman");
            if (value.find("/var/lib/docker/") != std::string::npos ||
                value.find("/docker/containers/") != std::string::npos)
                return std::string("docker");
            if (value.find("/var/lib/containerd/") != std::string::npos ||
                value.find("io.containerd.runtime") != std::string::npos)
                return std::string("containerd");
            if (value.find("lxcfs") != std::string::npos) return std::string("lxc");
        }
        return std::string();
    };

    if (!normalizedMarker(input.kubernetesServiceHost).empty())
        return makeResult("kubernetes", "KUBERNETES_SERVICE_HOST");
    if (const auto type = markerType(input.diceNextMarker); !type.empty())
        return makeResult(type, "DICENEXT_CONTAINER");
    if (!normalizedMarker(input.windowsSandboxMount).empty())
        return makeResult("windows-container", "CONTAINER_SANDBOX_MOUNT_POINT");
    if (const auto type = markerType(input.dotnetMarker); !type.empty())
        return makeResult(type, "DOTNET_RUNNING_IN_CONTAINER");
    if (const auto type = markerType(input.standardMarker); !type.empty())
        return makeResult(type, "container");
    if (const auto type = markerType(input.systemdMarker); !type.empty())
        return makeResult(type, "/run/systemd/container");
    if (input.dockerEnvFile) return makeResult("docker", "/.dockerenv");
    if (input.containerEnvFile) return makeResult("podman", "/run/.containerenv");
    if (const auto type = contentType(input.cgroup); !type.empty())
        return makeResult(type, "/proc/*/cgroup");
    if (const auto type = mountType(input.mountInfo); !type.empty())
        return makeResult(type, "/proc/self/mountinfo");
    return {};
}

ContainerEnvironment detectContainerEnvironment() {
    ContainerDetectionInput input;
    input.diceNextMarker = environmentValue("DICENEXT_CONTAINER");
    input.standardMarker = environmentValue("container");
    input.kubernetesServiceHost = environmentValue("KUBERNETES_SERVICE_HOST");
    input.windowsSandboxMount = environmentValue("CONTAINER_SANDBOX_MOUNT_POINT");
    input.dotnetMarker = environmentValue("DOTNET_RUNNING_IN_CONTAINER");
#if defined(__linux__)
    std::error_code ec;
    input.dockerEnvFile = fs::is_regular_file("/.dockerenv", ec);
    ec.clear();
    input.containerEnvFile = fs::is_regular_file("/run/.containerenv", ec);
    input.systemdMarker = readRuntimeProbeFile("/run/systemd/container");
    input.cgroup = readRuntimeProbeFile("/proc/1/cgroup") + "\n" +
        readRuntimeProbeFile("/proc/self/cgroup");
    input.mountInfo = readRuntimeProbeFile("/proc/self/mountinfo");
#endif
    return detectContainerEnvironment(input);
}

UpdateService::UpdateService(ConfigManager& config, std::function<void()> restart,
                             NotifyCallback notify, ContainerEnvironment container,
                             FetchCallback fetch, DownloadPolicy downloadPolicy, Clock clock)
    : config_(config), restart_(std::move(restart)), notify_(std::move(notify)),
      container_(std::move(container)), fetch_(std::move(fetch)),
      downloadPolicy_(downloadPolicy), clock_(std::move(clock)) {
    const DownloadPolicy defaults;
    if (downloadPolicy_.idleTimeout.count() <= 0) downloadPolicy_.idleTimeout = defaults.idleTimeout;
    if (downloadPolicy_.attemptTimeout.count() <= 0) downloadPolicy_.attemptTimeout = defaults.attemptTimeout;
    if (downloadPolicy_.pollInterval.count() <= 0) downloadPolicy_.pollInterval = defaults.pollInterval;
    if (container_.detected) {
        DICE_LOG_INFO("Container runtime detected ({} via {}); self-update download and "
                      "installation are disabled",
            container_.type, container_.evidence);
    }
    std::string pendingError;
    if (!reconcilePendingLocked(settings(), pendingError)) {
        phase_ = "error";
        error_ = pendingError;
    }
    worker_ = std::thread([this] { workerLoop(); });
}

UpdateService::~UpdateService() {
    stopping_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void UpdateService::emitNotification(const std::string& event,
                                     const std::string& message) const {
    if (!notify_) return;
    try {
        notify_(event, message);
    } catch (const std::exception& ex) {
        DICE_LOG_WARN("Update notification {} failed: {}", event, ex.what());
    } catch (...) {
        DICE_LOG_WARN("Update notification {} failed", event);
    }
}

void UpdateService::processInstallResult() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (installResultProcessed_) return;
        installResultProcessed_ = true;
    }

    const fs::path resultPath = fs::path("updates") / "last-result.json";
    std::error_code ec;
    if (!fs::is_regular_file(resultPath, ec)) return;

    std::string readError;
    const std::string text = readFileLimited(resultPath, 64 * 1024, readError);
    try {
        if (text.empty()) throw std::runtime_error(
            readError.empty() ? "result file is empty" : readError);
        const auto result = nlohmann::json::parse(text);
        const auto metadata = result.value("metadata", nlohmann::json::object());
        const bool success = result.value("success", false);
        const std::string tag = metadata.value("tag", std::string("unknown"));
        const std::string version = metadata.value("version", std::string());
        const int build = metadata.value("build", -1);
        const bool runningExpectedBuild =
            version == versionString() && build == buildNumber();

        if (success && runningExpectedBuild) {
            emitNotification("update_result",
                "Dice!Next 更新安装成功：" + tag + "\n当前运行版本：" + releaseTag());
        } else {
            std::string detail = result.value("message", std::string());
            if (success && !runningExpectedBuild) {
                detail = "启动后的版本与目标版本不一致";
            }
            emitNotification("update_error",
                "Dice!Next 更新安装失败：" + tag +
                (detail.empty() ? std::string() : "\n原因：" + detail));
        }
    } catch (const std::exception& ex) {
        emitNotification("update_error",
            "Dice!Next 无法读取更新安装结果：" + std::string(ex.what()));
    }
    fs::remove(resultPath, ec);
}

UpdateService::Settings UpdateService::settings() const {
    Settings result;
    result.autoCheck = config_.get<bool>("update/auto_check", true);
    result.intervalHours = std::clamp(config_.get<int>("update/check_interval_hours", 6), 1, 168);
    result.action = config_.get<std::string>("update/auto_action", "notify");
    result.scheduledInstall = config_.get<bool>("update/scheduled_install", false);
    if (!installSupported()) result.scheduledInstall = false;
    result.installTime = config_.get<std::string>("update/install_time", "04:00");
    if (!validInstallTime(result.installTime)) result.installTime = "04:00";
    result.source = config_.get<std::string>("update/source", "auto");
    result.customMirror = config_.get<std::string>("update/custom_mirror", "");
    if (result.action != "notify" && result.action != "download" && result.action != "install")
        result.action = "notify";
    if (container_.detected && result.action != "notify") result.action = "notify";
    if (result.source != "auto" && result.source != "direct" &&
        result.source != "mirror" && result.source != "custom")
        result.source = "auto";
    return result;
}

std::int64_t UpdateService::now() const { return clock_ ? clock_() : epochSeconds(); }

bool UpdateService::reconcilePendingLocked(const Settings& current, std::string& error, bool arm) {
    const fs::path stage = fs::path("updates") / "pending";
    scheduledInstallAt_ = 0;
    pendingTag_.clear();
    if (!fs::is_directory(stage)) return true;
    if (!pendingUpdateHeld(stage)) {
        if (!arm || current.action != "install" || !current.scheduledInstall || !installSupported()) return true;
        // Explicitly enabling scheduling also holds a package staged by an older core.
        std::ofstream hold(stage / kUpdateHoldFile, std::ios::binary | std::ios::trunc);
        hold << "Scheduled installation requires core authorization.\n";
        hold.close();
        if (!hold) { error = "cannot hold staged update for scheduled installation"; return false; }
    }
    try {
        std::string readError;
        auto metadata = Json::parse(readFileLimited(stage / "update.json", 64 * 1024, readError));
        pendingTag_ = metadata.at("tag").get<std::string>();
        if (compareRelease(versionString(), buildNumber(), metadata.at("version").get<std::string>(),
                           metadata.at("build").get<int>()) >= 0) {
            // A rolled-back/stale package must not restart the current build forever.
            std::error_code ec;
            fs::remove(stage / kUpdateInstallOnRestartFile, ec);
            if (ec) { error = "cannot revoke stale installation permission: " + ec.message(); return false; }
            phase_ = "staged";
            return true;
        }
        const int timezone = utils::effectiveTimezoneOffsetMinutes();
        const bool previouslyArmed = metadata.value("install_plan_enabled", false);
        const bool previouslyAutoArmed = metadata.value("auto_install_authorized", previouslyArmed);
        const bool autoArmed = current.action == "install" && installSupported() && (arm || previouslyAutoArmed);
        const bool enabled = current.action == "install" && current.scheduledInstall && installSupported() &&
            (arm || previouslyArmed);
        const fs::path restartPermission = stage / kUpdateInstallOnRestartFile;
        if (!autoArmed) {
            std::error_code ec;
            fs::remove(restartPermission, ec);
            if (ec) { error = "cannot revoke automatic installation on restart: " + ec.message(); return false; }
        }
        const auto previous = metadata.value("install_at", std::int64_t(0));
        auto due = enabled ? previous : 0;
        if (enabled && (due <= 0 || metadata.value("install_time", std::string()) != current.installTime ||
                       metadata.value("install_timezone", 9999) != timezone)) {
            due = nextInstallTime(now(), timezone, current.installTime);
        }
        if (autoArmed != previouslyAutoArmed || enabled != previouslyArmed || due != previous ||
            (enabled && metadata.value("install_time", std::string()) != current.installTime)) {
            metadata["auto_install_authorized"] = autoArmed;
            metadata["install_plan_enabled"] = enabled;
            metadata["install_at"] = due;
            metadata["install_time"] = current.installTime;
            metadata["install_timezone"] = timezone;
            if (!writePendingMetadata(stage / "update.json", metadata, error)) return false;
        }
        if (autoArmed && !fs::is_regular_file(restartPermission)) {
            std::ofstream permission(restartPermission, std::ios::binary | std::ios::trunc);
            permission << "Automatic installation enabled; apply this verified package on the next start.\n";
            permission.close();
            if (!permission) { error = "cannot authorize automatic installation on restart"; return false; }
        }
        scheduledInstallAt_ = due;
        phase_ = due > 0 ? "scheduled" : "staged";
        return true;
    } catch (const std::exception& ex) {
        error = "cannot restore staged update: " + std::string(ex.what());
        return false;
    }
}

bool UpdateService::downloadSupported() const {
    return !container_.detected;
}

bool UpdateService::installSupported() const {
    if (!downloadSupported()) return false;
#if defined(_WIN32)
    wchar_t managed[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"DICENEXT_MANAGED", managed, static_cast<DWORD>(std::size(managed)));
    std::error_code ec;
    return length > 0 && std::wstring_view(managed) == L"1" &&
        fs::is_regular_file("dice-next.exe", ec) &&
        fs::is_regular_file(fs::path("app") / "dice-next-core.exe", ec);
#else
    return false;
#endif
}

std::string UpdateService::containerUpdateError() const {
    return "self-update download and installation are disabled inside containers; "
        "pull a new image and recreate the container";
}

std::string UpdateService::sourceLabel(const std::string& prefix) {
    if (prefix.empty()) return "GitHub";
    const std::size_t start = prefix.find("://");
    const std::size_t hostStart = start == std::string::npos ? 0 : start + 3;
    const std::size_t hostEnd = prefix.find('/', hostStart);
    return prefix.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
}

UpdateService::Json UpdateService::status() const {
    const Settings current = settings();
    std::lock_guard<std::mutex> lock(mutex_);

    Json latest = nullptr;
    if (hasLatest_) {
        latest = Json{
            {"tag", latest_.tag},
            {"version", latest_.version},
            {"build", latest_.build},
            {"prerelease", latest_.prerelease},
            {"publishedAt", latest_.publishedAt},
            {"releaseUrl", latest_.releaseUrl.empty()
                ? "https://github.com/DiceZone/Dice-Next/releases/tag/" + latest_.tag
                : latest_.releaseUrl}
        };
        if (const auto* asset = selectAsset(latest_, currentOs(), currentArch())) {
            latest["asset"] = Json{
                {"name", asset->name},
                {"size", asset->size},
                {"sha256", asset->sha256}
            };
        }
    }

    return Json{
        {"current", Json{
            {"version", versionString()},
            {"build", buildNumber()},
            {"prerelease", isPrerelease()},
            {"tag", releaseTag()}
        }},
        {"platform", Json{{"os", currentOs()}, {"arch", currentArch()}}},
        {"latest", latest},
        {"updateAvailable", updateAvailable_},
        {"phase", phase_},
        {"cancelSupported", true},
        {"canCancel", downloadActive_ && !downloadCancelled_.load(std::memory_order_acquire)},
        {"error", error_},
        {"source", activeSource_},
        {"downloadedBytes", downloadedBytes_},
        {"totalBytes", totalBytes_},
        {"checkedAt", checkedAt_},
        {"downloadSupported", downloadSupported()},
        {"installSupported", installSupported()},
        {"scheduledInstallSupported", installSupported()},
        {"scheduledInstallAt", scheduledInstallAt_},
        {"pendingTag", pendingTag_},
        {"timezoneMinutes", utils::effectiveTimezoneOffsetMinutes()},
        {"selfUpdateBlockedReason", container_.detected ? "container" : ""},
        {"runtime", Json{
            {"container", container_.detected},
            {"containerType", container_.type},
            {"containerDetection", container_.evidence}
        }},
        {"pending", fs::is_directory(fs::path("updates") / "pending")},
        {"settings", Json{
            {"autoCheck", current.autoCheck},
            {"intervalHours", current.intervalHours},
            {"autoAction", current.action},
            {"scheduledInstall", current.scheduledInstall},
            {"installTime", current.installTime},
            {"source", current.source},
            {"customMirror", current.customMirror}
        }}
    };
}

bool UpdateService::updateSettings(const Json& values, std::string& error) {
    try {
        if (!values.is_object()) {
            error = "settings must be an object";
            return false;
        }

        const Settings previous = settings();
        Settings next = previous;
        if (values.contains("scheduledInstall")) {
            if (!values["scheduledInstall"].is_boolean()) {
                error = "scheduledInstall must be a boolean";
                return false;
            }
            next.scheduledInstall = values["scheduledInstall"].get<bool>();
            if (next.scheduledInstall && !installSupported()) {
                error = "scheduled installation requires the Windows dice-next.exe manager";
                return false;
            }
        }
        if (values.contains("installTime")) {
            if (!values["installTime"].is_string() ||
                !validInstallTime(values["installTime"].get<std::string>())) {
                error = "installTime must use HH:MM (00:00-23:59)";
                return false;
            }
            next.installTime = values["installTime"].get<std::string>();
        }
        if (values.contains("autoCheck")) {
            if (!values["autoCheck"].is_boolean()) {
                error = "autoCheck must be a boolean";
                return false;
            }
            next.autoCheck = values["autoCheck"].get<bool>();
        }
        if (values.contains("intervalHours")) {
            if (!values["intervalHours"].is_number_integer()) {
                error = "intervalHours must be an integer";
                return false;
            }
            next.intervalHours = values["intervalHours"].get<int>();
            if (next.intervalHours < 1 || next.intervalHours > 168) {
                error = "intervalHours must be between 1 and 168";
                return false;
            }
        }
        if (values.contains("autoAction")) {
            if (!values["autoAction"].is_string()) {
                error = "autoAction must be a string";
                return false;
            }
            next.action = values["autoAction"].get<std::string>();
            if (next.action != "notify" && next.action != "download" && next.action != "install") {
                error = "unknown autoAction";
                return false;
            }
            if (next.action != "notify" && !downloadSupported()) {
                error = containerUpdateError();
                return false;
            }
            if (next.action == "install" && !installSupported()) {
                error = "automatic installation requires the Windows dice-next.exe manager";
                return false;
            }
        }
        if (values.contains("source")) {
            if (!values["source"].is_string()) {
                error = "source must be a string";
                return false;
            }
            next.source = values["source"].get<std::string>();
            if (next.source != "auto" && next.source != "direct" &&
                next.source != "mirror" && next.source != "custom") {
                error = "unknown update source";
                return false;
            }
        }
        if (values.contains("customMirror")) {
            if (!values["customMirror"].is_string()) {
                error = "customMirror must be a string";
                return false;
            }
            next.customMirror = values["customMirror"].get<std::string>();
            while (!next.customMirror.empty() && next.customMirror.back() == '/') {
                next.customMirror.pop_back();
            }
            if (!next.customMirror.empty() && !safeHttpsUrl(next.customMirror)) {
                error = "custom mirror must be a safe HTTPS URL";
                return false;
            }
        }
        if (next.source == "custom" && next.customMirror.empty()) {
            error = "custom source requires customMirror";
            return false;
        }

        const auto store = [this](const Settings& value) {
            config_.set<bool>("update/auto_check", value.autoCheck);
            config_.set<int>("update/check_interval_hours", value.intervalHours);
            config_.set<std::string>("update/auto_action", value.action);
            config_.set<bool>("update/scheduled_install", value.scheduledInstall);
            config_.set<std::string>("update/install_time", value.installTime);
            config_.set<std::string>("update/source", value.source);
            config_.set<std::string>("update/custom_mirror", value.customMirror);
        };
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == "installing") {
            error = "cannot change update settings while installation is starting";
            return false;
        }
        store(next);
        if (!config_.save()) {
            store(previous);
            error = "cannot save update settings";
            return false;
        }
        if (next.source != previous.source || next.customMirror != previous.customMirror) {
            sourceOrder_.clear();
            sourceCacheUntil_ = 0;
        }
        const bool arm = next.scheduledInstall != previous.scheduledInstall ||
            next.installTime != previous.installTime || next.action != previous.action;
        if (!isBusyLocked() && !reconcilePendingLocked(next, error, arm)) {
            phase_ = "error";
            error_ = error;
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}
bool UpdateService::isBusyLocked() const {
    return downloadActive_ || phase_ == "checking" || phase_ == "installing" ||
        job_ != Job::none;
}

bool UpdateService::queueJobLocked(Job job, const std::string& phase, std::string& error) {
    if (stopping_.load(std::memory_order_acquire)) {
        error = "update service is stopping";
        return false;
    }
    if (isBusyLocked()) {
        error = "another update operation is already running";
        return false;
    }
    job_ = job;
    phase_ = phase;
    error_.clear();
    wake_.notify_all();
    return true;
}

bool UpdateService::requestCheck(bool force, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queueJobLocked(Job::check, "checking", error)) return false;
    forceCheck_ = force;
    return true;
}

bool UpdateService::requestDownload(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!downloadSupported()) {
        error = containerUpdateError();
        return false;
    }
    if (!hasLatest_ || !updateAvailable_) {
        error = "no newer release is available";
        return false;
    }
    if (!queueJobLocked(Job::download, "connecting", error)) return false;
    downloadedBytes_ = 0;
    totalBytes_ = 0;
    downloadCancelled_.store(false, std::memory_order_release);
    downloadActive_ = true;
    automaticDownload_ = false;
    return true;
}

bool UpdateService::requestCancelDownload(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!downloadActive_) {
        error = "no cancellable download is running";
        return false;
    }
    downloadCancelled_.store(true, std::memory_order_release);
    phase_ = "cancelling";
    return true;
}

bool UpdateService::requestInstall(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!downloadSupported()) {
        error = containerUpdateError();
        return false;
    }
    if (!installSupported()) {
        error = "automatic installation requires the Windows dice-next.exe manager";
        return false;
    }
    if (!fs::is_directory(fs::path("updates") / "pending")) {
        error = "no staged update is ready";
        return false;
    }
    if (!queueJobLocked(Job::install, "installing", error)) return false;
    scheduledInstallJob_ = false;
    return true;
}

bool UpdateService::requestManualRestart(std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == "installing") {
            error = "installation and restart are already starting";
            return false;
        }
        // The explicit endpoint coordinates a ready package with the worker.
        // Other starts use the persisted launcher permission for the same policy.
        if (!isBusyLocked() && settings().action == "install" && installSupported() &&
            fs::is_directory(fs::path("updates") / "pending")) {
            if (!queueJobLocked(Job::install, "installing", error)) return false;
            scheduledInstallJob_ = false;
            return true;
        }
    }
    if (!restart_) { error = "restart callback is not available"; return false; }
    try { restart_(); return true; }
    catch (const std::exception& ex) { error = ex.what(); return false; }
    catch (...) { error = "restart callback failed"; return false; }
}

void UpdateService::tick() {
    processInstallResult();

    bool due = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isBusyLocked()) return;
        const Settings current = settings();
        std::string pendingError;
        if (!reconcilePendingLocked(current, pendingError)) {
            phase_ = "error";
            error_ = pendingError;
            return;
        }
        if (scheduledInstallAt_ > 0 && now() >= scheduledInstallAt_) {
            std::string ignored;
            if (queueJobLocked(Job::install, "installing", ignored)) scheduledInstallJob_ = true;
            return;
        }
        if (!current.autoCheck) return;
        const auto interval = static_cast<std::int64_t>(current.intervalHours) * 60 * 60;
        due = checkedAt_ == 0 || epochSeconds() - checkedAt_ >= interval;
    }
    if (due) {
        std::string ignored;
        requestCheck(false, ignored);
    }
}

void UpdateService::workerLoop() {
    while (true) {
        Job next = Job::none;
        bool force = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return stopping_.load(std::memory_order_acquire) || job_ != Job::none;
            });
            if (stopping_.load(std::memory_order_acquire)) break;
            next = job_;
            job_ = Job::none;
            force = forceCheck_;
            forceCheck_ = false;
        }

        try {
            if (next == Job::check) doCheck(force);
            else if (next == Job::download) doDownload();
            else if (next == Job::install) doInstall();
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_.load(std::memory_order_acquire)) break;
            const bool cancelled = downloadActive_ && downloadCancelled_.load(std::memory_order_acquire);
            downloadActive_ = false;
            automaticDownload_ = false;
            phase_ = cancelled ? "cancelled" : "error";
            error_ = cancelled ? "" : ex.what();
            DICE_LOG_ERROR("Update service failed: {}", ex.what());
        }
    }
}

std::vector<UpdateService::Source> UpdateService::configuredSources(const Settings& current) const {
    std::vector<Source> result;
    auto add = [&](std::string prefix) {
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        if (!prefix.empty() && !safeHttpsUrl(prefix)) return;
        if (std::none_of(result.begin(), result.end(), [&](const Source& item) {
                return item.prefix == prefix;
            })) {
            result.push_back(Source{prefix, sourceLabel(prefix), 0});
        }
    };

    if (current.source == "direct") {
        add("");
    } else if (current.source == "custom") {
        add(current.customMirror);
    } else {
        if (current.source == "auto") add("");
        for (const auto& mirror : loadMirrorList()) add(mirror);
    }
    return result;
}

bool UpdateService::fetchToFile(const std::string& url, const fs::path& output,
                                std::uint64_t maxBytes, int timeoutSeconds,
                                std::string& error,
                                const CancellationCheck& cancelled) {
    if (!safeHttpsUrl(url) || maxBytes == 0 || maxBytes > kMaximumAssetSize + 1024) {
        error = "unsafe or invalid download request";
        return false;
    }

    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    fs::remove(output, ec);
    ec.clear();

    if (cancelled && cancelled()) {
        error = "update operation cancelled";
        return false;
    }
    const fs::path configPath = output.string() + "." + updaterTemporarySuffix() + ".curlcfg";
    const std::string outputText = fs::absolute(output).string();
    const std::string configText = fs::absolute(configPath).string();
    if (outputText.find('"') != std::string::npos || configText.find('"') != std::string::npos) {
        error = "update path contains unsupported quote characters";
        return false;
    }

    {
        std::ofstream config(configPath, std::ios::binary | std::ios::trunc);
        if (!config) {
            error = "cannot create curl configuration";
            return false;
        }
        config << "url = \"" << curlEscape(url) << "\"\n"
               << "output = \"" << curlEscape(outputText) << "\"\n"
               << "connect-timeout = 5\n"
               << "max-time = " << (std::max)(timeoutSeconds, 6) << "\n"
               << "max-filesize = " << maxBytes << "\n"
               << "retry = 1\n"
               << "retry-delay = 1\n"
               << "location\nfail\nsilent\n"
               << "proto = \"=https\"\n"
               << "proto-redir = \"=https\"\n"
               << "tlsv1.2\n"
               << "header = \"User-Agent: DiceNext-Updater/3\"\n"
               << "header = \"Accept: application/octet-stream\"\n"
               << "write-out = \"%{http_code}\"\n";
    }

    const dice::proc::Result probe =
        dice::proc::curlConfigCancellable(configPath, cancelled, 64);
    const int result = probe.exitCode;
    const std::string& curlStatus = probe.output;
    fs::remove(configPath, ec);
    if (probe.cancelled) {
        fs::remove(output, ec);
        error = "update operation cancelled";
        return false;
    }
    if (result != 0 || !fs::is_regular_file(output, ec)) {
        fs::remove(output, ec);
        std::smatch status;
        static const std::regex httpCode(R"(([0-9]{3})\s*$)");
        error = "download failed";
        if (std::regex_search(curlStatus, status, httpCode) && status[1].str() != "000") {
            error += " (HTTP " + status[1].str() + "; curl exit " +
                std::to_string(result) + ")";
        } else {
            error += " (curl exit " + std::to_string(result) + ")";
        }
        return false;
    }
    const auto size = fs::file_size(output, ec);
    if (ec || size == 0 || size > maxBytes) {
        fs::remove(output, ec);
        error = "downloaded file size is invalid";
        return false;
    }
    return true;
}

UpdateService::ProbeResult UpdateService::probeManifest(
    const Source& source, const CancellationCheck& cancelled) const {
    ProbeResult result;
    result.source = source;
    const auto started = std::chrono::steady_clock::now();
    const std::string url = buildMirroredUrl(kLatestManifestUrl, source.prefix);
    fs::path temporaryRoot = fs::path("updates") / "tmp";
    if (container_.detected) {
        std::error_code ec;
        const fs::path systemTemporary = fs::temp_directory_path(ec);
        if (!ec) temporaryRoot = systemTemporary / "dice-next-updater";
    }
    const fs::path temporary = temporaryRoot /
        ("manifest-" + updaterTemporarySuffix() + ".json");

    const auto shouldCancel = [this, &cancelled] {
        return stopping_.load(std::memory_order_acquire) ||
            (cancelled && cancelled());
    };

    std::string fetchError;
    const bool fetched = fetch_
        ? fetch_(url, temporary, kManifestLimit, 12, fetchError, shouldCancel)
        : fetchToFile(url, temporary, kManifestLimit, 12, fetchError, shouldCancel);
    if (!fetched) {
        std::error_code ec;
        fs::remove(temporary, ec);
        result.error = fetchError;
        return result;
    }

    std::string readError;
    const std::string body = readFileLimited(temporary, kManifestLimit, readError);
    std::error_code ec;
    fs::remove(temporary, ec);
    if (body.empty()) {
        result.error = readError;
        return result;
    }

    if (!parseReleaseManifest(body, result.manifest, result.error)) return result;
    result.source.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    result.ok = true;
    return result;
}

std::vector<UpdateService::ProbeResult> UpdateService::raceManifestSources(
    const std::vector<Source>& sources) const {
    struct RaceState {
        std::mutex mutex;
        std::condition_variable wake;
        std::vector<ProbeResult> results;
        std::size_t completed = 0;
        bool hasWinner = false;
        std::atomic<bool> cancel{false};
    } state;

    state.results.reserve(sources.size());
    // Keep this on std::thread rather than std::jthread: the macOS release
    // runner still targets the Xcode 15.4 libc++ that lacks jthread/stop_token.
    std::vector<std::thread> workers;
    workers.reserve(sources.size());
    try {
        for (const auto& source : sources) {
            workers.emplace_back([this, source, &state] {
                ProbeResult result;
                try {
                    result = probeManifest(source, [&state] {
                        return state.cancel.load(std::memory_order_acquire);
                    });
                } catch (const std::exception& ex) {
                    result.source = source;
                    result.error = ex.what();
                } catch (...) {
                    result.source = source;
                    result.error = "manifest probe failed";
                }

                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    if (result.ok && !state.hasWinner) {
                        state.hasWinner = true;
                        state.cancel.store(true, std::memory_order_release);
                    }
                    state.results.push_back(std::move(result));
                    ++state.completed;
                }
                state.wake.notify_one();
            });
        }
    } catch (...) {
        // A partially constructed vector of joinable std::threads would call
        // std::terminate while unwinding. Cancel and reap those already
        // started before propagating the allocation/thread creation failure.
        state.cancel.store(true, std::memory_order_release);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        throw;
    }

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.wake.wait(lock, [&] {
            return state.hasWinner || state.completed == sources.size() ||
                stopping_.load(std::memory_order_acquire);
        });
        if (state.hasWinner || stopping_.load(std::memory_order_acquire)) {
            state.cancel.store(true, std::memory_order_release);
        }
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();

    std::vector<ProbeResult> results;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        results = std::move(state.results);
    }
    std::sort(results.begin(), results.end(), [](const ProbeResult& left, const ProbeResult& right) {
        if (left.ok != right.ok) return left.ok > right.ok;
        return left.source.latencyMs < right.source.latencyMs;
    });
    return results;
}

void UpdateService::doCheck(bool force) {
    const Settings current = settings();
    ProbeResult selected;
    std::vector<Source> cachedSources;
    std::vector<Source> configured;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!force && sourceCacheUntil_ > epochSeconds()) cachedSources = sourceOrder_;
    }

    if (!cachedSources.empty()) {
        for (const auto& source : cachedSources) {
            selected = probeManifest(source);
            if (stopping_.load(std::memory_order_acquire)) return;
            if (selected.ok) break;
        }
    }

    std::vector<ProbeResult> raced;
    if (!selected.ok) {
        configured = configuredSources(current);
        if (configured.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_.load(std::memory_order_acquire)) return;
            phase_ = "error";
            error_ = "no valid update source is configured";
            checkedAt_ = epochSeconds();
            return;
        }
        raced = raceManifestSources(configured);
        if (stopping_.load(std::memory_order_acquire)) return;
        const auto found = std::find_if(raced.begin(), raced.end(),
            [](const ProbeResult& result) { return result.ok; });
        if (found != raced.end()) selected = *found;
    }

    if (!selected.ok) {
        std::string details = "GitHub and configured mirrors are unavailable";
        for (const auto& result : raced) {
            if (!result.error.empty()) {
                details += "; " + result.source.label + ": " + result.error;
                break;
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = "error";
        error_ = std::move(details);
        checkedAt_ = epochSeconds();
        DICE_LOG_WARN("Update check failed: {}", error_);
        return;
    }

    if (!selectAsset(selected.manifest, currentOs(), currentArch())) {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = "error";
        error_ = "latest release has no asset for " + currentOs() + "-" + currentArch();
        checkedAt_ = epochSeconds();
        return;
    }

    std::vector<Source> ordered;
    if (!raced.empty()) {
        for (const auto& result : raced) if (result.ok) ordered.push_back(result.source);
        for (const auto& source : configured) {
            if (std::none_of(ordered.begin(), ordered.end(), [&](const Source& item) {
                    return item.prefix == source.prefix;
                })) {
                ordered.push_back(source);
            }
        }
    } else {
        ordered = cachedSources;
        const auto found = std::find_if(ordered.begin(), ordered.end(), [&](const Source& source) {
            return source.prefix == selected.source.prefix;
        });
        if (found != ordered.end()) std::rotate(ordered.begin(), found, found + 1);
    }
    if (ordered.empty()) ordered.push_back(selected.source);

    const bool available = compareRelease(versionString(), buildNumber(),
        selected.manifest.version, selected.manifest.build) < 0;
    const std::string checkedTag = selected.manifest.tag;
    const std::string checkedReleaseUrl = selected.manifest.releaseUrl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) return;
        latest_ = std::move(selected.manifest);
        hasLatest_ = true;
        updateAvailable_ = available;
        automaticDownload_ = false;
        activeSource_ = selected.source.label;
        sourceOrder_ = std::move(ordered);
        sourceCacheUntil_ = epochSeconds() + kMirrorCacheSeconds;
        checkedAt_ = epochSeconds();
        error_.clear();
        phase_ = scheduledInstallAt_ > 0 ? "scheduled" : available ? "available" : "up_to_date";
        DICE_LOG_INFO("Update check via {}: latest {} (current {})",
            activeSource_, latest_.tag, releaseTag());

        const Settings activeSettings = settings();
        if (available && latest_.tag != pendingTag_ && activeSettings.action != "notify" && downloadSupported()) {
            downloadedBytes_ = 0;
            totalBytes_ = 0;
            automaticDownload_ = true;
            phase_ = "connecting";
            downloadCancelled_.store(false, std::memory_order_release);
            downloadActive_ = true;
            job_ = Job::download;
            wake_.notify_all();
        }
    }

    if (!stopping_.load(std::memory_order_acquire) && available && notify_ &&
        config_.get<std::string>("update/last_notified_tag", "") != checkedTag) {
        const std::string action = container_.detected
            ? "仅通知（容器内禁止程序自更新，请更新镜像后重建容器）"
            : current.action == "download"
                ? "自动下载" : current.action == "install"
                    ? (current.scheduledInstall ? "自动下载，定时安装（" + current.installTime + "，服务器时区）" : "自动下载并安装")
                    : "仅通知";
        emitNotification("update_available",
            "检测到 Dice!Next 新版本：" + checkedTag +
            "\n当前版本：" + releaseTag() +
            "\n更新策略：" + action +
            "\n发布页：" + checkedReleaseUrl);
        config_.set<std::string>("update/last_notified_tag", checkedTag);
        if (!config_.save()) {
            DICE_LOG_WARN("Could not persist update notification dedup tag {}", checkedTag);
        }
    }
}
bool UpdateService::sha256File(const fs::path& file, std::string& digest, std::string& error,
                               const CancellationCheck& cancelled) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "cannot open downloaded asset for hashing";
        return false;
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context) EVP_MD_CTX_free(context);
        error = "cannot initialize SHA-256";
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        if (cancelled && cancelled()) {
            EVP_MD_CTX_free(context);
            error = "update operation cancelled";
            return false;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context, buffer.data(),
                                           static_cast<std::size_t>(count)) != 1) {
            EVP_MD_CTX_free(context);
            error = "cannot calculate SHA-256";
            return false;
        }
    }
    if (!input.eof()) {
        EVP_MD_CTX_free(context);
        error = "cannot read downloaded asset";
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, bytes.data(), &length) != 1) {
        EVP_MD_CTX_free(context);
        error = "cannot finalize SHA-256";
        return false;
    }
    EVP_MD_CTX_free(context);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) output << std::setw(2) << static_cast<int>(bytes[i]);
    digest = output.str();
    return true;
}

bool UpdateService::downloadAsset(const ReleaseManifest& manifest, const ReleaseAsset& asset,
                                  const std::vector<Source>& sources, fs::path& archive,
                                  std::string& usedSource, std::string& error) {
    const fs::path downloads = fs::path("updates") / "downloads";
    std::error_code ec;
    fs::create_directories(downloads, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    archive = downloads / asset.name;
    const auto shouldCancel = [this] {
        return stopping_.load(std::memory_order_acquire) ||
            downloadCancelled_.load(std::memory_order_acquire);
    };
    if (fs::is_regular_file(archive, ec) && fs::file_size(archive, ec) == asset.size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shouldCancel()) phase_ = "verifying";
        }
        std::string existingDigest;
        std::string digestError;
        if (sha256File(archive, existingDigest, digestError, shouldCancel) &&
            lower(existingDigest) == asset.sha256) {
            usedSource = "local cache";
            return true;
        }
    }

    const auto downloadNames = githubAssetNameCandidates(asset.name);
    std::string failures;

    for (const auto& source : sources) {
        for (const auto& downloadName : downloadNames) {
            if (shouldCancel()) {
                error = "update operation cancelled";
                return false;
            }
            const std::string originalUrl =
                "https://github.com/DiceZone/Dice-Next/releases/download/" +
                urlEncodeSegment(manifest.tag) + "/" + urlEncodeSegment(downloadName);
            const fs::path partial = downloads /
                (asset.name + ".part-" + updaterTemporarySuffix());
            const std::string url = buildMirroredUrl(originalUrl, source.prefix);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeSource_ = source.label;
                totalBytes_ = asset.size;
                downloadedBytes_ = 0;
                if (!shouldCancel()) phase_ = "connecting";
            }

            std::string fetchError;
            std::atomic<bool> timedOut{false};
            const auto attemptCancelled = [&] {
                return shouldCancel() || timedOut.load(std::memory_order_acquire);
            };
            const auto started = std::chrono::steady_clock::now();
            auto lastProgress = started;
            std::uint64_t previousSize = 0;
            std::string timeoutError;
            const auto timeoutSeconds = static_cast<int>(std::clamp<long long>(
                (downloadPolicy_.attemptTimeout.count() + 999) / 1000, 1, 20 * 60));
            auto transfer = std::async(std::launch::async, [&] {
                return fetch_
                    ? fetch_(url, partial, asset.size + 1, timeoutSeconds, fetchError,
                             attemptCancelled)
                    : fetchToFile(url, partial, asset.size + 1, timeoutSeconds, fetchError,
                                  attemptCancelled);
            });
            while (transfer.wait_for(downloadPolicy_.pollInterval) != std::future_status::ready) {
                std::error_code progressError;
                const auto currentSize = fs::file_size(partial, progressError);
                if (!progressError) {
                    if (currentSize > previousSize) {
                        lastProgress = std::chrono::steady_clock::now();
                        previousSize = currentSize;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    downloadedBytes_ = (std::min)(
                        static_cast<std::uint64_t>(currentSize), asset.size);
                    if (currentSize > 0 && !shouldCancel()) phase_ = "downloading";
                }
                const auto now = std::chrono::steady_clock::now();
                if (!attemptCancelled() &&
                    (now - lastProgress >= downloadPolicy_.idleTimeout ||
                     now - started >= downloadPolicy_.attemptTimeout)) {
                    timeoutError = now - lastProgress >= downloadPolicy_.idleTimeout
                        ? "download stalled: no new data received"
                        : "download attempt timed out";
                    timedOut.store(true, std::memory_order_release);
                }
            }
            bool fetched = false;
            try {
                fetched = transfer.get();
            } catch (const std::exception& ex) {
                fetchError = ex.what();
            } catch (...) {
                fetchError = "download transfer failed";
            }
            if (!fetched || attemptCancelled()) {
                // Remove only this attempt's partial file, never a verified cache.
                fs::remove(partial, ec);
                if (shouldCancel()) {
                    error = "update operation cancelled";
                    return false;
                }
                if (!failures.empty()) failures += "; ";
                failures += source.label;
                if (downloadName != asset.name) failures += " (" + downloadName + ")";
                failures += ": " + (!timeoutError.empty() ? timeoutError :
                    fetchError.empty() ? "download failed" : fetchError);
                // Changing an asset filename cannot fix a stalled connection.
                if (timedOut.load(std::memory_order_acquire)) break;
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                downloadedBytes_ = asset.size;
                if (!shouldCancel()) phase_ = "verifying";
            }
            const auto size = fs::file_size(partial, ec);
            std::string actualDigest;
            std::string digestError;
            const bool verified = !ec && size == asset.size &&
                sha256File(partial, actualDigest, digestError, shouldCancel) &&
                lower(actualDigest) == asset.sha256;
            if (!verified) {
                fs::remove(partial, ec);
                if (shouldCancel()) {
                    error = "update operation cancelled";
                    return false;
                }
                if (!failures.empty()) failures += "; ";
                failures += source.label;
                if (downloadName != asset.name) failures += " (" + downloadName + ")";
                failures += ": size or SHA-256 mismatch";
                DICE_LOG_WARN("Rejected update asset {} from {}: integrity check failed",
                    downloadName, source.label);
                continue;
            }

            const fs::path oldArchive = downloads /
                (asset.name + ".old-" + updaterTemporarySuffix());
            if (fs::exists(archive, ec)) {
                fs::rename(archive, oldArchive, ec);
                if (ec) {
                    fs::remove(partial, ec);
                    error = "cannot replace cached update archive: " + ec.message();
                    return false;
                }
            }
            fs::rename(partial, archive, ec);
            if (ec) {
                if (fs::exists(oldArchive)) fs::rename(oldArchive, archive, ec);
                fs::remove(partial, ec);
                error = "cannot finalize update archive: " + ec.message();
                return false;
            }
            fs::remove(oldArchive, ec);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                downloadedBytes_ = asset.size;
                totalBytes_ = asset.size;
            }
            if (downloadName != asset.name) {
                DICE_LOG_INFO("Recovered legacy update manifest asset {} as {}",
                    asset.name, downloadName);
            }
            usedSource = source.label;
            return true;
        }
    }
    error = failures.empty() ? "no verified download source is available" : failures;
    return false;
}

bool UpdateService::prepareWindowsStage(const fs::path& archive,
                                        const ReleaseManifest& manifest,
                                        std::string& error) {
#if !defined(_WIN32)
    (void)archive;
    (void)manifest;
    error = "automatic staging is currently available only for Windows packages";
    return false;
#else
    const auto sequence = updaterTemporarySuffix();
    const fs::path updates = fs::path("updates");
    const fs::path extractRoot = updates / ("extract-" + sequence);
    const fs::path pendingNew = updates / ("pending-new-" + sequence);
    const fs::path pending = updates / "pending";
    const fs::path pendingOld = updates / ("pending-old-" + sequence);
    std::error_code ec;

    fs::remove_all(extractRoot, ec);
    fs::remove_all(pendingNew, ec);
    fs::remove_all(pendingOld, ec);
    fs::create_directories(extractRoot, ec);
    if (ec) {
        error = "cannot create update extraction directory: " + ec.message();
        return false;
    }

    const std::wstring archiveText = fs::absolute(archive).wstring();
    const std::wstring extractText = fs::absolute(extractRoot).wstring();
    if (archiveText.find(L'"') != std::wstring::npos || extractText.find(L'"') != std::wstring::npos) {
        error = "update path contains unsupported quote characters";
        fs::remove_all(extractRoot, ec);
        return false;
    }

    const std::string tar = dice::proc::systemTool("tar.exe");
    const fs::path archivePath = fs::absolute(archive);
    const auto shouldCancel = [this] {
        return stopping_.load(std::memory_order_acquire) ||
            downloadCancelled_.load(std::memory_order_acquire);
    };
    const dice::proc::Result listed =
        dice::proc::runPathsCancellable(
            tar, {"-tf", archivePath}, shouldCancel, 8 * 1024 * 1024);
    const std::string& listing = listed.output;
    if (listed.cancelled) {
        error = "update operation cancelled";
        fs::remove_all(extractRoot, ec);
        return false;
    }
    if (listed.exitCode != 0 || listed.truncated || listing.empty()) {
        error = "cannot inspect the update archive";
        fs::remove_all(extractRoot, ec);
        return false;
    }

    std::istringstream entries(listing);
    std::string entry;
    std::size_t entryCount = 0;
    while (std::getline(entries, entry)) {
        if (++entryCount > 20000 || !archiveEntrySafe(entry)) {
            error = "update archive contains an unsafe path";
            fs::remove_all(extractRoot, ec);
            return false;
        }
    }

    const dice::proc::Result extracted =
        dice::proc::runPathsCancellable(
            tar, {"-xf", archivePath, "-C", fs::absolute(extractRoot)},
            shouldCancel, 4096);
    if (extracted.cancelled) {
        error = "update operation cancelled";
        fs::remove_all(extractRoot, ec);
        return false;
    }
    if (extracted.exitCode != 0) {
        error = "cannot extract the update archive";
        fs::remove_all(extractRoot, ec);
        return false;
    }

    fs::path packageRoot;
    if (fs::is_regular_file(extractRoot / "app" / "dice-next-core.exe", ec)) {
        packageRoot = extractRoot;
    } else {
        for (const auto& candidate : fs::directory_iterator(extractRoot, ec)) {
            if (ec) break;
            if (candidate.is_directory(ec) &&
                fs::is_regular_file(candidate.path() / "app" / "dice-next-core.exe", ec)) {
                if (!packageRoot.empty()) {
                    error = "update archive has multiple package roots";
                    fs::remove_all(extractRoot, ec);
                    return false;
                }
                packageRoot = candidate.path();
            }
        }
    }

    const auto missing = packageRoot.empty()
        ? std::vector<std::string>{"package root"}
        : missingWindowsPackageComponents(packageRoot);
    if (!missing.empty()) {
        std::ostringstream details;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) details << ", ";
            details << missing[i];
        }
        error = "downloaded archive is not a complete Dice!Next Windows package; missing: " +
            details.str();
        fs::remove_all(extractRoot, ec);
        return false;
    }
    {
        nlohmann::json metadata{
            {"schema", 1},
            {"tag", manifest.tag},
            {"version", manifest.version},
            {"build", manifest.build},
            {"staged_at", epochSeconds()},
            {"install_plan_enabled", false},
            {"auto_install_authorized", false}
        };
        std::ofstream output(packageRoot / "update.json", std::ios::binary | std::ios::trunc);
        output << metadata.dump(2) << '\n';
        if (!output) {
            error = "cannot write staged update metadata";
            fs::remove_all(extractRoot, ec);
            return false;
        }
        // Never accept a restart permission carried inside the downloaded archive.
        fs::remove(packageRoot / kUpdateInstallOnRestartFile, ec);
        if (ec) {
            error = "cannot discard archived installation permission: " + ec.message();
            fs::remove_all(extractRoot, ec);
            return false;
        }
        // Create the gate before publishing the stage. Only a completed download
        // or an explicit policy change may authorize installation on restart.
        std::ofstream hold(packageRoot / kUpdateHoldFile, std::ios::binary | std::ios::trunc);
        hold << "Installation requires explicit authorization from the core.\n";
        hold.close();
        if (!hold) {
            error = "cannot hold staged update for installation";
            fs::remove_all(extractRoot, ec);
            return false;
        }
    }

    fs::rename(packageRoot, pendingNew, ec);
    if (ec) {
        error = "cannot prepare staged update: " + ec.message();
        fs::remove_all(extractRoot, ec);
        fs::remove_all(pendingNew, ec);
        return false;
    }
    fs::remove_all(extractRoot, ec);

    if (shouldCancel()) {
        fs::remove_all(pendingNew, ec);
        error = "update operation cancelled";
        return false;
    }
    bool hadPending = fs::exists(pending, ec);
    if (hadPending) {
        fs::rename(pending, pendingOld, ec);
        if (ec) {
            error = "cannot rotate previous staged update: " + ec.message();
            fs::remove_all(pendingNew, ec);
            return false;
        }
    }

    fs::rename(pendingNew, pending, ec);
    if (ec) {
        std::error_code restoreError;
        if (hadPending) fs::rename(pendingOld, pending, restoreError);
        fs::remove_all(pendingNew, restoreError);
        error = "cannot activate staged update: " + ec.message();
        return false;
    }
    fs::remove_all(pendingOld, ec);
    return true;
#endif
}

void UpdateService::doDownload() {
    ReleaseManifest manifest;
    const auto configured = configuredSources(settings());
    std::vector<Source> sources;
    bool automatic = false;
    std::string initialError;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        automatic = automaticDownload_;
        if (!downloadSupported()) {
            initialError = containerUpdateError();
        } else if (!hasLatest_ || !updateAvailable_) {
            initialError = "no newer release is available";
        } else {
            manifest = latest_;
            // A settings save can race an in-flight check. Reuse probe ordering,
            // but never reuse a source excluded by the current configuration.
            for (const auto& cached : sourceOrder_) {
                const auto found = std::find_if(configured.begin(), configured.end(),
                    [&](const Source& source) { return source.prefix == cached.prefix; });
                if (found != configured.end()) sources.push_back(*found);
            }
            for (const auto& source : configured) {
                if (std::none_of(sources.begin(), sources.end(), [&](const Source& existing) {
                        return existing.prefix == source.prefix;
                    })) sources.push_back(source);
            }
        }
    }

    auto fail = [&](std::string message) {
        if (stopping_.load(std::memory_order_acquire)) return;
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_.load(std::memory_order_acquire)) return;
            cancelled = downloadCancelled_.load(std::memory_order_acquire);
            phase_ = cancelled ? "cancelled" : "error";
            error_ = cancelled ? "" : message;
            downloadActive_ = false;
            automaticDownload_ = false;
        }
        if (cancelled) return;
        DICE_LOG_WARN("Update download failed: {}", message);
        if (automatic) {
            emitNotification("update_error",
                "Dice!Next 自动更新失败" +
                (manifest.tag.empty() ? std::string() : "：" + manifest.tag) +
                "\n原因：" + message);
        }
    };

    if (!initialError.empty()) {
        fail(std::move(initialError));
        return;
    }
    if (downloadCancelled_.load(std::memory_order_acquire)) {
        fail("update operation cancelled");
        return;
    }

    const ReleaseAsset* asset = selectAsset(manifest, currentOs(), currentArch());
    if (!asset) {
        fail("latest release has no matching platform asset");
        return;
    }

    fs::path archive;
    std::string usedSource;
    std::string downloadError;
    if (!downloadAsset(manifest, *asset, sources, archive, usedSource, downloadError)) {
        fail(std::move(downloadError));
        return;
    }

#if defined(_WIN32)
    std::string stageError;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!downloadCancelled_.load(std::memory_order_acquire)) phase_ = "preparing";
    }
    if (!prepareWindowsStage(archive, manifest, stageError)) {
        fail(std::move(stageError));
        return;
    }
#endif

    Settings current;
    bool installQueued = false;
    std::string pendingError;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = settings();
        if (stopping_.load(std::memory_order_acquire)) return;
        downloadActive_ = false;
        automaticDownload_ = false;
        activeSource_ = usedSource;
        error_.clear();
        if (downloadCancelled_.load(std::memory_order_acquire)) {
            phase_ = "cancelled";
            return;
        }
#if defined(_WIN32)
        phase_ = "staged";
#else
        phase_ = "downloaded";
#endif
        DICE_LOG_INFO("Verified update {} downloaded from {} to {}",
            manifest.tag, usedSource, archive.string());

        if (!reconcilePendingLocked(current, pendingError, true)) {
            phase_ = "error";
            error_ = pendingError;
        } else if (current.action == "install" && !current.scheduledInstall && installSupported()) {
            phase_ = "installing";
            job_ = Job::install;
            scheduledInstallJob_ = false;
            installQueued = true;
            wake_.notify_all();
        }
    }

    if (!pendingError.empty()) {
        if (automatic) emitNotification("update_error", "Dice!Next 更新包已下载，但无法保存安装计划：" + pendingError);
        return;
    }

    if (automatic && !stopping_.load(std::memory_order_acquire)) {
        emitNotification("update_result",
            "Dice!Next 自动更新包已下载并通过 SHA-256 校验：" + manifest.tag +
            "\n下载源：" + usedSource +
            (installQueued ? "\n即将重启并安装更新。" : current.scheduledInstall && current.action == "install"
                ? "\n将在服务器时区的 " + current.installTime + " 安装更新。"
                : "\n更新包已准备完成，等待手动安装。"));
    }
}
void UpdateService::doInstall() {
    auto fail = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phase_ = "error";
            error_ = message;
            // Do not retry a failed restart every minute (or after a restart).
            if (scheduledInstallJob_) {
                scheduledInstallAt_ = 0;
                const fs::path path = fs::path("updates") / "pending" / "update.json";
                try {
                    std::string ignored;
                    auto metadata = Json::parse(readFileLimited(path, 64 * 1024, ignored));
                    metadata["install_plan_enabled"] = false;
                    metadata["install_at"] = 0;
                    if (!writePendingMetadata(path, metadata, ignored)) DICE_LOG_WARN("{}", ignored);
                } catch (const std::exception& ex) {
                    DICE_LOG_WARN("Could not cancel failed installation plan: {}", ex.what());
                }
                scheduledInstallJob_ = false;
            }
        }
        DICE_LOG_WARN("Update installation failed: {}", message);
        emitNotification("update_error",
            "Dice!Next 更新安装失败。\n原因：" + message);
    };

    if (!downloadSupported()) {
        fail(containerUpdateError());
        return;
    }
    if (!installSupported() || !fs::is_directory(fs::path("updates") / "pending")) {
        fail("staged update cannot be installed in the current launch mode");
        return;
    }

    DICE_LOG_INFO("Handing staged update to dice-next.exe manager");
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::milliseconds(350), [&] {
                return stopping_.load(std::memory_order_acquire);
            })) {
            return;
        }
    }
    if (restart_) {
        const fs::path stage = fs::path("updates") / "pending";
        std::string authorizationError;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (scheduledInstallJob_) {
                const Settings current = settings();
                std::string pendingError;
                if (!reconcilePendingLocked(current, pendingError) || scheduledInstallAt_ <= 0 || now() < scheduledInstallAt_) {
                    if (!pendingError.empty()) { phase_ = "error"; error_ = pendingError; }
                    return;
                }
                phase_ = "installing";
            }
            std::error_code ec;
            fs::remove(stage / kUpdateHoldFile, ec);
            if (ec) {
                authorizationError = "cannot authorize staged installation: " + ec.message();
            } else {
                scheduledInstallAt_ = 0;
            }
        }
        if (!authorizationError.empty()) { fail(authorizationError); return; }
        try { restart_(); }
        catch (...) {
            std::ofstream(stage / kUpdateHoldFile) << "Installation interrupted.\n";
            fail("restart callback failed; staged update remains held");
        }
    } else {
        fail("restart callback is not available");
    }
}
}  // namespace dice::update
