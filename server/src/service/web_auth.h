#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <random>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <sstream>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>

namespace dice {

// Shared WebUI password plus browser sessions. A normal session is memory-only
// and expires with the browser; a trusted device is persisted for 30 days in
// config/webui_sessions.json so a program restart does not log it out.
class WebAuth {
public:
    static WebAuth& instance() { static WebAuth a; return a; }

    void configure(const std::string& password, const std::filesystem::path& sessionsPath,
                   const std::string& apiKey = std::string()) {
        std::lock_guard<std::mutex> lk(m_);
        password_ = password;
        apiKey_ = apiKey;
        sessionsPath_ = sessionsPath;
        tokens_.clear();
        loadTrustedLocked();
    }
    void setPassword(const std::string& password) {
        std::lock_guard<std::mutex> lk(m_);
        password_ = hashPassword(password); tokens_.clear(); saveTrustedLocked();
    }
    bool hasPassword() const { std::lock_guard<std::mutex> lk(m_); return !password_.empty(); }
    bool checkPassword(const std::string& pw) const { std::lock_guard<std::mutex> lk(m_); return !password_.empty() && verifyPassword(password_, pw); }

    std::string issueToken(bool trustDevice = false, bool* trustedPersisted = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        const std::string token = randomToken();
        tokens_[token] = trustDevice ? std::time(nullptr) + 30 * 24 * 3600 : 0;
        const bool persisted = !trustDevice || saveTrustedLocked();
        if (!persisted) tokens_.erase(token);
        if (trustedPersisted) *trustedPersisted = persisted;
        return token;
    }
    bool validToken(const std::string& token) const {
        std::lock_guard<std::mutex> lk(m_);
        if (password_.empty()) return false;   // 未设口令 = 无有效会话（前端走 setup 流程）
        const auto it = tokens_.find(token);
        return it != tokens_.end() && (it->second == 0 || it->second > std::time(nullptr));
    }
    /// X-API-Key 校验（服务间/脚本调用凭据，与 server/api_key 常量时间比较）。
    bool checkApiKey(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        if (apiKey_.empty() || apiKey_.size() != key.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < apiKey_.size(); ++i)
            diff |= static_cast<unsigned char>(apiKey_[i]) ^ static_cast<unsigned char>(key[i]);
        return diff == 0;
    }
    void revoke(const std::string& token) {
        std::lock_guard<std::mutex> lk(m_); tokens_.erase(token); saveTrustedLocked();
    }

    /// PBKDF2-SHA256 加盐哈希（格式 pbkdf2$salt$hash）。空口令原样返回（=未设置）。
    static std::string hashPassword(const std::string& pw) {
        if (pw.empty()) return pw;
        unsigned char salt[16];
        if (RAND_bytes(salt, sizeof(salt)) != 1) return pw;   // 随机源故障时退回明文（等价旧行为）
        unsigned char out[32];
        PKCS5_PBKDF2_HMAC(pw.c_str(), static_cast<int>(pw.size()), salt, sizeof(salt),
                          120000, EVP_sha256(), sizeof(out), out);
        std::ostringstream s; s << "pbkdf2$";
        auto hex = [&s](const unsigned char* b, size_t n) {
            for (size_t i = 0; i < n; ++i) s << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b[i]);
        };
        hex(salt, sizeof(salt)); s << '$'; hex(out, sizeof(out));
        return s.str();
    }
    /// 校验口令：新格式哈希 + 旧明文兼容（迁移期）。
    static bool verifyPassword(const std::string& stored, const std::string& pw) {
        if (stored.empty()) return pw.empty();
        if (stored.rfind("pbkdf2$", 0) != 0) return pw == stored;   // 旧明文（尚未升级）
        const size_t d1 = stored.find('$', 7);
        const size_t d2 = d1 == std::string::npos ? std::string::npos : stored.find('$', d1 + 1);
        if (d1 == std::string::npos || d2 == std::string::npos) return false;
        auto unhex = [](const std::string& h) {
            std::string b; int v = 0;
            for (size_t i = 0; i + 1 < h.size(); i += 2) {
                std::istringstream ss(h.substr(i, 2)); ss >> std::hex >> v; b += static_cast<char>(v);
            }
            return b;
        };
        const std::string salt = unhex(stored.substr(d1 + 1, d2 - d1 - 1));
        const std::string hash = unhex(stored.substr(d2 + 1));
        if (salt.empty() || hash.size() != 32) return false;
        unsigned char out[32];
        PKCS5_PBKDF2_HMAC(pw.c_str(), static_cast<int>(pw.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
                          120000, EVP_sha256(), sizeof(out), out);
        unsigned char diff = 0;
        for (size_t i = 0; i < sizeof(out); ++i) diff |= out[i] ^ static_cast<unsigned char>(hash[i]);
        return diff == 0;
    }

private:
    static std::string randomToken() {
        static const char* hex = "0123456789abcdef";
        std::random_device rd; std::mt19937_64 g(rd() ^ (uint64_t)std::random_device{}() << 1);
        std::string s; s.reserve(40); for (int i = 0; i < 40; ++i) s += hex[g() & 0xF]; return s;
    }
    void loadTrustedLocked() {
        if (sessionsPath_.empty()) return;
        try {
            std::ifstream in(sessionsPath_, std::ios::binary);
            if (!in) return;
            nlohmann::json j; in >> j;
            const auto now = std::time(nullptr);
            for (const auto& item : j.value("trusted", nlohmann::json::array())) {
                const std::string token = item.value("token", std::string());
                const std::time_t expires = item.value("expires_at", std::time_t(0));
                if (!token.empty() && expires > now) tokens_[token] = expires;
            }
            // Prune expired entries after loading. This writes atomically, so a
            // crash or abrupt restart cannot leave a truncated sessions file.
            saveTrustedLocked();
        } catch (...) {}
    }
    bool saveTrustedLocked() const {
        if (sessionsPath_.empty()) return true;
        try {
            nlohmann::json trusted = nlohmann::json::array(); const auto now = std::time(nullptr);
            for (const auto& [token, expires] : tokens_) if (expires > now) trusted.push_back({{"token", token}, {"expires_at", expires}});
            std::error_code ec;
            if (!sessionsPath_.parent_path().empty()) std::filesystem::create_directories(sessionsPath_.parent_path(), ec);
            if (ec) return false;
            const auto tempPath = sessionsPath_.string() + ".tmp";
            {
                std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
                if (!out) return false;
                out << nlohmann::json{{"trusted", trusted}}.dump(2) << '\n';
                out.flush();
                if (!out) return false;
            }
            std::filesystem::remove(sessionsPath_, ec); ec.clear();
            std::filesystem::rename(tempPath, sessionsPath_, ec);
            if (ec) { std::filesystem::remove(tempPath, ec); return false; }
            return true;
        } catch (...) { return false; }
    }
    mutable std::mutex m_;
    std::string password_;
    std::string apiKey_;
    std::filesystem::path sessionsPath_;
    std::unordered_map<std::string, std::time_t> tokens_; // 0 = browser session only
};

} // namespace dice
