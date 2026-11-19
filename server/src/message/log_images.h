#pragma once
#include "qq_official_message.h"

namespace dice::logimages {
struct Image { size_t start, length; std::string ref; };

inline std::string unescape(std::string s) {
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
        {"&#44;", ","}, {"&#91;", "["}, {"&#93;", "]"}, {"&amp;", "&"}}) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    }
    return s;
}

// Scan all supported image forms together: references must retain message order,
// including unavailable images, so subsequent URLs cannot shift to another image.
inline std::vector<Image> scan(const std::string& text) {
    std::vector<Image> out;
    for (size_t p = 0; p < text.size(); ++p) {
        size_t prefix = 0;
        bool fields = false;
        for (const auto& tag : {std::string("[CQ:image,"), std::string("[img,"), std::string("[图片:"), std::string("[图:"), std::string("[图片]")}) {
            if (text.compare(p, tag.size(), tag) == 0) {
                prefix = tag.size(); fields = tag == "[CQ:image," || tag == "[img,"; break;
            }
        }
        if (!prefix) continue;
        const auto end = text.find(']', p);
        if (end == std::string::npos) continue;
        std::string ref, file;
        if (fields) {
            for (size_t k = p + prefix; k < end;) {
                auto next = text.find(',', k); if (next == std::string::npos || next > end) next = end;
                auto field = text.substr(k, next - k);
                if (field.starts_with("url=")) ref = unescape(field.substr(4));
                if (field.starts_with("file=")) file = unescape(field.substr(5));
                k = next + 1;
            }
            if (ref.empty()) ref = file;
        } else if (text[p + prefix - 1] != ']') ref = unescape(text.substr(p + prefix, end - p - prefix));
        out.push_back({p, end - p + 1, ref}); p = end;
    }
    return out;
}

inline std::string refs(const std::string& text) {
    nlohmann::json refs = nlohmann::json::array();
    for (const auto& image : scan(text)) refs.push_back(image.ref);
    return refs.empty() ? std::string() : refs.dump();
}

inline std::string labels(const std::string& text) {
    std::string out; size_t last = 0;
    for (const auto& image : scan(text)) {
        out += text.substr(last, image.start - last); out += "[图片]"; last = image.start + image.length;
    }
    return out + text.substr(last);
}
}
