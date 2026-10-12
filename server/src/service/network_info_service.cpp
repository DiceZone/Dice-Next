#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include "network_info_service.h"
#include "../common/logger.h"
#include "../common/subprocess.h"
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace dice {
namespace {
int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
bool ipv4(const std::string& value, std::array<int, 4>& parts) {
    if (value.empty() || value.size() > 15) return false;
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        const auto begin = at; int part = 0;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
            part = part * 10 + value[at++] - '0';
            if (at - begin > 3 || part > 255) return false;
        }
        if (at == begin || (at - begin > 1 && value[begin] == '0')) return false;
        parts[i] = part;
        if (i < 3 && (at == value.size() || value[at++] != '.')) return false;
    }
    return at == value.size() && parts[0] > 0 && parts[0] != 127 && parts[0] < 224
        && !(parts[0] == 169 && parts[1] == 254);
}
bool privateIp(const std::array<int, 4>& p) {
    return p[0] == 10 || (p[0] == 172 && p[1] >= 16 && p[1] <= 31)
        || (p[0] == 192 && p[1] == 168) || (p[0] == 100 && p[1] >= 64 && p[1] <= 127);
}
std::string addressBytes(const void* value) {
    const auto* b = static_cast<const unsigned char*>(value);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
}
} // namespace
nlohmann::json NetworkInfoService::Snapshot::json() const {
    const auto record = [](const AddressState& item) {
        return nlohmann::json{{"status", item.status}, {"addresses", item.addresses},
            {"stale", item.stale}, {"checked_at", item.checkedAt}};
    };
    return {{"lan", record(lan)}, {"wan", record(wan)}, {"refreshing", refreshing}, {"attempted_at", attemptedAt}};
}
NetworkInfoService::NetworkInfoService(Detector local, Detector publicIp, std::chrono::milliseconds refreshEvery)
    : local_(local ? std::move(local) : localAddresses),
      public_(publicIp ? std::move(publicIp) : publicAddress), refreshEvery_(refreshEvery) {}
NetworkInfoService::~NetworkInfoService() { stop(); }
void NetworkInfoService::start() {
    std::lock_guard lock(mutex_);
    if (started_ || stopping_) return;
    started_ = true; requested_ = true; state_.refreshing = true;
    for (auto* item : {&state_.lan, &state_.wan}) if (item->addresses.empty()) item->status = "detecting";
    try { worker_ = std::thread([this] { run(); }); }
    catch (...) {
        started_ = false; requested_ = false; state_.refreshing = false;
        state_.lan.status = state_.wan.status = "failed";
        DICE_LOG_WARN_EXCEPTION("无法启动后台地址检测，连接信息中可手动填写地址");
    }
}
void NetworkInfoService::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    state_.refreshing = false;
    for (auto* item : {&state_.lan, &state_.wan}) if (item->status == "detecting") item->status = "failed";
}
bool NetworkInfoService::refresh() {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || state_.refreshing) return false;
    requested_ = true; state_.refreshing = true;
    for (auto* item : {&state_.lan, &state_.wan}) if (item->addresses.empty()) item->status = "detecting";
    changed_.notify_one(); return true;
}
NetworkInfoService::Snapshot NetworkInfoService::snapshot() const {
    std::lock_guard lock(mutex_); return state_;
}
std::vector<std::string> NetworkInfoService::normalize(std::vector<std::string> values, bool publicOnly) {
    std::vector<std::string> out; std::set<std::string> seen;
    for (auto& value : values) {
        value = trim(std::move(value)); std::array<int, 4> parts{};
        if (!ipv4(value, parts) || (publicOnly && privateIp(parts)) || !seen.insert(value).second) continue;
        out.push_back(std::move(value)); if (out.size() == 32) break;
    }
    if (!publicOnly) std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        std::array<int, 4> p{}, q{}; ipv4(a, p); ipv4(b, q);
        return privateIp(p) && !privateIp(q);
    });
    if (publicOnly && out.size() > 1) out.resize(1);
    return out;
}
std::vector<std::string> NetworkInfoService::localAddresses(const Cancelled& cancelled) {
    std::vector<std::string> values;
    if (cancelled()) return values;
#ifdef _WIN32
    ULONG length = 16384; ULONG result = ERROR_BUFFER_OVERFLOW;
    std::vector<unsigned char> buffer;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        if (length > 4 * 1024 * 1024) throw std::runtime_error("网卡信息超出合理大小");
        buffer.resize(length);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &length);
    }
    if (result == ERROR_NO_DATA) return values;
    if (result != NO_ERROR) throw std::runtime_error("无法枚举活动网卡，系统错误 " + std::to_string(result));
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter && !cancelled(); adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
            if (!item->Address.lpSockaddr || item->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* addr = reinterpret_cast<const sockaddr_in*>(item->Address.lpSockaddr);
            values.push_back(addressBytes(&addr->sin_addr));
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) throw std::runtime_error("无法枚举活动网卡");
    struct Release { ifaddrs* value; ~Release() { freeifaddrs(value); } } release{interfaces};
    for (auto* item = interfaces; item && !cancelled(); item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* addr = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        values.push_back(addressBytes(&addr->sin_addr));
    }
#endif
    return normalize(std::move(values));
}
std::vector<std::string> NetworkInfoService::publicAddress(const Cancelled& cancelled) {
    // Documented fixed HTTPS endpoints: www.ipify.org and AWS's checkip service.
    // No user data, credentials, proxy, curlrc or redirects. Cancel also reaps
    // the child if DNS/curl hangs. No HTTP/WebSocket listener is used.
#ifdef _WIN32
    const auto curl = proc::systemTool("curl.exe");
    if (!std::filesystem::path(curl).is_absolute()) return {}; // never run a cwd/PATH impostor
#else
    const auto curl = proc::systemTool("curl");
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    const auto stop = [&] { return cancelled() || std::chrono::steady_clock::now() >= deadline; };
    for (const auto* url : {"https://api.ipify.org", "https://checkip.amazonaws.com"}) {
        if (stop()) break;
        const auto result = proc::runCancellable(curl,
            {"--disable", "--ipv4", "--silent", "--fail", "--proto", "=https", "--noproxy", "*",
             "--connect-timeout", "2", "--max-time", "3", url}, stop, 128);
        if (!result.ok() || result.truncated || result.cancelled) continue;
        auto addresses = normalize({result.output}, true);
        if (!addresses.empty()) return addresses;
    }
    return {};
}
void NetworkInfoService::detect(bool publicIp) {
    std::vector<std::string> addresses;
    try { addresses = normalize((publicIp ? public_ : local_)([this] { return stopping_.load(); }), publicIp); }
    catch (...) { DICE_LOG_WARN_EXCEPTION(publicIp ? "服务器公网地址检测未完成" : "服务器网卡地址检测未完成"); }
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    auto& item = publicIp ? state_.wan : state_.lan;
    if (!addresses.empty()) {
        item.addresses = std::move(addresses); item.status = "ready";
        item.stale = false; item.checkedAt = nowMillis();
    } else {
        item.status = "failed"; item.stale = !item.addresses.empty();
    }
}
void NetworkInfoService::run() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (refreshEvery_.count() > 0) changed_.wait_for(lock, refreshEvery_, [this] { return requested_ || stopping_; });
        else changed_.wait(lock, [this] { return requested_ || stopping_; });
        if (stopping_) break;
        requested_ = false; state_.refreshing = true; state_.attemptedAt = nowMillis();
        for (auto* item : {&state_.lan, &state_.wan}) if (item->addresses.empty()) item->status = "detecting";
        lock.unlock();
        detect(false); // LAN is available independently of a slow/offline WAN check
        if (!stopping_) detect(true);
        lock.lock(); state_.refreshing = false;
    }
}
} // namespace dice
