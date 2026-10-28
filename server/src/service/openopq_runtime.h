#pragma once

// 内置登录（实验性）：openopq 协议端当作库来用。
//
// 协议端不随配布包分发。用户新建「内置登录」账号时，DN 带着
// BDC 骰娘 API Key 向中心取清单和执行档——没有 Key 中心不给下载，签名服务同样
// 认这把 Key。执行档放进 runtime/openopq/<版本>/，按账号拉起一个进程，
// 自动写好 Config.json（签名地址、默认正向 WS 连接），二维码经它本机的管理接口
// 取回，直接显示在 WebUI 里。
//
// 清单使用 Ed25519 验签，执行档校验大小与 SHA-256；安全性依赖签名私钥妥善保管。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dice::openopq {

struct Asset {
    std::string os;
    std::string arch;
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Manifest {
    std::string version;      // openopq 自己的版本，如 v1.3.0-rc.1
    std::string qqVersion;    // Config.json 的 Version，须与签名服务对上
    std::string signUrl;      // 含 {key} 占位
    std::vector<std::string> cdnLists;
    std::vector<Asset> assets;
};

/// BDC 骰娘 API Key：bdc_ 开头，只含字母数字、-、_。它要拼进签名地址的路径。
bool validApiKey(const std::string& key);
/// QQ 号：5~12 位数字。
bool validAccount(const std::string& account);

/// 校验并解析中心返回的清单：{"payload":"<清单 JSON 文本>","signature":"<base64>"}。
/// 签名是对 payload 原文字节的 Ed25519 签名。
bool parseSignedManifest(const std::string& body, const std::string& publicKeyBase64,
                         Manifest& out, std::string& error);
const Asset* assetFor(const Manifest& manifest, const std::string& os, const std::string& arch);
std::string signUrlFor(const Manifest& manifest, const std::string& apiKey);
/// 给 openopq 的 Config.json：只开 OneBot v11。
/// 默认 DN 经它端口上的正向 WS 连过去；``reverseWsPort`` 大于 0 时另写一条反向 WS，
/// 让 openopq 连回 DN（要客户端支持，v1.3.0-rc.1 不支持）。
nlohmann::json buildConfig(const Manifest& manifest, const std::string& apiKey,
                           const std::string& account, int reverseWsPort = 0,
                           const std::string& reverseToken = {});

/// 内置登录账号与 openopq 之间的连接方式
inline constexpr const char* kTransportForward = "forward_ws";
inline constexpr const char* kTransportReverse = "reverse_ws";
inline bool validTransport(const std::string& value) {
    return value == kTransportForward || value == kTransportReverse;
}

/// 本机回环上一个当前空闲的端口；取不到返回 0。
int pickFreeLoopbackPort();
/// 32 位十六进制随机串，用作反向 WS 的令牌。
std::string randomToken();

struct LaunchParams {
    std::string adapterId;
    std::string apiKey;
    std::string account;
    int consolePort = 0;
    std::string transport = kTransportForward;
    //: 反向 WS 用：DN 监听的本机端口与握手令牌。建账号时就分配好，切换连接方式不必重建
    int wsPort = 0;
    std::string token;
};

class Instance;

class Runtime {
public:
    static Runtime& instance();

    /// 幂等：已在运行就什么都不做；失败停下的会重新开始。
    void launch(const LaunchParams& params);
    void stop(const std::string& adapterId);
    void stopAll();
    /// 停掉并删除该账号的 openopq 目录（设备身份、日志）。删除账号时用。
    void forget(const std::string& adapterId);

    nlohmann::json status(const std::string& adapterId);
    bool beginLogin(const std::string& adapterId, std::string& error);
    bool cancelLogin(const std::string& adapterId, std::string& error);
    bool disconnect(const std::string& adapterId, std::string& error);

    static std::filesystem::path adapterDir(const std::string& adapterId);

private:
    std::shared_ptr<Instance> find(const std::string& adapterId);

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Instance>> instances_;
};

}  // namespace dice::openopq
