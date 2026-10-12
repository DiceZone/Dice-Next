#pragma once

#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dice::qqmsg {

inline std::string cqEscape(std::string_view text) {
    std::string out;
    for (char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '[': out += "&#91;"; break;
        case ']': out += "&#93;"; break;
        case ',': out += "&#44;"; break;
        default: out += ch;
        }
    }
    return out;
}

inline std::string imageCode(const std::string& url) {
    return "[CQ:image,file=" + cqEscape(url) + "]";
}

// Official attachments sometimes omit the scheme. Never interpret faceId=0
// as an image identifier or turn an arbitrary local path into a remote URL.
inline std::string imageUrl(std::string url) {
    if (url.empty()) return {};
    for (unsigned char ch : url)
        if (ch <= 0x20 || ch == 0x7f || ch == '"' || ch == '\'' || ch == '<' || ch == '>') return {};
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    else if (url.find("://") == std::string::npos && url.find('/') != std::string::npos
             && url.substr(0, url.find('/')).find('.') != std::string::npos)
        url = "https://" + url;
    const size_t prefix = url.rfind("https://", 0) == 0 ? 8 : url.rfind("http://", 0) == 0 ? 7 : 0;
    if (!prefix || url.size() <= prefix || url[prefix] == '/' || url[prefix] == '?'
        || url.find('@', prefix) != std::string::npos) return {};
    return url;
}

struct Face {
    int type = 0;
    size_t length = 0;
    std::string id;
    std::string url;
};

inline std::optional<Face> parseFace(std::string_view text) {
    if (!text.starts_with("<faceType")) return std::nullopt;
    const auto end = text.find('>');
    if (end == std::string_view::npos || end > 16384 || text.substr(1, end - 1).find('<') != std::string_view::npos)
        return std::nullopt;
    // Comma-separated attributes, with quoted values (ext is base64 JSON).
    // A bounded scanner avoids a regex's recursion on large/malformed input.
    std::map<std::string, std::string> fields;
    size_t p = 1;
    const auto space = [&] { while (p < end && std::isspace(static_cast<unsigned char>(text[p]))) ++p; };
    while (p < end) {
        space();
        const size_t begin = p;
        while (p < end && (std::isalnum(static_cast<unsigned char>(text[p])) || text[p] == '_')) ++p;
        const std::string key(text.substr(begin, p - begin));
        space();
        if (key.empty() || p >= end || text[p++] != '=') return std::nullopt;
        space();
        std::string value;
        if (p < end && (text[p] == '"' || text[p] == '\'')) {
            const char quote = text[p++];
            const size_t start = p;
            while (p < end && text[p] != quote) ++p;
            if (p == end) return std::nullopt;
            value = std::string(text.substr(start, p - start));
            ++p;
        } else {
            const size_t start = p;
            while (p < end && text[p] != ',' && !std::isspace(static_cast<unsigned char>(text[p]))) ++p;
            value = std::string(text.substr(start, p - start));
        }
        if (!fields.emplace(key, value).second) return std::nullopt;
        space();
        if (p < end && text[p++] != ',') return std::nullopt;
    }
    Face face;
    const auto type = fields.find("faceType");
    if (type == fields.end() || (type->second != "1" && type->second != "6")) return std::nullopt;
    face.type = type->second == "1" ? 1 : 6;
    face.length = end + 1;
    if (const auto id = fields.find("faceId"); id != fields.end()) face.id = id->second;
    for (const char* key : {"url", "image_url"}) {
        if (const auto field = fields.find(key); field != fields.end()) face.url = imageUrl(field->second);
        if (!face.url.empty()) break;
    }
    if (face.type == 6 && face.url.empty()) {
        if (const auto ext = fields.find("ext"); ext != fields.end()) {
            try {
                const auto data = nlohmann::json::parse(drogon::utils::base64Decode(ext->second), nullptr, false);
                if (data.is_object()) for (const char* key : {"url", "image_url"}) {
                    if (data.contains(key) && data[key].is_string()) face.url = imageUrl(data[key].get<std::string>());
                    if (!face.url.empty()) break;
                }
            } catch (...) {}
        }
    }
    return face;
}

struct IncomingText {
    std::string command;
    std::string raw;
    std::string display;
};

inline IncomingText normalizeIncoming(const std::string& content, const nlohmann::json& attachments) {
    struct Image { std::string url; bool used = false; };
    std::vector<Image> images;
    if (attachments.is_array()) for (const auto& attachment : attachments) {
        if (!attachment.is_object() || !attachment.contains("url") || !attachment["url"].is_string()) continue;
        const auto mime = attachment.find("content_type");
        if (mime != attachment.end() && mime->is_string() && !mime->get_ref<const std::string&>().empty()
            && !mime->get_ref<const std::string&>().starts_with("image/")) continue;
        auto url = imageUrl(attachment["url"].get<std::string>());
        if (!url.empty()) images.push_back({std::move(url)});
    }
    IncomingText out;
    for (size_t p = 0; p < content.size();) {
        const auto face = content[p] == '<' ? parseFace(std::string_view(content).substr(p)) : std::nullopt;
        if (!face) {
            out.command += content[p]; out.raw += content[p]; out.display += content[p++];
            continue;
        }
        if (face->type == 1) {
            const bool numeric = !face->id.empty() && face->id.find_first_not_of("0123456789") == std::string::npos;
            out.raw += numeric ? "[CQ:face,id=" + face->id + "]" : "[表情]";
            out.display += "[表情]";
        } else {
            auto url = face->url;
            for (auto& image : images) {
                if (!image.used && (url.empty() || image.url == url)) {
                    url = image.url; image.used = true; break;
                }
            }
            out.raw += url.empty() ? "[图片]" : imageCode(url);
            out.display += "[图片]";
        }
        p += face->length;
    }
    // Ordinary attached images also need a CQ reference, even when the body is
    // empty. Do not append an attachment already represented by a face marker.
    for (const auto& image : images) if (!image.used) {
        if (!out.raw.empty()) out.raw += '\n';
        if (!out.display.empty()) out.display += '\n';
        out.raw += imageCode(image.url);
        out.display += "[图片]";
    }
    return out;
}

// Historical log rows may still contain the official marker. Image references
// are backfilled separately, in order, by the transcript renderer.
inline std::string normalizeLogFaces(const std::string& content) {
    std::string out;
    for (size_t p = 0; p < content.size();) {
        const auto face = content[p] == '<' ? parseFace(std::string_view(content).substr(p)) : std::nullopt;
        if (!face) { out += content[p++]; continue; }
        out += face->type == 1 ? "[表情]" : face->url.empty() ? "[图片]" : imageCode(face->url);
        p += face->length;
    }
    return out;
}

} // namespace dice::qqmsg
