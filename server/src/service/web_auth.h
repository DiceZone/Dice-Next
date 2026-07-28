#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <random>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace dice {

// Shared WebUI password plus browser sessions. A normal session is memory-only
// and expires with the browser; a trusted device is persisted for 30 days in
// config/webui_sessions.json so a program restart does not log it out.
class WebAuth {
public:
    static WebAuth& instance() { static WebAuth a; return a; }

    void configure(const std::string& password, const std::filesystem::path& sessionsPath) {
        std::lock_guard<std::mutex> lk(m_);
        password_ = password; sessionsPath_ = sessionsPath; tokens_.clear(); loadTrustedLocked();
    }
    void setPassword(const std::string& password) {
        std::lock_guard<std::mutex> lk(m_);
        password_ = password; tokens_.clear(); saveTrustedLocked();
    }
    bool hasPassword() const { std::lock_guard<std::mutex> lk(m_); return !password_.empty(); }
    bool checkPassword(const std::string& pw) const { std::lock_guard<std::mutex> lk(m_); return !password_.empty() && pw == password_; }

    std::string issueToken(bool trustDevice = false) {
        std::lock_guard<std::mutex> lk(m_);
        const std::string token = randomToken();
        tokens_[token] = trustDevice ? std::time(nullptr) + 30 * 24 * 3600 : 0;
        if (trustDevice) saveTrustedLocked();
        return token;
    }
    bool validToken(const std::string& token) const {
        std::lock_guard<std::mutex> lk(m_);
        if (password_.empty()) return true;
        const auto it = tokens_.find(token);
        return it != tokens_.end() && (it->second == 0 || it->second > std::time(nullptr));
    }
    void revoke(const std::string& token) {
        std::lock_guard<std::mutex> lk(m_); tokens_.erase(token); saveTrustedLocked();
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
            std::ifstream in(sessionsPath_); nlohmann::json j; in >> j;
            const auto now = std::time(nullptr);
            for (const auto& item : j.value("trusted", nlohmann::json::array())) {
                const std::string token = item.value("token", std::string());
                const std::time_t expires = item.value("expires_at", std::time_t(0));
                if (!token.empty() && expires > now) tokens_[token] = expires;
            }
            saveTrustedLocked();
        } catch (...) {}
    }
    void saveTrustedLocked() const {
        if (sessionsPath_.empty()) return;
        try {
            nlohmann::json trusted = nlohmann::json::array(); const auto now = std::time(nullptr);
            for (const auto& [token, expires] : tokens_) if (expires > now) trusted.push_back({{"token", token}, {"expires_at", expires}});
            std::filesystem::create_directories(sessionsPath_.parent_path());
            std::ofstream out(sessionsPath_, std::ios::trunc); out << nlohmann::json{{"trusted", trusted}}.dump(2);
        } catch (...) {}
    }
    mutable std::mutex m_;
    std::string password_;
    std::filesystem::path sessionsPath_;
    std::unordered_map<std::string, std::time_t> tokens_; // 0 = browser session only
};

} // namespace dice
