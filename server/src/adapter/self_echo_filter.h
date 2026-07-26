#pragma once
// ─── Dice!Next — 自回声去重 + 跨骰防环护栏────────────
// 自控：开启自响应后，骰娘账号自身发出的消息（post_type=message_sent）也进
// 管线，允许用骰娘账号发指令自控（含插件/自定义回复）。为防止骰娘**自己的回复**
// 被再次当消息处理而无限自触发，这里登记每条「本程序主动发出」的消息，收到 message_sent
// 时若能在近期登记里匹配到 → 判为自身回声，跳过（不处理）。而操作者手打的消息不经我们
// 的发送方法、不会被登记 → 正常进管线。
//
// 跨骰防环：若一条待发回复以指令前缀（. 。 ! ！ / ／）开头，别的骰子可能把它当指令
// 而互相触发。发送前在首个非空白字符前插入零宽空格（U+200B），人眼几乎无感，但其它
// 骰子的「首字符是否前缀」判定会失败，从而不触发。

#include <string>
#include <deque>
#include <mutex>
#include <ctime>

namespace dice {

// 归一化用于自回声匹配：去掉方括号码（[CQ:..]/[img,..] 等）+ 合并空白。两侧（登记
// 的出站文本 与 收到的 message_sent 明文）用同一函数，保证纯文本回复能精确匹配。
inline std::string normalizeEcho(const std::string& s) {
    std::string stripped; stripped.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') { size_t e = s.find(']', i); if (e != std::string::npos) { i = e + 1; continue; } }
        stripped += s[i++];
    }
    std::string r; bool pendingSpace = false;
    for (char c : stripped) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { pendingSpace = true; }
        else { if (pendingSpace && !r.empty()) r += ' '; pendingSpace = false; r += c; }
    }
    return r;
}

// 跨骰护栏：回复以指令前缀开头时，在首个非空白字符前插入零宽空格。
inline std::string guardCrossBot(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
    if (i >= text.size()) return text;
    unsigned char c = static_cast<unsigned char>(text[i]);
    bool isPrefix = (c == '.' || c == '!' || c == '/');
    // 。= E3 80 82
    if (!isPrefix && c == 0xE3 && i + 2 < text.size()
        && (unsigned char)text[i + 1] == 0x80 && (unsigned char)text[i + 2] == 0x82) isPrefix = true;
    // ！= EF BC 81 ; ／= EF BC 8F
    if (!isPrefix && c == 0xEF && i + 2 < text.size() && (unsigned char)text[i + 1] == 0xBC
        && ((unsigned char)text[i + 2] == 0x81 || (unsigned char)text[i + 2] == 0x8F)) isPrefix = true;
    if (!isPrefix) return text;
    return text.substr(0, i) + "\xE2\x80\x8B" + text.substr(i);   // U+200B 零宽空格
}

class SelfEchoFilter {
public:
    static SelfEchoFilter& instance() { static SelfEchoFilter f; return f; }

    // 登记一条本程序主动发出的消息（key = "<platform>:<target>"，norm = normalizeEcho 后文本）。
    void mark(const std::string& key, const std::string& norm) {
        if (norm.empty()) return;
        std::lock_guard<std::mutex> lk(m_);
        prune();
        q_.push_back({key + "\x1f" + norm, (int64_t)std::time(nullptr)});
    }

    // 若近期登记里有匹配项 → 消费它并返回 true（判为自身回声）。
    bool consume(const std::string& key, const std::string& norm) {
        std::lock_guard<std::mutex> lk(m_);
        prune();
        std::string k = key + "\x1f" + norm;
        for (auto it = q_.begin(); it != q_.end(); ++it)
            if (it->k == k) { q_.erase(it); return true; }
        return false;
    }

private:
    struct E { std::string k; int64_t t; };
    void prune() {   // 只保留最近 ~15 秒（回声通常秒级返回）。
        int64_t now = (int64_t)std::time(nullptr);
        while (!q_.empty() && now - q_.front().t > 15) q_.pop_front();
        while (q_.size() > 200) q_.pop_front();
    }
    std::deque<E> q_;
    std::mutex m_;
};

}  // namespace dice
