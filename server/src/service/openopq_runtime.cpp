// winsock2 须在任何 windows.h 之前。
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "openopq_runtime.h"

#include "../common/logger.h"
#include "../common/subprocess.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>

namespace dice::openopq {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// 中心的清单地址。清单本身带签名，地址被劫持也只能让下载失败。
// 不带扩展名、放在 /api 下：前面是 CDN，静态扩展名可能被缓存，带 Key 才给的东西不能进缓存。
constexpr const char* kManifestUrl = "https://cloud.dice.zone/api/openopq/manifest";
// 对应私钥在骰子之间仓库外保管，用来给清单签名。
constexpr const char* kManifestPublicKey = "eQKl/N6UbaUw8g4OW9R+UfiH40rXSvcVFmHrC6xE3OI=";
constexpr std::uint64_t kManifestLimit = 256 * 1024;
constexpr std::uint64_t kBinaryLimit = 128ull * 1024 * 1024;
constexpr auto kManifestCacheTtl = std::chrono::minutes(30);
constexpr int kBackoffFirst = 5;
constexpr int kBackoffMax = 300;

std::atomic<unsigned long long> sequence{0};

/// 本地联调用：DICE_OPENOPQ_MANIFEST_URL 指向 http://127.0.0.1 上的假中心。
/// 清单仍须用正式公钥验签，所以这只换地址、换不了内容。
std::string manifestUrl(bool& loopbackHttp) {
    loopbackHttp = false;
    const char* overridden = std::getenv("DICE_OPENOPQ_MANIFEST_URL");
    if (overridden && *overridden) {
        const std::string url = overridden;
        if (url.rfind("http://127.0.0.1:", 0) == 0) { loopbackHttp = true; return url; }
        if (url.rfind("https://", 0) == 0) return url;
    }
    return kManifestUrl;
}

bool safeUrl(const std::string& url, bool loopbackHttp) {
    if (url.size() > 2048) return false;
    const bool https = url.rfind("https://", 0) == 0;
    const bool loopback = loopbackHttp && url.rfind("http://127.0.0.1:", 0) == 0;
    if (!https && !loopback) return false;
    return std::none_of(url.begin(), url.end(), [](unsigned char c) {
        return c <= 0x20 || c == '"' || c == '\\' || c == '`' || c == '$' || c == 0x7f;
    });
}

bool safeToken(const std::string& value, std::size_t maxLength) {
    return !value.empty() && value != "." && value != ".." && value.size() <= maxLength &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        });
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

bool base64Decode(const std::string& text, std::string& out) {
    if (text.empty() || text.size() % 4 != 0 || text.size() > 1024 * 1024) return false;
    std::string buffer(text.size() / 4 * 3, '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(buffer.data()),
                                       reinterpret_cast<const unsigned char*>(text.data()),
                                       static_cast<int>(text.size()));
    if (length < 0) return false;
    std::size_t padding = 0;
    if (text.back() == '=') ++padding;
    if (text.size() > 1 && text[text.size() - 2] == '=') ++padding;
    buffer.resize(static_cast<std::size_t>(length) - padding);
    out = std::move(buffer);
    return true;
}

bool verifyEd25519(const std::string& publicKeyBase64, const std::string& message,
                   const std::string& signatureBase64) {
    std::string key, signature;
    if (!base64Decode(publicKeyBase64, key) || key.size() != 32) return false;
    if (!base64Decode(signatureBase64, signature) || signature.size() != 64) return false;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(key.data()), key.size());
    if (!pkey) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const bool ok = context &&
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(context, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                         reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    if (context) EVP_MD_CTX_free(context);
    EVP_PKEY_free(pkey);
    return ok;
}

bool sha256File(const fs::path& file, std::string& digest) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context) EVP_MD_CTX_free(context);
        return false;
    }
    std::array<char, 64 * 1024> buffer{};
    bool ok = true;
    while (ok && input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) ok = EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    ok = ok && input.eof() && EVP_DigestFinal_ex(context, bytes.data(), &length) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) return false;
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) hex << std::setw(2) << static_cast<int>(bytes[i]);
    digest = hex.str();
    return true;
}

std::string readFile(const fs::path& file, std::uint64_t limit) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    std::string text;
    text.resize(static_cast<std::size_t>(limit));
    input.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

std::string pathText(const fs::path& path) {
    try { return fs::absolute(path).string(); } catch (...) { return {}; }
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

struct Fetched {
    int http = 0;
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

/// 带 Key 下载到文件。Key 写在 curl 配置文件里，不出现在进程列表上。
Fetched fetchWithKey(const std::string& url, const std::string& apiKey, const fs::path& output,
                     std::uint64_t maxBytes, int timeoutSeconds, bool loopbackHttp,
                     const dice::proc::CancellationCheck& cancelled) {
    Fetched result;
    if (!safeUrl(url, loopbackHttp) || !validApiKey(apiKey)) {
        result.error = "下载地址或密钥格式不正确";
        return result;
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    fs::remove(output, ec);
    const fs::path config = output.string() + "." + std::to_string(++sequence) + ".curlcfg";
    const std::string outputText = pathText(output);
    if (outputText.empty() || outputText.find('"') != std::string::npos) {
        result.error = "下载目录路径无法使用";
        return result;
    }
    {
        std::ofstream file(config, std::ios::binary | std::ios::trunc);
        if (!file) {
            result.error = "无法写入下载配置";
            return result;
        }
        file << "url = \"" << curlEscape(url) << "\"\n"
             << "output = \"" << curlEscape(outputText) << "\"\n"
             << "connect-timeout = 8\n"
             << "max-time = " << (std::max)(timeoutSeconds, 10) << "\n"
             << "max-filesize = " << maxBytes << "\n"
             << "location\nfail\nsilent\n"
             << "proto = \"" << (loopbackHttp ? "=http,https" : "=https") << "\"\n"
             << "proto-redir = \"" << (loopbackHttp ? "=http,https" : "=https") << "\"\n"
             << "header = \"User-Agent: DiceNext-OpenOPQ/1\"\n"
             << "header = \"Authorization: Bearer " << curlEscape(apiKey) << "\"\n"
             << "write-out = \"%{http_code}\"\n";
    }
    const auto run = dice::proc::curlConfigCancellable(config, cancelled, 64);
    fs::remove(config, ec);
    result.cancelled = run.cancelled;
    result.http = std::atoi(run.output.c_str());
    result.ok = run.exitCode == 0 && result.http >= 200 && result.http < 300 && fs::is_regular_file(output, ec);
    if (!result.ok) {
        fs::remove(output, ec);
        result.error = result.http > 0 ? "中心返回 HTTP " + std::to_string(result.http)
                                       : "无法连接中心（curl " + std::to_string(run.exitCode) + "）";
    }
    return result;
}

struct CachedManifest {
    std::chrono::steady_clock::time_point at;
    Manifest manifest;
};
std::mutex manifestMutex;
std::unordered_map<std::string, CachedManifest> manifestCache;  // 按 Key 缓存：每把 Key 都要中心认过
std::mutex binaryMutex;

std::string tail(const std::string& text, std::size_t length) {
    std::string out = text.size() > length ? text.substr(text.size() - length) : text;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

}  // namespace

// ─── 纯函数 ───────────────────────────────────────────────────

bool validApiKey(const std::string& key) {
    return key.rfind("bdc_", 0) == 0 && key.size() >= 12 && key.size() <= 256 &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        });
}

bool validAccount(const std::string& account) {
    return account.size() >= 5 && account.size() <= 12 && account.front() != '0' &&
        std::all_of(account.begin(), account.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool parseSignedManifest(const std::string& body, const std::string& publicKeyBase64,
                         Manifest& out, std::string& error) {
    error.clear();
    if (body.size() > kManifestLimit) {
        error = "清单超过大小限制";
        return false;
    }
    const json envelope = json::parse(body, nullptr, false);
    if (!envelope.is_object() || !envelope.contains("payload") || !envelope["payload"].is_string() ||
        !envelope.contains("signature") || !envelope["signature"].is_string()) {
        error = "清单格式不正确";
        return false;
    }
    const std::string payload = envelope["payload"].get<std::string>();
    if (!verifyEd25519(publicKeyBase64, payload, envelope["signature"].get<std::string>())) {
        error = "清单签名校验失败";
        return false;
    }
    const json doc = json::parse(payload, nullptr, false);
    if (!doc.is_object() || !doc.contains("schema") || !doc["schema"].is_number_integer() ||
        doc["schema"] != 1) {
        error = "清单版本不受支持";
        return false;
    }
    for (const auto* key : {"version", "qq_version", "sign_url"}) {
        if (!doc.contains(key) || !doc[key].is_string()) {
            error = "清单字段格式不正确：" + std::string(key);
            return false;
        }
    }
    Manifest manifest;
    manifest.version = doc.value("version", std::string());
    manifest.qqVersion = doc.value("qq_version", std::string());
    manifest.signUrl = doc.value("sign_url", std::string());
    if (!safeToken(manifest.version, 64) || !safeToken(manifest.qqVersion, 64) ||
        manifest.signUrl.find("{key}") == std::string::npos) {
        error = "清单字段不完整";
        return false;
    }
    if (doc.contains("cdn_lists")) {
        if (!doc["cdn_lists"].is_array()) {
            error = "清单里的 CDN 列表格式不正确";
            return false;
        }
        for (const auto& item : doc["cdn_lists"]) {
            if (!item.is_string()) {
                error = "清单里的 CDN 地址格式不正确";
                return false;
            }
            manifest.cdnLists.push_back(item.get<std::string>());
        }
    }
    static const std::regex hex64("^[0-9a-f]{64}$");
    if (!doc.contains("assets") || !doc["assets"].is_array() || doc["assets"].empty()) {
        error = "清单未提供执行档列表";
        return false;
    }
    if (doc.contains("assets") && doc["assets"].is_array()) {
        for (const auto& item : doc["assets"]) {
            if (!item.is_object()) {
                error = "清单里的执行档条目格式不正确";
                return false;
            }
            for (const auto* key : {"os", "arch", "url", "sha256"}) {
                if (!item.contains(key) || !item[key].is_string()) {
                    error = "清单里的执行档字段格式不正确：" + std::string(key);
                    return false;
                }
            }
            if (!item.contains("size") || !item["size"].is_number_integer() ||
                item["size"] <= 0 || item["size"] > kBinaryLimit) {
                error = "清单里的执行档大小不正确";
                return false;
            }
            Asset asset;
            asset.os = item.value("os", std::string());
            asset.arch = item.value("arch", std::string());
            asset.url = item.value("url", std::string());
            asset.sha256 = item.value("sha256", std::string());
            asset.size = item.value("size", std::uint64_t{0});
            if (asset.os.empty() || asset.arch.empty() || asset.url.empty() ||
                !std::regex_match(asset.sha256, hex64) || asset.size == 0 || asset.size > kBinaryLimit) {
                error = "清单里的执行档条目不完整";
                return false;
            }
            manifest.assets.push_back(std::move(asset));
        }
    }
    out = std::move(manifest);
    return true;
}

const Asset* assetFor(const Manifest& manifest, const std::string& os, const std::string& arch) {
    for (const auto& asset : manifest.assets)
        if (asset.os == os && asset.arch == arch) return &asset;
    return nullptr;
}

std::string signUrlFor(const Manifest& manifest, const std::string& apiKey) {
    std::string url = manifest.signUrl;
    for (std::size_t at = url.find("{key}"); at != std::string::npos; at = url.find("{key}", at + apiKey.size()))
        url.replace(at, 5, apiKey);
    return url;
}

json buildConfig(const Manifest& manifest, const std::string& apiKey, const std::string& account,
                 int reverseWsPort, const std::string& reverseToken) {
    json config{
        {"CDNLists", manifest.cdnLists},
        {"FakeSignApi", signUrlFor(manifest, apiKey)},
        {"FileLists", json::array()},
        {"Version", manifest.qqVersion},
        {"Accounts", json::array({account})},
        {"Protocol", "onebot11"},
        {"OneBot", {
            {"message_format", "array"},
            {"heartbeat_interval_ms", 15000},
        }},
    };
    if (reverseWsPort > 0) {
        config["OneBot"]["reverse_ws"] = json::array({{
            {"url", "ws://127.0.0.1:" + std::to_string(reverseWsPort) + "/"},
            {"role", "Universal"},
            {"token", reverseToken},
            {"reconnect_interval_ms", 3000},
        }});
    }
    return config;
}

int pickFreeLoopbackPort() {
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return 0; }
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int port = 0;
    socklen_t length = sizeof(address);
    if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        getsockname(sock, reinterpret_cast<sockaddr*>(&address), &length) == 0)
        port = ntohs(address.sin_port);
#if defined(_WIN32)
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    return port;
}

std::string randomToken() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (auto b : bytes) hex << std::setw(2) << static_cast<int>(b);
    return hex.str();
}

// ─── 单个账号的 openopq 进程 ──────────────────────────────────

class Instance {
public:
    explicit Instance(LaunchParams p) : params(std::move(p)), dir(Runtime::adapterDir(params.adapterId)) {}
    ~Instance() {
        stopping = true;
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void start() {
        stopping = false;
        {
            std::lock_guard lock(mutex);
            stage = "preparing";
            error.clear();
            alive = true;
        }
        worker = std::thread([this] {
            try {
                run();
            } catch (const std::exception& ex) {
                const std::string message = "内置登录运行失败：" + tail(ex.what(), 400);
                setStage("failed", message);
                DICE_LOG_ERROR_M("内置登录", "账号 {}：{}。", params.adapterId, message);
            } catch (...) {
                setStage("failed", "内置登录运行失败：未知异常");
                DICE_LOG_ERROR_M("内置登录", "账号 {}：内置登录发生未知异常，已停止。", params.adapterId);
            }
            std::lock_guard lock(mutex);
            alive = false;
        });
    }

    void stop() {
        stopping = true;
        wake.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        alive = false;
        stage = "stopped";
    }

    bool running() {
        std::lock_guard lock(mutex);
        return alive;
    }

    json snapshot() {
        std::lock_guard lock(mutex);
        json out{{"stage", stage}, {"error", error}, {"version", version},
                 {"restarts", restarts}, {"account", params.account}};
        // 下载中报已收到多少：curl 边收边写，量一下半成品的大小就够了。
        // 几 MB 的东西在慢线路上要下一阵子，页面上只有一句「正在下载」会被当成卡死。
        if (stage == "downloading" && downloadTotal > 0) {
            std::error_code ec;
            const auto received = fs::exists(downloadPath, ec) ? fs::file_size(downloadPath, ec) : 0;
            out["download"] = {{"bytes", ec ? 0 : received}, {"total", downloadTotal}};
        }
        return out;
    }

    bool isRunningStage() {
        std::lock_guard lock(mutex);
        return stage == "running";
    }

    /// 本机管理接口。会话 Cookie 存在账号目录里；401 时用 console-key.txt 重新登录一次。
    bool console(const std::string& method, const std::string& path, const json& body,
                 int& http, json& reply, std::string& failure) {
        std::lock_guard lock(consoleMutex);
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string text;
            if (!consoleRaw(method, path, body.is_null() ? std::string() : body.dump(), http, text, failure)) return false;
            if (http == 401 && attempt == 0) {
                const std::string key = readFile(dir / "data" / "console-key.txt", 4096);
                const auto begin = key.find_first_not_of(" \r\n\t");
                if (begin == std::string::npos) {
                    failure = "找不到 openopq 控制台密钥";
                    return false;
                }
                const std::string trimmed = key.substr(begin, key.find_last_not_of(" \r\n\t") - begin + 1);
                std::string ignored;
                int loginHttp = 0;
                if (!consoleRaw("POST", "auth/login", json{{"key", trimmed}}.dump(), loginHttp, ignored, failure)) return false;
                if (loginHttp != 200) {
                    failure = "openopq 控制台拒绝了密钥（HTTP " + std::to_string(loginHttp) + "）";
                    return false;
                }
                continue;
            }
            reply = json::parse(text, nullptr, false);
            if (reply.is_discarded()) reply = json::object();
            return true;
        }
        failure = "openopq 控制台认证失败";
        return false;
    }

    LaunchParams params;
    const fs::path dir;
    std::atomic<bool> stopping{false};
    std::mutex statusCacheMutex;
    json cachedConsole;
    std::chrono::steady_clock::time_point cachedAt;

private:
    bool consoleRaw(const std::string& method, const std::string& path, const std::string& body,
                    int& http, std::string& text, std::string& failure) {
        const auto id = std::to_string(++sequence);
        const fs::path config = dir / ("console-" + id + ".curlcfg");
        const fs::path response = dir / ("console-" + id + ".out");
        const std::string jar = pathText(dir / "console.cookies");
        const std::string responseText = pathText(response);
        if (jar.empty() || responseText.empty() || jar.find('"') != std::string::npos) {
            failure = "账号目录路径无法使用";
            return false;
        }
        {
            std::ofstream file(config, std::ios::binary | std::ios::trunc);
            if (!file) {
                failure = "无法写入请求配置";
                return false;
            }
            file << "url = \"http://127.0.0.1:" << params.consolePort << "/console/api/" << curlEscape(path) << "\"\n"
                 << "proto = \"=http\"\n"
                 << "noproxy = \"*\"\n"
                 << "connect-timeout = 2\nmax-time = 8\nsilent\n"
                 << "max-filesize = 4194304\n"
                 << "cookie = \"" << curlEscape(jar) << "\"\n"
                 << "cookie-jar = \"" << curlEscape(jar) << "\"\n"
                 << "output = \"" << curlEscape(responseText) << "\"\n"
                 << "write-out = \"%{http_code}\"\n";
            if (method == "POST") {
                file << "request = \"POST\"\n"
                     << "header = \"Content-Type: application/json\"\n"
                     << "data-raw = \"" << curlEscape(body.empty() ? std::string("{}") : body) << "\"\n";
            }
        }
        const auto run = dice::proc::curlConfig(config, 64);
        std::error_code ec;
        fs::remove(config, ec);
        text = readFile(response, 4 * 1024 * 1024);
        fs::remove(response, ec);
        http = std::atoi(run.output.c_str());
        if (http == 0) {
            failure = "openopq 管理接口没有响应";
            return false;
        }
        return true;
    }

    void setStage(const std::string& next, const std::string& message = {}) {
        std::lock_guard lock(mutex);
        stage = next;
        error = message;
    }

    void sleepFor(int seconds) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::seconds(seconds), [this] { return stopping.load(); });
    }

    bool loadManifest(Manifest& manifest, int& http, std::string& failure) {
        {
            std::lock_guard lock(manifestMutex);
            auto it = manifestCache.find(params.apiKey);
            if (it != manifestCache.end() && std::chrono::steady_clock::now() - it->second.at < kManifestCacheTtl) {
                manifest = it->second.manifest;
                return true;
            }
        }
        bool loopbackHttp = false;
        const std::string url = manifestUrl(loopbackHttp);
        const fs::path file = dir / "manifest.json";
        const auto fetched = fetchWithKey(url, params.apiKey, file, kManifestLimit, 20, loopbackHttp,
                                          [this] { return stopping.load(); });
        http = fetched.http;
        if (!fetched.ok) {
            failure = "取 openopq 清单失败：" + fetched.error;
            return false;
        }
        const std::string body = readFile(file, kManifestLimit);
        std::error_code ec;
        fs::remove(file, ec);
        if (!parseSignedManifest(body, kManifestPublicKey, manifest, failure)) return false;
        if (!safeUrl(signUrlFor(manifest, params.apiKey), loopbackHttp)) {
            failure = "清单里的签名地址不可用";
            return false;
        }
        std::lock_guard lock(manifestMutex);
        manifestCache[params.apiKey] = {std::chrono::steady_clock::now(), manifest};
        return true;
    }

    bool ensureBinary(const Manifest& manifest, fs::path& binary, int& http, std::string& failure) {
        const Asset* asset = assetFor(manifest, currentOs(), currentArch());
        if (!asset) {
            failure = "中心没有提供本系统（" + currentOs() + "-" + currentArch() + "）的 openopq";
            return false;
        }
        bool loopbackHttp = false;
        manifestUrl(loopbackHttp);
        if (!safeUrl(asset->url, loopbackHttp)) {
            failure = "清单里的下载地址不可用";
            return false;
        }
        const fs::path folder = fs::path("runtime") / "openopq" / manifest.version;
#if defined(_WIN32)
        binary = folder / "openopq.exe";
#else
        binary = folder / "openopq";
#endif
        std::lock_guard lock(binaryMutex);  // 多个账号同时启动时只下载一次
        std::error_code ec;
        std::string digest;
        if (fs::is_regular_file(binary, ec) && fs::file_size(binary, ec) == asset->size &&
            sha256File(binary, digest) && digest == asset->sha256)
            return true;

        const fs::path partial = binary.string() + ".partial";
        {
            std::lock_guard progress(mutex);
            downloadPath = partial;
            downloadTotal = asset->size;
        }
        setStage("downloading");
        const auto fetched = fetchWithKey(asset->url, params.apiKey, partial, asset->size, 600, loopbackHttp,
                                          [this] { return stopping.load(); });
        http = fetched.http;
        if (!fetched.ok) {
            failure = "下载 openopq 失败：" + fetched.error;
            return false;
        }
        if (fs::file_size(partial, ec) != asset->size || !sha256File(partial, digest) || digest != asset->sha256) {
            fs::remove(partial, ec);
            failure = "下载的 openopq 校验不符，已丢弃";
            return false;
        }
        fs::remove(binary, ec);
        fs::rename(partial, binary, ec);
        if (ec) {
            fs::remove(partial, ec);
            failure = "无法放置 openopq 执行档：" + ec.message();
            return false;
        }
#if !defined(_WIN32)
        fs::permissions(binary, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                        fs::perm_options::replace, ec);
#endif
        DICE_LOG_INFO_M("内置登录", "openopq {} 已下载并校验。", manifest.version);
        return true;
    }

    void killStale() {
#if !defined(_WIN32)
        // DN 异常退出时子进程会留下来占着端口；按本账号独有的数据目录找出来结束掉。
        // Windows 上进程在作业对象里，DN 退出时系统会一并结束它。
        const std::string target = pathText(dir / "data");
        if (target.empty()) return;
        std::string pattern;
        for (char c : target) {
            if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) pattern.push_back('\\');
            pattern.push_back(c);
        }
        dice::proc::run("pkill", {"-f", "--", "--data-dir " + pattern + "( |$)"}, 256);
#endif
    }

    void rotateLog() {
        std::error_code ec;
        const fs::path log = dir / "openopq.log";
        if (fs::is_regular_file(log, ec) && fs::file_size(log, ec) > 8ull * 1024 * 1024) {
            fs::remove(dir / "openopq.log.1", ec);
            fs::rename(log, dir / "openopq.log.1", ec);
        }
    }

    void run() {
        int backoff = kBackoffFirst;
        std::error_code ec;
        fs::create_directories(dir / "data", ec);
        while (!stopping) {
            setStage("preparing");
            Manifest manifest;
            fs::path binary;
            int http = 0;
            std::string failure;
            const bool ready = loadManifest(manifest, http, failure) && ensureBinary(manifest, binary, http, failure);
            if (stopping) break;
            if (!ready) {
                if (http == 401 || http == 403) {
                    setStage("key_rejected", "中心未接受这把 BDC 骰娘 API Key（HTTP " + std::to_string(http) + "）");
                    DICE_LOG_WARN_M("内置登录", "账号 {}：中心拒绝了 API Key，不再重试。", params.adapterId);
                    break;
                }
                setStage("backoff", failure);
                DICE_LOG_WARN_M("内置登录", "账号 {}：{}，{} 秒后重试。", params.adapterId, failure, backoff);
                sleepFor(backoff);
                backoff = (std::min)(backoff * 2, kBackoffMax);
                continue;
            }
            {
                std::lock_guard lock(mutex);
                version = manifest.version;
            }
            {
                std::ofstream config(dir / "Config.json", std::ios::binary | std::ios::trunc);
                const bool reverse = params.transport == kTransportReverse;
                config << buildConfig(manifest, params.apiKey, params.account,
                                      reverse ? params.wsPort : 0, reverse ? params.token : std::string()).dump(2);
                if (!config) {
                    setStage("failed", "无法写入 openopq 配置");
                    break;
                }
            }
            killStale();
            rotateLog();
            {
                std::lock_guard lock(mutex);
                stage = "running";
                error.clear();
                ++restarts;
            }
            {
                std::lock_guard lock(statusCacheMutex);
                cachedConsole = nullptr;
            }
            DICE_LOG_INFO_M("内置登录", "账号 {}：启动 openopq {}（管理端口 {}）。",
                            params.adapterId, manifest.version, params.consolePort);
            const auto startedAt = std::chrono::steady_clock::now();
            const auto result = dice::proc::runPathsCancellable(
                pathText(binary),
                {"--config", fs::absolute(dir / "Config.json"), "--port", std::to_string(params.consolePort),
                 "--data-dir", fs::absolute(dir / "data"), "--account", params.account,
                 "--headless", "--quiet", "--log-file", fs::absolute(dir / "openopq.log")},
                [this] { return stopping.load(); }, 16 * 1024, true, dir);
            if (stopping) break;
            if (std::chrono::steady_clock::now() - startedAt > std::chrono::minutes(10)) backoff = kBackoffFirst;
            std::string message = "openopq 已退出（代码 " + std::to_string(result.exitCode) + "）";
            const std::string output = tail(result.output, 300);
            if (!output.empty()) message += "：" + output;
            setStage("backoff", message);
            DICE_LOG_WARN_M("内置登录", "账号 {}：{}，{} 秒后重启。", params.adapterId, message, backoff);
            sleepFor(backoff);
            backoff = (std::min)(backoff * 2, kBackoffMax);
        }
    }

    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::mutex consoleMutex;
    std::string stage = "idle";
    std::string error;
    std::string version;
    int restarts = 0;
    bool alive = false;
    fs::path downloadPath;
    std::uint64_t downloadTotal = 0;
};

// ─── Runtime ─────────────────────────────────────────────────

Runtime& Runtime::instance() {
    static Runtime runtime;
    return runtime;
}

fs::path Runtime::adapterDir(const std::string& adapterId) {
    return fs::path("data") / "openopq" / adapterId;
}

std::shared_ptr<Instance> Runtime::find(const std::string& adapterId) {
    std::lock_guard lock(mutex_);
    auto it = instances_.find(adapterId);
    return it == instances_.end() ? nullptr : it->second;
}

void Runtime::launch(const LaunchParams& params) {
    if (params.adapterId.empty() ||
        !std::all_of(params.adapterId.begin(), params.adapterId.end(), [](unsigned char c) { return std::isdigit(c); }))
        return;
    std::shared_ptr<Instance> previous;
    {
        std::lock_guard lock(mutex_);
        auto it = instances_.find(params.adapterId);
        if (it != instances_.end()) {
            const auto& p = it->second->params;
            const bool same = p.apiKey == params.apiKey && p.account == params.account &&
                p.consolePort == params.consolePort && p.transport == params.transport &&
                p.wsPort == params.wsPort && p.token == params.token;
            if (same && it->second->running()) return;
            previous = it->second;
            instances_.erase(it);
        }
    }
    if (previous) previous->stop();
    auto instance = std::make_shared<Instance>(params);
    {
        std::lock_guard lock(mutex_);
        instances_[params.adapterId] = instance;
    }
    instance->start();
}

void Runtime::stop(const std::string& adapterId) {
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(mutex_);
        auto it = instances_.find(adapterId);
        if (it == instances_.end()) return;
        instance = it->second;
        instances_.erase(it);
    }
    instance->stop();
}

void Runtime::stopAll() {
    std::unordered_map<std::string, std::shared_ptr<Instance>> all;
    {
        std::lock_guard lock(mutex_);
        all.swap(instances_);
    }
    for (auto& [id, instance] : all) instance->stop();
}

void Runtime::forget(const std::string& adapterId) {
    stop(adapterId);
    if (adapterId.empty() ||
        !std::all_of(adapterId.begin(), adapterId.end(), [](unsigned char c) { return std::isdigit(c); }))
        return;
    std::error_code ec;
    fs::remove_all(adapterDir(adapterId), ec);
}

json Runtime::status(const std::string& adapterId) {
    auto instance = find(adapterId);
    if (!instance) return json{{"stage", "stopped"}, {"error", ""}, {"console", nullptr}};
    json out = instance->snapshot();
    out["console"] = nullptr;
    if (!instance->isRunningStage()) return out;
    {
        std::lock_guard lock(instance->statusCacheMutex);
        if (!instance->cachedConsole.is_null() &&
            std::chrono::steady_clock::now() - instance->cachedAt < std::chrono::seconds(1)) {
            out["console"] = instance->cachedConsole;
            return out;
        }
    }
    int http = 0;
    json reply;
    std::string failure;
    if (!instance->console("GET", "status", nullptr, http, reply, failure) || http != 200) {
        // 进程刚起来、管理接口还没开时会走到这里，不算错误。
        out["consoleError"] = failure.empty() ? "HTTP " + std::to_string(http) : failure;
        return out;
    }
    json console{
        {"phase", reply.value("phase", std::string())},
        {"account", reply.value("account", std::string())},
        {"online", reply.value("online", false)},
        {"nickname", reply.value("nickname", std::string())},
        {"error", reply.value("error", std::string())},
        {"notice", reply.value("notice", std::string())},
        {"qr", reply.value("qr", std::string())},
        {"qrExpires", reply.contains("qr_expires") ? reply["qr_expires"] : json(nullptr)},
        {"appVersion", reply.value("app_version", std::string())},
    };
    {
        std::lock_guard lock(instance->statusCacheMutex);
        instance->cachedConsole = console;
        instance->cachedAt = std::chrono::steady_clock::now();
    }
    out["console"] = std::move(console);
    return out;
}

namespace {
bool control(const std::shared_ptr<Instance>& instance, const std::string& path, const json& body,
             std::string& error) {
    if (!instance || !instance->isRunningStage()) {
        error = "openopq 尚未运行";
        return false;
    }
    int http = 0;
    json reply;
    if (!instance->console("POST", path, body, http, reply, error)) return false;
    {
        std::lock_guard lock(instance->statusCacheMutex);
        instance->cachedConsole = nullptr;
    }
    if (http >= 200 && http < 300) return true;
    error = reply.value("error", "openopq 返回 HTTP " + std::to_string(http));
    return false;
}
}  // namespace

bool Runtime::beginLogin(const std::string& adapterId, std::string& error) {
    auto instance = find(adapterId);
    return control(instance, "login", json{{"account", instance ? instance->params.account : std::string()}}, error);
}

bool Runtime::cancelLogin(const std::string& adapterId, std::string& error) {
    return control(find(adapterId), "cancel", json::object(), error);
}

bool Runtime::disconnect(const std::string& adapterId, std::string& error) {
    return control(find(adapterId), "disconnect", json::object(), error);
}

}  // namespace dice::openopq
