#pragma once
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace dice::notice_webhook {
using Json = nlohmann::json;
inline const std::vector<std::string>& variables() {
    static const std::vector<std::string> names{"message", "title", "time", "level", "level_name", "event", "origin",
        "platform", "adapter_id", "account_id", "group_id", "group_name", "user_id", "user_name"};
    return names;
}
inline bool clean(const std::string& value, size_t limit) {
    return value.size() <= limit && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
inline bool validUrl(const std::string& url) {
    if (!clean(url, 4096) || url.find_first_of(" <>\"{}#\\") != std::string::npos) return false;
    const size_t start = url.rfind("https://", 0) == 0 ? 8 : url.rfind("http://", 0) == 0 ? 7 : 0;
    if (!start) return false;
    const auto authority = url.substr(start, url.find_first_of("/?", start) - start);
    return !authority.empty() && authority.find('@') == std::string::npos;
}
inline Json preset(const std::string& profile) {
    if (profile == "wecom" || profile == "dingtalk") return {{"msgtype", "text"}, {"text", {{"content", "{{message}}"}}}};
    if (profile == "feishu") return {{"msg_type", "text"}, {"content", {{"text", "{{message}}"}}}};
    return {{"ts", "{{time}}"}, {"level", "{{level}}"}, {"op", "{{event}}"}, {"title", "{{title}}"}, {"msg", "{{message}}"}, {"origin", "{{origin}}"}};
}
inline void validateTemplate(const Json& value, int depth = 0) {
    if (depth > 16) throw std::invalid_argument("Webhook JSON 模板嵌套不能超过 16 层");
    if (value.is_structured()) {
        for (const auto& child : value) validateTemplate(child, depth + 1);
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        for (size_t pos = 0; (pos = text.find("{{", pos)) != std::string::npos;) {
            const auto end = text.find("}}", pos + 2);
            if (end == std::string::npos) throw std::invalid_argument("Webhook 模板变量缺少 }}");
            const auto name = text.substr(pos + 2, end - pos - 2);
            if (std::find(variables().begin(), variables().end(), name) == variables().end())
                throw std::invalid_argument("Webhook 模板包含未知变量：" + name);
            pos = end + 2;
        }
    }
}
inline Json normalize(const Json& input) {
    const auto profile = input.value("profile", std::string("generic"));
    if (profile != "generic" && profile != "wecom" && profile != "dingtalk" && profile != "feishu" && profile != "custom")
        throw std::invalid_argument("不支持的 Webhook 类型");
    const auto url = input.value("url", std::string());
    if (!validUrl(url)) throw std::invalid_argument("Webhook 地址无效，请填写完整的 HTTP(S) 地址");
    const auto method = input.value("method", std::string("POST"));
    if (method != "POST" && method != "PUT" && method != "PATCH") throw std::invalid_argument("Webhook 只支持 POST、PUT、PATCH");
    Json headers = input.value("headers", Json::object());
    if (!headers.is_object() || headers.size() > 32) throw std::invalid_argument("Webhook 请求头必须是最多 32 项的 JSON 对象");
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const auto& key = it.key(); auto lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool token = !key.empty() && key.size() <= 128 && std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return std::isalnum(c) || std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos;
        });
        if (!token || !it->is_string() || !clean(it->get<std::string>(), 8192) || lower == "host" || lower == "connection" ||
            lower == "content-length" || lower == "transfer-encoding" || lower.rfind("proxy-", 0) == 0)
            throw std::invalid_argument("Webhook 请求头无效或包含受保护字段");
    }
    Json result{{"kind", "webhook"}, {"url", url}, {"profile", profile}, {"method", method}, {"headers", headers}};
    if (input.contains("body_template")) {
        const auto& body = input["body_template"];
        if (!body.is_object() || body.dump().size() > 64 * 1024) throw std::invalid_argument("Webhook 请求体模板必须是 64 KiB 以内的 JSON 对象");
        validateTemplate(body); result["body_template"] = body;
    } else if (profile == "custom") throw std::invalid_argument("请配置自定义 Webhook JSON 请求体");
    const auto secret = input.value("sign_secret", std::string());
    if (!secret.empty()) {
        if ((profile != "dingtalk" && profile != "feishu") || !clean(secret, 4096)) throw std::invalid_argument("签名密钥仅适用于钉钉和飞书");
        result["sign_secret"] = secret;
    }
    return result;
}
// Parse the original template once. Substituted data is never reparsed as JSON
// or as another template, and object keys / URL / headers are never evaluated.
inline Json render(const Json& value, const Json& vars) {
    if (value.is_object()) { auto out = value; for (auto& child : out) child = render(child, vars); return out; }
    if (value.is_array()) { auto out = value; for (auto& child : out) child = render(child, vars); return out; }
    if (!value.is_string()) return value;
    const auto& text = value.get_ref<const std::string&>();
    std::string out; size_t pos = 0;
    while (pos < text.size()) {
        const auto begin = text.find("{{", pos);
        if (begin == std::string::npos) { out += text.substr(pos); break; }
        out += text.substr(pos, begin - pos); const auto end = text.find("}}", begin + 2);
        if (end == std::string::npos) throw std::invalid_argument("Webhook 模板变量不完整");
        const auto key = text.substr(begin + 2, end - begin - 2);
        const auto it = vars.find(key);
        const Json replacement = it == vars.end() ? Json("") : *it;
        if (begin == 0 && end + 2 == text.size()) return replacement;
        out += replacement.is_string() ? replacement.get<std::string>() : replacement.dump();
        pos = end + 2;
    }
    return out;
}
inline std::string clipped(std::string text, size_t bytes) {
    if (text.size() <= bytes) return text;
    const std::string suffix = "\n…（内容过长，请查看 Dice!Next 后台）";
    size_t end = bytes > suffix.size() ? bytes - suffix.size() : 0;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return text.substr(0, end) + suffix;
}
inline std::string signature(const std::string& key, const std::string& message) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &size))
        throw std::runtime_error("Webhook 签名计算失败");
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), digest, size);
    out.resize(static_cast<size_t>(length)); return out;
}
inline std::string urlEncode(const std::string& text) {
    static constexpr char hex[] = "0123456789ABCDEF"; std::string result;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
inline std::string withoutSigningParams(const std::string& url) {
    const auto split = url.find('?'); if (split == std::string::npos) return url;
    std::string out = url.substr(0, split); size_t at = split + 1; bool first = true;
    while (at < url.size()) {
        const auto end = url.find('&', at); const auto part = url.substr(at, end - at); const auto key = part.substr(0, part.find('='));
        if (!part.empty() && key != "timestamp" && key != "sign") { out += first ? '?' : '&'; out += part; first = false; }
        if (end == std::string::npos) break; at = end + 1;
    }
    return out;
}
struct Request { std::string url, method, body; Json headers; };
inline Request build(const Json& channel, Json vars, int64_t nowMs) {
    const auto c = normalize(channel); const auto profile = c.value("profile", std::string("generic"));
    if (profile == "wecom") vars["message"] = clipped(vars.value("message", std::string()), 2048);
    auto body = render(c.value("body_template", preset(profile)), vars);
    std::string url = c["url"].get<std::string>(); const auto secret = c.value("sign_secret", std::string());
    if (!secret.empty() && profile == "dingtalk") {
        const auto timestamp = std::to_string(nowMs); url = withoutSigningParams(url);
        url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("timestamp=") + timestamp + "&sign=" + urlEncode(signature(secret, timestamp + "\n" + secret));
    } else if (!secret.empty() && profile == "feishu") {
        const auto timestamp = std::to_string(nowMs / 1000); body["timestamp"] = timestamp; body["sign"] = signature(timestamp + "\n" + secret, "");
    }
    const auto serialized = body.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (serialized.size() > 256 * 1024) throw std::invalid_argument("渲染后的 Webhook 请求体超过 256 KiB");
    return {std::move(url), c["method"].get<std::string>(), serialized, c["headers"]};
}
} // namespace dice::notice_webhook
