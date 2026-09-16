#pragma once

// 头像字节比对：判断一个 QQ 官方 OpenID 背后是不是某个真实 QQ 号。
//
// # 为什么能这么比
//
// 腾讯对同一个账号的头像，在 QQ 号端点和机器人 OpenID 端点上返回的是同一份
// 文件，不是各自重新编码的两张图：
//
//     QQ 号    https://q1.qlogo.cn/g?b=qq&nk={qq}&s=40
//     OpenID   https://q.qlogo.cn/qqapp/{appId}/{openId}/40
//
// 字节一致，就说明两个标识指向同一个账号。而同一张图片被不同 QQ 号设为头像时
// 字节并不相同（已实测），所以「把别人的头像另存再上传」这条冒认路径走不通——
// 这是本机制敢当验证用的前提。
//
// # 为什么取 40
//
// 比对只看字节是否相同，不看画质，取两个端点都提供的最小档，一张一千多字节。
//
// # 两类共用头像必须挡掉
//
// 用户没设过头像（默认头像），或者用的是商城头像时，这张图是腾讯统一下发的，
// 多人共用同一份字节，比对就失去意义——两个都用着同一张商城头像的人会互相验证
// 成功。
//
//   · 默认头像：每次比对顺带探一个必定不存在的 OpenID，当场问出这一刻的默认图
//     哈希。写死一串哈希的话，腾讯换图那天名单会安静失效，探针不会。
//   · 商城头像：没法凭一张图自己看出来。改为记住每个通过验证的哈希属于谁，同一
//     个哈希再出现在另一个身份上就拒绝——第一次撞车就被拦下，同样不用维护名单。
//
// 两种都提示用户换一张自己的图片再验证：换过之后哈希就是这个账号独有的。
//
// ⚠️ 同步阻塞（含 curl）。三张图并进一次 curl 调用并收紧超时，最坏几秒；与
// `.log` 上传日志站同属指令内的网络操作，但别在更热的路径上复用。

#include "../../common/subprocess.h"
#include "../../storage/database.h"

#include <openssl/evp.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

namespace dice::identity {

enum class AvatarProof {
    kMatched,            // 两端字节一致，且不是共用头像
    kMismatch,           // 两端都取到了，但不是同一张
    kOpenIdUnavailable,  // OpenID 侧没取到图
    kQQUnavailable,      // QQ 侧没取到图
    kProbeFailed,        // 默认头像探针没取到，无从判断是否为默认图
    kSharedAvatar,       // 默认头像 / 商城头像，多人共用
    kBadParameter,
};

struct AvatarProofResult {
    AvatarProof status = AvatarProof::kBadParameter;
    std::string openIdSha256;
    std::string qqSha256;
    bool ok() const { return status == AvatarProof::kMatched; }
};

inline std::string avatarSha256(const std::string& data) {
    if (data.empty()) return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
        && EVP_DigestUpdate(ctx, data.data(), data.size()) == 1
        && EVP_DigestFinal_ex(ctx, digest, &length) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return {};
    std::ostringstream out;
    for (unsigned int i = 0; i < length; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return out.str();
}

inline std::string qqAvatarUrl(const std::string& qq, int size = 40) {
    return "https://q1.qlogo.cn/g?b=qq&nk=" + qq + "&s=" + std::to_string(size);
}

inline std::string openIdAvatarUrl(const std::string& appId, const std::string& openId, int size = 40) {
    return "https://q.qlogo.cn/qqapp/" + appId + "/" + openId + "/" + std::to_string(size);
}

/// 一个必定没有对应账号的 OpenID，用来问出当前的默认头像。
/// OpenID 是 32 位十六进制；每次换一个，既撞不上真实账号，也不会被缓存成特例。
inline std::string defaultAvatarProbeId() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (int i = 0; i < 32; ++i) out += hex[rng() & 15];
    return out;
}

/// 纯判定：给定三段字节得出结论，不碰网络，便于单测把每条分支钉住。
inline AvatarProofResult classifyAvatars(const std::string& probe,
                                         const std::string& subject,
                                         const std::string& target) {
    AvatarProofResult result;
    if (subject.empty()) { result.status = AvatarProof::kOpenIdUnavailable; return result; }
    if (target.empty())  { result.status = AvatarProof::kQQUnavailable;     return result; }
    // 探针取不到时不要放行：少了这道闸，默认头像会被当成一张普通头像去比。
    if (probe.empty())   { result.status = AvatarProof::kProbeFailed;       return result; }

    result.openIdSha256 = avatarSha256(subject);
    result.qqSha256 = avatarSha256(target);
    if (result.openIdSha256.empty() || result.qqSha256.empty()) {
        result.status = AvatarProof::kProbeFailed;
        return result;
    }
    if (result.openIdSha256 == avatarSha256(probe)) {
        result.status = AvatarProof::kSharedAvatar;
        return result;
    }
    result.status = result.openIdSha256 == result.qqSha256
        ? AvatarProof::kMatched : AvatarProof::kMismatch;
    return result;
}

namespace detail {

inline std::string readAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

/// 三张图一次 curl 取回：URL 走 -K 配置文件而不是命令行，既防注入也只起一个进程。
inline bool fetchAvatars(const std::string& probeUrl, const std::string& subjectUrl,
                         const std::string& targetUrl, std::string& probe,
                         std::string& subject, std::string& target) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "dice-avatar-proof";
    std::filesystem::create_directories(dir, ec);
    const auto stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto probePath = dir / (stamp + "-probe.jpg");
    const auto subjectPath = dir / (stamp + "-openid.jpg");
    const auto targetPath = dir / (stamp + "-qq.jpg");
    const auto configPath = dir / (stamp + ".curlcfg");

    {
        std::ofstream cfg(configPath, std::ios::binary);
        if (!cfg) return false;
        const auto entry = [&cfg](const std::string& url, const std::filesystem::path& out) {
            cfg << "url = \"" << url << "\"\noutput = \"" << out.generic_string() << "\"\n";
        };
        entry(probeUrl, probePath);
        entry(subjectUrl, subjectPath);
        entry(targetUrl, targetPath);
        cfg << "--max-time 4\n--connect-timeout 2\n--silent\n--fail\n--location\n";
    }

    dice::proc::curlConfig(configPath);
    probe = readAll(probePath);
    subject = readAll(subjectPath);
    target = readAll(targetPath);
    for (const auto& path : {configPath, probePath, subjectPath, targetPath})
        std::filesystem::remove(path, ec);
    return true;
}

}   // namespace detail

/// 这张头像是不是已经被别的身份用过。是的话说明它多人共用（商城头像是典型），
/// 证明不了任何事——提示用户换一张自己的图片再验证。
inline bool avatarClaimedByOther(Database& db, const std::string& sha, const std::string& appId,
                                 const std::string& openId, const std::string& qq) {
    auto* storage = db.getStorage();
    if (!storage || sha.empty()) return false;
    try {
        for (const auto& row : storage->get_all<AvatarProofRow>(
                 orm::where(orm::c(&AvatarProofRow::avatarSha256) == sha))) {
            // 同一个人重复验证不算撞车。
            if (row.adapterAccount == appId && row.openId == openId && row.qq == qq) continue;
            return true;
        }
    } catch (...) {}
    return false;
}

/// 记下这次通过验证的哈希归属，供下次撞车检测使用。
inline void rememberAvatarProof(Database& db, const std::string& sha, const std::string& appId,
                                const std::string& openId, const std::string& qq) {
    auto* storage = db.getStorage();
    if (!storage || sha.empty()) return;
    try {
        const auto existing = storage->get_all<AvatarProofRow>(
            orm::where(orm::c(&AvatarProofRow::avatarSha256) == sha
                       and orm::c(&AvatarProofRow::adapterAccount) == appId
                       and orm::c(&AvatarProofRow::openId) == openId),
            orm::limit(1));
        if (!existing.empty()) return;
        AvatarProofRow row;
        row.avatarSha256 = sha;
        row.adapterAccount = appId;
        row.openId = openId;
        row.qq = qq;
        // 与 identity_endpoints 的 created_at 保持同一种写法：Unix 秒的十进制串。
        row.createdAt = std::to_string(static_cast<long long>(std::time(nullptr)));
        storage->insert(row);
    } catch (...) {}
}

/// 完整流程：抓图 → 判定。调用方还需自行确认这个哈希没被别的身份用过（商城头像）。
inline AvatarProofResult proveOpenIdIsQQ(const std::string& appId, const std::string& openId,
                                         const std::string& qq) {
    AvatarProofResult result;
    if (appId.empty() || openId.empty() || qq.empty()) return result;
    std::string probe, subject, target;
    if (!detail::fetchAvatars(openIdAvatarUrl(appId, defaultAvatarProbeId()),
                              openIdAvatarUrl(appId, openId), qqAvatarUrl(qq),
                              probe, subject, target)) {
        result.status = AvatarProof::kProbeFailed;
        return result;
    }
    return classifyAvatars(probe, subject, target);
}

}   // namespace dice::identity
