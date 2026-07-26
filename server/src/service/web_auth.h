#pragma once
// ─── Dice!Next — WebUI 登录鉴权 ───────────────────────
// 极简的「共享口令 + 会话 token」鉴权：设了口令就要求登录，登录成功颁发随机
// token，写进 Cookie(dice_session)。浏览器同源请求会自动带 Cookie，所以前端
// 各处的 fetch 无需逐个改。token 存内存，重启即失效（可接受，局域网管理面板）。

#include <string>
#include <set>
#include <mutex>
#include <random>

namespace dice {

class WebAuth {
public:
    static WebAuth& instance() { static WebAuth a; return a; }

    void setPassword(const std::string& p) {
        std::lock_guard<std::mutex> lk(m_);
        password_ = p;
        tokens_.clear();   // 改/清口令都让现有会话失效
    }
    bool hasPassword() const { std::lock_guard<std::mutex> lk(m_); return !password_.empty(); }
    bool checkPassword(const std::string& pw) const {
        std::lock_guard<std::mutex> lk(m_);
        return !password_.empty() && pw == password_;
    }

    std::string issueToken() {
        std::lock_guard<std::mutex> lk(m_);
        std::string t = randomToken();
        tokens_.insert(t);
        return t;
    }
    // 未设口令 → 视为始终通过（鉴权关闭）。
    bool validToken(const std::string& t) const {
        std::lock_guard<std::mutex> lk(m_);
        if (password_.empty()) return true;
        return !t.empty() && tokens_.count(t) > 0;
    }
    void revoke(const std::string& t) {
        std::lock_guard<std::mutex> lk(m_);
        tokens_.erase(t);
    }

private:
    static std::string randomToken() {
        static const char* hex = "0123456789abcdef";
        std::random_device rd;
        std::mt19937_64 g(rd() ^ (uint64_t)std::random_device{}() << 1);
        std::string s; s.reserve(40);
        for (int i = 0; i < 40; ++i) s += hex[g() & 0xF];
        return s;
    }
    mutable std::mutex m_;
    std::string password_;
    std::set<std::string> tokens_;
};

}  // namespace dice
