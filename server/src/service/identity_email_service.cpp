#include "identity_email_service.h"
#include "../common/subprocess.h"
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace dice::identity_email {
namespace {
bool clean(const std::string& s) {
    return s.size() <= 1024 && std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool mailbox(const std::string& s) {
    if (s.empty() || !clean(s)) return false;
    const auto at = s.find('@');
    return at != std::string::npos && at > 0 && at + 1 < s.size()
        && s.find('@', at + 1) == std::string::npos
        && s.find_first_of(" <>\"\\,;:") == std::string::npos;
}
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) { if (c == '\\' || c == '"') out += '\\'; out += c; }
    return out + "\"";
}
std::string base64Body(const std::string& body) {
    std::string encoded(4 * ((body.size() + 2) / 3) + 1, '\0');
    const auto n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(body.data()), static_cast<int>(body.size()));
    encoded.resize(n);
    std::string out;
    for (size_t i = 0; i < encoded.size(); i += 76) out += encoded.substr(i, 76) + "\r\n";
    return out;
}
std::string randomHex() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("random unavailable");
    std::ostringstream out;
    for (auto b : bytes) out << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return out.str();
}
std::string newCode() {
    std::uint32_t n;
    constexpr std::uint32_t range = 100000000;
    constexpr std::uint32_t limit = UINT32_MAX - UINT32_MAX % range;
    do { if (RAND_bytes(reinterpret_cast<unsigned char*>(&n), sizeof(n)) != 1)
        throw std::runtime_error("random unavailable"); } while (n >= limit);
    std::ostringstream out; out << std::setw(8) << std::setfill('0') << n % range;
    return out.str();
}
struct TempMail {
    std::filesystem::path dir;
    ~TempMail() { std::error_code ec; if (!dir.empty()) std::filesystem::remove_all(dir, ec); }
};
bool smtpSend(const Json& s, const std::string& to, const std::string& body) {
    if (!validSettings(s) || !mailbox(to)) return false;
    try {
        namespace fs = std::filesystem;
        TempMail tmp;
        const auto path = fs::temp_directory_path() / ("dicenext-identity-mail-" + randomHex());
        if (!fs::create_directory(path)) return false;
        tmp.dir = path;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
        const auto eml = path / "mail.eml", config = path / "curl.cfg";
        const auto from = s.at("from").get<std::string>();
        {
            std::ofstream out(eml, std::ios::binary);
            out << "From: <" << from << ">\r\nTo: <" << to << ">\r\n"
                << "Subject: Dice!Next QQ binding verification\r\nMIME-Version: 1.0\r\n"
                << "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" << base64Body(body);
            out.close(); if (!out) return false;
        }
        {
            std::ofstream out(config, std::ios::binary);
            const bool ssl = s.value("ssl", true);
            out << "url = " << quoted(std::string(ssl ? "smtps://" : "smtp://") + s.at("host").get<std::string>()
                + ":" + std::to_string(s.at("port").get<int>())) << "\n"
                << "mail-from = " << quoted(from) << "\nmail-rcpt = " << quoted(to) << "\n"
                << "user = " << quoted(s.at("user").get<std::string>() + ":" + s.at("pass").get<std::string>()) << "\n"
                << "upload-file = " << quoted(eml.generic_string()) << "\n"
                << "proto = \"=smtp,smtps\"\nmax-time = 15\nconnect-timeout = 5\nsilent\n";
            if (!ssl) out << "ssl-reqd\n"; // STARTTLS is mandatory; never send credentials in plaintext.
            out.close(); if (!out) return false;
        }
        for (const auto& p : {eml, config}) fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write,
                                                          fs::perm_options::replace);
#ifdef _WIN32
        const auto program = proc::systemTool("curl.exe");
#else
        const std::string program = "curl";
#endif
        // Disable ~/.curlrc: its trace or insecure flags must not affect credential delivery.
        return proc::runPaths(program, {fs::path("-q"), fs::path("-K"), config}, 1024).ok();
    } catch (...) { return false; }
}
} // namespace

bool validQQ(const std::string& qq) {
    return qq.size() >= 5 && qq.size() <= 12 && qq.front() != '0'
        && std::all_of(qq.begin(), qq.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
bool validSettings(const Json& s) {
    try {
        if (!s.is_object()) return false;
        const auto host = s.at("host").get<std::string>();
        const auto user = s.at("user").get<std::string>();
        const auto pass = s.at("pass").get<std::string>();
        const auto port = s.at("port").get<int>();
        return !host.empty() && host.size() <= 253 && host.front() != '-' && host.back() != '.'
            && std::all_of(host.begin(), host.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
            }) && port > 0 && port <= 65535 && s.at("ssl").is_boolean()
            && !user.empty() && clean(user) && user.find(':') == std::string::npos
            && !pass.empty() && clean(pass) && mailbox(s.at("from").get<std::string>());
    } catch (...) { return false; }
}
Json publicSettings(const Json& s) {
    Json out = {{"enabled", false}, {"host", ""}, {"port", 465}, {"ssl", true},
                {"user", ""}, {"from", ""}, {"password_configured", false}};
    if (s.is_object()) {
        for (const auto* key : {"enabled", "host", "port", "ssl", "user", "from"})
            if (s.contains(key)) out[key] = s[key];
        out["password_configured"] = s.contains("pass") && s["pass"].is_string() && !s["pass"].get<std::string>().empty();
    }
    return out;
}
bool updateSettings(const Json& old, const Json& input, Json& out) {
    if (!input.is_object()) return false;
    out = publicSettings(old); out.erase("password_configured");
    out["pass"] = old.is_object() ? old.value("pass", std::string()) : std::string();
    for (const auto* key : {"enabled", "host", "port", "ssl", "user", "from"})
        if (input.contains(key)) out[key] = input[key];
    if (input.contains("pass")) {
        if (!input["pass"].is_string()) return false;
        if (!input["pass"].get<std::string>().empty()) out["pass"] = input["pass"];
    }
    if (!out["enabled"].is_boolean() || !out["ssl"].is_boolean() || !out["port"].is_number_integer()) return false;
    for (const auto* key : {"host", "user", "pass", "from"})
        if (!out[key].is_string() || !clean(out[key].get<std::string>())) return false;
    return !out["enabled"].get<bool>() || validSettings(out);
}
std::string verificationBody(const std::string& qq, const std::string& code) {
    return "Dice!Next QQ 账号绑定验证码：" + code + "\r\n\r\n"
        "有人申请将其他平台账号关联到 QQ " + qq + "。\r\n"
        "验证码 10 分钟内有效，请仅在你自己发起申请的机器人私聊中输入：\r\n"
        ".bind confirm " + code + "\r\n\r\n"
        "若非本人操作，请忽略本邮件。不要把验证码发送给他人；通过验证会关联人物卡等数据。\r\n";
}
std::string statusKey(Status s) {
    switch (s) {
    case Status::Queued: return "queued";
    case Status::Sent: return "sent";
    case Status::Disabled: return "disabled";
    case Status::BadConfig: return "bad_config";
    case Status::RateLimited: return "rate_limited";
    case Status::Busy: return "busy";
    case Status::SendFailed: return "send_failed";
    case Status::NotReady: return "not_ready";
    case Status::WrongCode: return "wrong_code";
    case Status::Locked: return "locked";
    case Status::Verified: return "verified";
    default: return "missing";
    }
}

struct Service::Impl {
    using Clock = std::chrono::steady_clock;
    using Digest = std::array<unsigned char, 32>;
    struct Pending { std::string qq; Digest hash; Clock::time_point expires; unsigned attempts = 0;
                     std::uint64_t generation = 0; bool ready = false; };
    struct Work { std::string ctx, qq, code; Json smtp; std::uint64_t generation;
                  std::function<void(Status)> completed; };
    Sender sender; Now now; std::array<unsigned char, 32> secret{};
    std::mutex mu; std::condition_variable cv; std::thread worker; bool stopping = false;
    std::map<std::string, Pending> pending;
    std::map<std::string, std::deque<Clock::time_point>> history;
    std::deque<Work> queue; std::uint64_t generation = 0;
    Impl(Sender send, Now time) : sender(send ? std::move(send) : smtpSend),
        now(time ? std::move(time) : [] { return Clock::now(); }) {
        if (RAND_bytes(secret.data(), secret.size()) != 1) throw std::runtime_error("random unavailable");
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard lock(mu); stopping = true; queue.clear(); }
        cv.notify_one(); worker.join(); OPENSSL_cleanse(secret.data(), secret.size());
    }
    Digest hash(const std::string& ctx, const std::string& qq, const std::string& code) {
        const std::string text = ctx + '\0' + qq + '\0' + code;
        Digest out{}; unsigned len = 0;
        if (!HMAC(EVP_sha256(), secret.data(), secret.size(),
                  reinterpret_cast<const unsigned char*>(text.data()), text.size(), out.data(), &len)
            || len != out.size()) throw std::runtime_error("digest unavailable");
        return out;
    }
    void prune(Clock::time_point t) {
        for (auto it = history.begin(); it != history.end();) {
            auto& h = it->second;
            while (!h.empty() && t - h.front() >= std::chrono::hours(1)) h.pop_front();
            if (h.empty()) it = history.erase(it); else ++it;
        }
        for (auto it = pending.begin(); it != pending.end();)
            if (it->second.expires <= t) it = pending.erase(it); else ++it;
    }
    void run() {
        for (;;) {
            Work work;
            { std::unique_lock lock(mu); cv.wait(lock, [this] { return stopping || !queue.empty(); });
              if (stopping) return; work = std::move(queue.front()); queue.pop_front(); }
            bool sent = false;
            try { sent = sender(work.smtp, work.qq + "@qq.com", verificationBody(work.qq, work.code)); } catch (...) {}
            OPENSSL_cleanse(work.code.data(), work.code.size());
            { std::lock_guard lock(mu);
              auto it = pending.find(work.ctx);
              if (it == pending.end() || it->second.generation != work.generation) continue;
              if (sent) { it->second.ready = true; it->second.expires = now() + std::chrono::minutes(10); }
              else pending.erase(it); }
            try { if (work.completed) work.completed(sent ? Status::Sent : Status::SendFailed); } catch (...) {}
        }
    }
};
Service::Service(Sender send, Now now) : impl_(std::make_unique<Impl>(std::move(send), std::move(now))) {}
Service::~Service() = default;
Status Service::begin(const std::string& ctx, const std::string& qq, const Json& smtp,
                      std::function<void(Status)> completed) {
    if (!smtp.is_object() || !smtp.contains("enabled")) return Status::Disabled;
    if (!smtp["enabled"].is_boolean()) return Status::BadConfig;
    if (!smtp["enabled"].get<bool>()) return Status::Disabled;
    if (ctx.empty() || ctx.size() > 2048 || !validQQ(qq) || !validSettings(smtp)) return Status::BadConfig;
    auto& p = *impl_; std::lock_guard lock(p.mu); const auto t = p.now(); p.prune(t);
    const std::array<std::string, 3> buckets = {"ctx:" + ctx, "qq:" + qq, "global"};
    for (size_t i = 0; i < buckets.size(); ++i) {
        auto it = p.history.find(buckets[i]); if (it == p.history.end()) continue;
        if (it->second.size() >= (i == 2 ? 100 : 5)
            || (i < 2 && t - it->second.back() < std::chrono::seconds(60))) return Status::RateLimited;
    }
    if (p.stopping || p.queue.size() >= 16 || p.pending.size() >= 1000) return Status::Busy;
    try {
        auto code = newCode(); const auto seq = ++p.generation;
        p.pending[ctx] = {qq, p.hash(ctx, qq, code), t + std::chrono::minutes(10), 0, seq, false};
        for (const auto& bucket : buckets) p.history[bucket].push_back(t);
        p.queue.push_back({ctx, qq, std::move(code), smtp, seq, std::move(completed)});
        p.cv.notify_one(); return Status::Queued;
    } catch (...) { p.pending.erase(ctx); return Status::SendFailed; }
}
void Service::cancel(const std::string& ctx) {
    std::lock_guard lock(impl_->mu);
    impl_->pending.erase(ctx);
}
Status Service::verify(const std::string& ctx, const std::string& qq, const std::string& code) {
    auto& p = *impl_; std::lock_guard lock(p.mu);
    auto it = p.pending.find(ctx);
    if (it == p.pending.end()) return Status::Missing;
    auto& ticket = it->second;
    if (ticket.expires <= p.now()) { p.pending.erase(it); return Status::Missing; }
    if (!ticket.ready) return Status::NotReady;
    const bool format = code.size() == 8 && std::all_of(code.begin(), code.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    const auto supplied = p.hash(ctx, qq, code);
    if (ticket.qq != qq || !format || CRYPTO_memcmp(supplied.data(), ticket.hash.data(), supplied.size()) != 0) {
        if (++ticket.attempts >= 5) { p.pending.erase(it); return Status::Locked; }
        return Status::WrongCode;
    }
    p.pending.erase(it); return Status::Verified;
}
} // namespace dice::identity_email
