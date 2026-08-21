#include "i18n.h"
#include "../common/logger.h"
#include "../common/markdown.h"

#include <fstream>
#include <filesystem>
#include <sstream>
#include <cctype>

namespace fs = std::filesystem;

namespace dice {

thread_local bool I18n::outboundCaptureActive_ = false;
thread_local bool I18n::outboundCaptureMarkdown_ = false;

// ═══════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════

I18n::I18n(std::string resourceDir, Locale defaultLocale)
    : resourceDir_(std::move(resourceDir))
    , defaultLocale_(defaultLocale) {}

std::vector<Locale> I18n::supportedLocales() {
    return {Locale::kZhHant, Locale::kZhHans, Locale::kEn, Locale::kJa};
}

// ═══════════════════════════════════════════════════════════════
// Loading
// ═══════════════════════════════════════════════════════════════

bool I18n::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Resolve the resource directory: prefer the configured path, but
    // fall back to a parent-relative copy (handy when the binary runs
    // from a build/ subfolder) — mirrors the web/dist lookup in main.cpp.
    std::string dir = resourceDir_;
    if (!fs::exists(dir) && fs::exists("../" + resourceDir_)) {
        dir = "../" + resourceDir_;
    }

    bundles_.clear();
    keywordMap_.clear();
    int loaded = 0;

    for (Locale loc : supportedLocales()) {
        fs::path file = fs::path(dir) / (std::string(localeToString(loc)) + ".json");
        if (!fs::exists(file)) {
            DICE_LOG_WARN("I18n: resource file missing: {}", file.string());
            continue;
        }
        try {
            std::ifstream in(file);
            json j;
            in >> j;
            bundles_[loc] = std::move(j);
            ++loaded;
            DICE_LOG_INFO("I18n: loaded locale '{}' from {}",
                          localeToString(loc), file.string());
        } catch (const std::exception& e) {
            DICE_LOG_ERROR("I18n: failed to parse {}: {}", file.string(), e.what());
        }
    }

    // ── 自定义翻译文件 —— 加载目录里内置四语之外的所有 <code>.json。
    // 文件名(去扩展名)即语言码，注册为动态 Locale；用户可自加任意语言。
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
        std::string code = e.path().stem().string();
        if (code == "zh-Hans" || code == "zh-Hant" || code == "en" || code == "ja") continue;
        if (code.empty() || code[0] == '_') continue;   // _开头保留（模板/示例）
        try {
            std::ifstream in(e.path());
            json j;
            in >> j;
            if (!j.is_object()) continue;
            Locale loc = registerCustomLocale(code);
            bundles_[loc] = std::move(j);
            ++loaded;
            DICE_LOG_INFO("I18n: loaded CUSTOM locale '{}' from {}", code, e.path().string());
        } catch (const std::exception& ex) {
            DICE_LOG_ERROR("I18n: failed to parse custom locale {}: {}", e.path().string(), ex.what());
        }
    }

    // ── 汇总 .lang 切换关键词 —— 每个包（含内置）可在 `_meta.keywords`
    // 声明触发词（如 ["ko","korean","韩语","한국어"]）；语言码本身恒可用。
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (const auto& [loc, j] : bundles_) {
        keywordMap_[lower(localeToString(loc))] = loc;
        if (j.contains("_meta") && j["_meta"].is_object() &&
            j["_meta"].contains("keywords") && j["_meta"]["keywords"].is_array()) {
            for (const auto& kw : j["_meta"]["keywords"])
                if (kw.is_string() && !kw.get<std::string>().empty())
                    keywordMap_[lower(kw.get<std::string>())] = loc;
        }
    }

    if (loaded == 0) {
        DICE_LOG_ERROR("I18n: no locale bundles loaded from '{}' — "
                       "replies will fall back to raw keys", dir);
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
// custom-locale helpers
// ═══════════════════════════════════════════════════════════════

std::optional<Locale> I18n::localeForKeyword(const std::string& input) const {
    std::string s;
    for (char c : input) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // trim
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::nullopt;
    auto e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = keywordMap_.find(s); it != keywordMap_.end()) return it->second;
    return std::nullopt;
}

std::string I18n::localeDisplayName(Locale loc) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bundles_.find(loc);
    if (it != bundles_.end()) {
        const json& j = it->second;
        if (j.contains("_meta") && j["_meta"].is_object() && j["_meta"].contains("name") &&
            j["_meta"]["name"].is_string())
            return j["_meta"]["name"].get<std::string>();
        if (j.contains("lang") && j["lang"].is_object() && j["lang"].contains("name") &&
            j["lang"]["name"].is_string())
            return j["lang"]["name"].get<std::string>();
    }
    return localeToString(loc);
}

std::vector<std::pair<std::string, std::string>> I18n::listLocales() const {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<Locale> locs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [loc, _] : bundles_) locs.push_back(loc);
    }
    for (Locale loc : locs)
        out.emplace_back(localeToString(loc), localeDisplayName(loc));
    return out;
}

bool I18n::reload() {
    DICE_LOG_INFO("I18n: reloading translation bundles");
    return load();
}

// Recursively flatten a json object into dotted keys for every string leaf.
static void flattenNode(const json& node, const std::string& prefix,
                        std::map<std::string, std::string>& out) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            flattenNode(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), out);
    } else if (node.is_string()) {
        out[prefix] = node.get<std::string>();
    }
}

std::map<std::string, std::string> I18n::flatten(Locale loc) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::string> out;
    auto it = bundles_.find(loc);
    if (it != bundles_.end()) flattenNode(it->second, "", out);
    // `_meta`（显示名/切换关键词）不是文案，别混进「全部文本」编辑器。
    for (auto e = out.begin(); e != out.end();)
        e = (e->first.rfind("_meta", 0) == 0) ? out.erase(e) : std::next(e);
    return out;
}

// ═══════════════════════════════════════════════════════════════
// Lookup
// ═══════════════════════════════════════════════════════════════

const json* I18n::lookupNode(Locale loc, const std::string& key) const {
    auto it = bundles_.find(loc);
    if (it == bundles_.end()) return nullptr;

    const json* node = &it->second;
    std::stringstream ss(key);
    std::string segment;
    while (std::getline(ss, segment, '.')) {
        if (!node->is_object() || !node->contains(segment)) {
            return nullptr;
        }
        node = &(*node)[segment];
    }
    return (node->is_string()) ? node : nullptr;
}

std::string I18n::tr(Locale loc, const std::string& key, const Args& args) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找顺序（更新）：
    //   请求语言的覆盖 → 请求语言的人格层(若启用) → 请求语言的内置 →
    //   默认语言的覆盖 → 默认语言的人格层 → 默认语言的内置 → raw key。
    // 关键：请求语言的「内置文案」必须优先于「默认语言的覆盖」——否则用户把简中
    // 文案覆盖后，切到英文仍会显示那条简中覆盖（应回到英文内置）。
    auto ovIt = overrides_.find(loc);
    if (ovIt != overrides_.end()) {
        auto kIt = ovIt->second.find(key);
        if (kIt != ovIt->second.end()) return renderTemplate(kIt->second.value, args, kIt->second.format);
    }
    // persona layer (between override and bundle)
    if (const json* personaNode = lookupPersonaNode(loc, key))
        return renderTemplate(personaNode->get<std::string>(), args, lookupPersonaFormat(loc, key));
    if (const json* locNode = lookupNode(loc, key)) {
        const std::string raw = locNode->get<std::string>();
        return renderTemplate(raw, args, trustedTemplateFormat(raw));
    }

    const json* node = nullptr;
    if (loc != defaultLocale_) {
        auto dIt = overrides_.find(defaultLocale_);
        if (dIt != overrides_.end()) {
            auto kIt = dIt->second.find(key);
            if (kIt != dIt->second.end()) return renderTemplate(kIt->second.value, args, kIt->second.format);
        }
        // default locale persona layer
        if (const json* dPersona = lookupPersonaNode(defaultLocale_, key))
            return renderTemplate(dPersona->get<std::string>(), args, lookupPersonaFormat(defaultLocale_, key));
        node = lookupNode(defaultLocale_, key);  // fall back to default locale bundle
    }
    if (!node) {
        // Last resort: return the key so missing translations are visible
        // rather than crashing or returning empty output.
        DICE_LOG_DEBUG("I18n: missing key '{}' for locale '{}'",
                       key, localeToString(loc));
        return key;
    }
    const std::string raw = node->get<std::string>();
    return renderTemplate(raw, args, trustedTemplateFormat(raw));
}

ContentFormat I18n::trustedTemplateFormat(const std::string& value) {
    return markdown::hasFormatting(value) ? ContentFormat::kMarkdown : ContentFormat::kPlainText;
}

void I18n::beginOutboundCapture() {
    outboundCaptureActive_ = true;
    outboundCaptureMarkdown_ = false;
}

ContentFormat I18n::endOutboundCapture() {
    const bool useMarkdown = outboundCaptureActive_ && outboundCaptureMarkdown_;
    outboundCaptureActive_ = false;
    outboundCaptureMarkdown_ = false;
    return useMarkdown ? ContentFormat::kMarkdown : ContentFormat::kPlainText;
}

void I18n::noteOutboundFormat(ContentFormat format) {
    if (outboundCaptureActive_ && format == ContentFormat::kMarkdown)
        outboundCaptureMarkdown_ = true;
}

std::string I18n::renderTemplate(const std::string& value, const Args& args,
                                 ContentFormat format) {
    noteOutboundFormat(format);
    if (format != ContentFormat::kMarkdown || args.empty()) return interpolate(value, args);
    Args safe;
    for (const auto& [name, arg] : args) safe[name] = markdown::escapeLiteral(arg);
    return interpolate(value, safe);
}

// ═══════════════════════════════════════════════════════════════
// Interpolation
// ═══════════════════════════════════════════════════════════════

std::string I18n::interpolate(const std::string& tmpl, const Args& args) {
    if (args.empty() || tmpl.find('{') == std::string::npos) {
        return tmpl;
    }

    std::string out;
    out.reserve(tmpl.size() + 16);

    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i + 1);
            if (close != std::string::npos) {
                std::string name = tmpl.substr(i + 1, close - i - 1);
                auto it = args.find(name);
                if (it != args.end()) {
                    out += it->second;
                } else {
                    // Unknown placeholder — keep it verbatim for debuggability
                    out += tmpl.substr(i, close - i + 1);
                }
                i = close + 1;
                continue;
            }
        }
        out += tmpl[i++];
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════
// Overrides
// ═══════════════════════════════════════════════════════════════

void I18n::setOverride(Locale loc, const std::string& key, const std::string& value,
                       ContentFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    overrides_[loc][key] = OverrideValue{value, format};
}

void I18n::clearOverride(Locale loc, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = overrides_.find(loc);
    if (it != overrides_.end()) it->second.erase(key);
}

bool I18n::hasOverride(Locale loc, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = overrides_.find(loc);
    return it != overrides_.end() && it->second.count(key) > 0;
}

ContentFormat I18n::getOverrideFormat(Locale loc, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = overrides_.find(loc);
    if (it == overrides_.end()) return ContentFormat::kPlainText;
    auto kt = it->second.find(key);
    return kt == it->second.end() ? ContentFormat::kPlainText : kt->second.format;
}

std::string I18n::getDefault(Locale loc, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const json* node = lookupNode(loc, key);
    if (!node && loc != defaultLocale_) node = lookupNode(defaultLocale_, key);
    return node ? node->get<std::string>() : std::string();
}

ContentFormat I18n::getDefaultFormat(Locale loc, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const json* node = lookupNode(loc, key);
    if (!node && loc != defaultLocale_) node = lookupNode(defaultLocale_, key);
    return node ? trustedTemplateFormat(node->get<std::string>()) : ContentFormat::kPlainText;
}

// ═══════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════

void I18n::setDefaultLocale(Locale loc) {
    std::lock_guard<std::mutex> lock(mutex_);
    defaultLocale_ = loc;
}

Locale I18n::defaultLocale() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return defaultLocale_;
}

std::vector<Locale> I18n::availableLocales() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Locale> result;
    result.reserve(bundles_.size());
    for (const auto& [loc, _] : bundles_) result.push_back(loc);
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Persona layer (骰娘人格切换)
// ═══════════════════════════════════════════════════════════════

const json* I18n::lookupPersonaNode(Locale loc, const std::string& key) const {
    // Persona bundles are flat {dotted-key → string}. No nested traversal.
    auto it = personaBundles_.find(loc);
    if (it == personaBundles_.end()) return nullptr;
    if (!it->second.is_object()) return nullptr;
    auto kit = it->second.find(key);
    if (kit == it->second.end()) return nullptr;
    return kit->is_string() ? &(*kit) : nullptr;
}

ContentFormat I18n::lookupPersonaFormat(Locale loc, const std::string& key) const {
    auto it = personaFormats_.find(loc);
    if (it == personaFormats_.end() || !it->second.is_object()) return ContentFormat::kPlainText;
    auto kit = it->second.find(key);
    if (kit == it->second.end() || !kit->is_string()) return ContentFormat::kPlainText;
    return contentFormatFromString(kit->get<std::string>());
}

void I18n::setPersona(int personaId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activePersonaId_ = personaId;
    DICE_LOG_INFO("I18n: persona set to ID {}", personaId);
}

int I18n::getActivePersonaId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activePersonaId_;
}

bool I18n::loadPersona() {
    // DB-driven personas: data is injected via setPersonaBundles() by
    // PersonaManager. This method is kept for API compatibility but is a no-op.
    return true;
}

void I18n::setPersonaBundles(Locale loc, const json& bundles, const json& formats) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bundles.is_object() && !bundles.empty()) {
        personaBundles_[loc] = bundles;
        personaFormats_[loc] = formats.is_object() ? formats : json::object();
        DICE_LOG_INFO("I18n: persona bundles injected for locale '{}' ({} entries)",
                      localeToString(loc), personaBundles_[loc].size());
    }
}

void I18n::clearPersonaBundles() {
    std::lock_guard<std::mutex> lock(mutex_);
    personaBundles_.clear();
    personaFormats_.clear();
    DICE_LOG_INFO("I18n: persona bundles cleared");
}

}  // namespace dice
