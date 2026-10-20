#include "image_resource_service.h"
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <set>

namespace dice::image_resources {
namespace {
bool escaped(const std::string& text, std::size_t at) {
    std::size_t count = 0;
    while (at && text[--at] == '\\') ++count;
    return count % 2 != 0;
}
std::string decode(std::string value) {
    for (const auto& [from, to] : std::vector<std::pair<std::string,std::string>>{
            {"&#44;",","},{"&#91;","["},{"&#93;","]"},{"&amp;","&"}}) {
        std::size_t at = 0;
        while ((at = value.find(from, at)) != std::string::npos) { value.replace(at, from.size(), to); at += to.size(); }
    }
    return value;
}
void walkJson(nlohmann::json& node, const Mapper& mapper, unsigned depth, std::size_t& visited) {
    if (depth > 64 || ++visited > 200000) throw std::runtime_error("图片引用的层级或节点数量超过限制");
    if (node.is_string()) node = rewriteText(node.get<std::string>(), mapper);
    else if (node.is_array() || node.is_object()) for (auto& value : node) walkJson(value, mapper, depth + 1, visited);
}
void walkYaml(YAML::Node node, const Mapper& mapper, unsigned depth, std::size_t& visited) {
    if (depth > 64 || ++visited > 200000) throw std::runtime_error("图片引用的层级或节点数量超过限制");
    if (node.IsScalar()) {
        const auto before = node.Scalar(), after = rewriteText(before, mapper);
        if (after != before) node = after;
    } else if (node.IsSequence()) for (auto value : node) walkYaml(value, mapper, depth + 1, visited);
    else if (node.IsMap()) for (auto value : node) walkYaml(value.second, mapper, depth + 1, visited);
}
}
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string(); return std::string(text.begin(), text.end());
}
std::filesystem::path fromUtf8(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(), value.end())); }
std::string escapeCode(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '&') out += "&amp;"; else if (c == ',') out += "&#44;";
        else if (c == '[') out += "&#91;"; else if (c == ']') out += "&#93;"; else out += c;
    }
    return out;
}
std::string imageCode(const std::string& path) { return "[img,file=" + escapeCode(path) + "]"; }
std::string unescapeCode(std::string value) {return decode(std::move(value));}

std::vector<ImageReference> references(const std::string& text) {
    std::vector<ImageReference> out;
    std::size_t nextClose=std::string::npos;
    for (std::size_t at = 0; (at = text.find('[', at)) != std::string::npos; ++at) {
        if (escaped(text, at)) continue;
        if(nextClose==std::string::npos || nextClose<at)nextClose=text.find(']',at+1);
        const auto close=nextClose;
        if (close == std::string::npos) break;
        const auto token = std::string_view(text).substr(at, close - at + 1);
        const bool cq = token.rfind("[CQ:image,",0)==0 || token.rfind("[img,",0)==0;
        if (cq) {
            // Require a field boundary; 'profile=' is not 'file='.
            const auto key = token.find(",file=");
            const auto url = token.find(",url=");
            const auto start = key != std::string::npos ? key + 6 : url != std::string::npos ? url + 5 : std::string::npos;
            if (start != std::string::npos) {
                auto end = token.find(',',start); if (end == std::string::npos) end = token.size()-1;
                if (end > start) out.push_back({at+start,at+end,decode(std::string(token.substr(start,end-start))),true});
            }
            at = close; continue;
        }
        bool image = false;
        for (const std::string prefix : {"[图:","[图片:","[img:","[image:"}) if (token.rfind(prefix,0)==0) {
            const auto start = at+prefix.size();
            out.push_back({start,close,decode(text.substr(start,close-start)),true}); image = true; break;
        }
        if (image) { at=close; continue; }
        // Markdown image destination. It is never interpreted as markup while listing.
        if (at && text[at-1]=='!' && !escaped(text,at-1) && close+1<text.size() && text[close+1]=='(') {
            std::size_t begin=close+2, end=begin;
            if (begin<text.size() && text[begin]=='<') { ++begin; end=text.find('>',begin); }
            else {
                int depth=0;
                for (;end<text.size();++end) {
                    if (escaped(text,end)) continue;
                    if (text[end]=='(') ++depth;
                    if (text[end]==')' && depth--==0) break;
                    if (std::isspace(static_cast<unsigned char>(text[end])) && depth==0) break;
                }
            }
            if (end!=std::string::npos && end>begin && end<text.size()) out.push_back({begin,end,text.substr(begin,end-begin),false});
        }
    }
    return out;
}

std::string rewriteText(const std::string& text, const Mapper& mapper) {
    if (auto mapped=mapper(text); mapped && *mapped!=text) return *mapped;
    const auto spans=references(text);
    std::string out; std::size_t cursor=0;
    for (const auto& span : spans) {
        if (span.begin<cursor) continue;
        out += text.substr(cursor,span.begin-cursor);
        if (auto mapped=mapper(span.value)) out += span.cq ? escapeCode(*mapped) : *mapped;
        else out += text.substr(span.begin,span.end-span.begin);
        cursor=span.end;
    }
    out += text.substr(cursor);
    return out;
}

std::string rewriteDocument(const Document& document, const Mapper& mapper) {
    if (document.format=="json") {
        auto node=nlohmann::json::parse(document.content); const auto before=node;
        std::size_t visited=0;walkJson(node,mapper,0,visited); return node==before ? document.content : node.dump(2);
    }
    if (document.format=="yaml") {
        auto node=YAML::Load(document.content);
        bool changed=false;
        std::size_t visited=0;
        walkYaml(node,[&](const std::string& ref) { auto result=mapper(ref); if (result && *result!=ref) changed=true; return result; },0,visited);
        return changed ? YAML::Dump(node) : document.content;
    }
    return rewriteText(document.content,mapper);
}
std::vector<std::string> documentReferences(const Document& document) {
    std::set<std::string> out;
    (void)rewriteDocument(document,[&](const std::string& ref)->std::optional<std::string> {
        out.insert(ref); return std::nullopt;
    });
    return {out.begin(),out.end()};
}
} // namespace dice::image_resources
