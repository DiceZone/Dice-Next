#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

namespace dice::ai {
using OptionsJson = nlohmann::json;
inline int boundedOption(const OptionsJson& j, const char* key, int fallback, int lo, int hi) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return fallback;
    const double v = j[key].get<double>();
    return std::isfinite(v) ? static_cast<int>((std::clamp)(v, double(lo), double(hi))) : fallback;
}
inline double priceOption(const OptionsJson& j, const char* key, double fallback = 0) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return fallback;
    const double v = j[key].get<double>();
    return std::isfinite(v) ? (std::clamp)(v, 0.0, 1000000.0) : fallback;
}
inline std::string choiceOption(const OptionsJson& j, const char* key,
        std::initializer_list<const char*> choices, const char* fallback) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        const auto value = j[key].get<std::string>();
        for (auto c : choices) if (value == c) return value;
    }
    return fallback;
}
inline int minuteOfDay(const std::string& value) {
    if (value.size() != 5 || value[2] != ':') return -1;
    for (auto i : {0, 1, 3, 4}) if (value[i] < '0' || value[i] > '9') return -1;
    const int h = (value[0]-'0')*10 + value[1]-'0', m = (value[3]-'0')*10 + value[4]-'0';
    return h < 24 && m < 60 ? h*60+m : -1;
}
inline OptionsJson normalizeModelOptions(const OptionsJson& j) {
    using J = OptionsJson;
    const double in = priceOption(j, "price_in"), out = priceOption(j, "price_out");
    J off = j.contains("off_peak") && j["off_peak"].is_object() ? j["off_peak"] : J::object();
    auto time = [&](const char* key, const char* def) {
        auto s = off.contains(key) && off[key].is_string() ? off[key].get<std::string>() : std::string(def);
        return minuteOfDay(s) >= 0 ? s : std::string(def);
    };
    return J{{"thinking", choiceOption(j,"thinking",{"auto","enabled","disabled"},"auto")},
        {"thinking_protocol", choiceOption(j,"thinking_protocol",{"auto","deepseek","reasoning_effort"},"auto")},
        {"reasoning_effort", choiceOption(j,"reasoning_effort",{"low","medium","high"},"high")},
        {"max_tokens", boundedOption(j,"max_tokens",0,0,393216)},
        {"price_in",in}, {"price_out",out}, {"price_cached_in",priceOption(j,"price_cached_in",in)},
        {"off_peak", {{"enabled",off.contains("enabled") && off["enabled"] == true},
            {"start",time("start","00:00")}, {"end",time("end","08:00")},
            {"utc_offset_minutes",boundedOption(off,"utc_offset_minutes",480,-720,840)},
            {"price_in",priceOption(off,"price_in",in)}, {"price_out",priceOption(off,"price_out",out)},
            {"price_cached_in",priceOption(off,"price_cached_in",priceOption(off,"price_in",in))}}}};
}
inline bool isOffPeak(const OptionsJson& options, std::time_t started) {
    const auto& off = options.at("off_peak");
    if (!off.value("enabled",false)) return false;
    const int start = minuteOfDay(off.value("start",std::string())), end = minuteOfDay(off.value("end",std::string()));
    if (start < 0 || end < 0 || start == end) return false;
    auto secs = static_cast<long long>(started) + off.value("utc_offset_minutes",480)*60LL;
    const int now = static_cast<int>((secs%86400+86400)%86400/60);
    return start < end ? now >= start && now < end : now >= start || now < end;
}
inline long long cachedInputTokens(const OptionsJson& usage, long long prompt) {
    auto details = usage.value("prompt_tokens_details", OptionsJson::object());
    long long hit = usage.value("prompt_cache_hit_tokens", details.is_object() ? details.value("cached_tokens",0LL) : 0LL);
    return (std::clamp)(hit, 0LL, (std::max)(0LL,prompt));
}
inline double estimateCost(const OptionsJson& options, long long input, long long output,
        long long cached, std::time_t started) {
    const auto& rate = isOffPeak(options,started) ? options.at("off_peak") : options;
    input = (std::max)(0LL,input); output = (std::max)(0LL,output); cached = (std::clamp)(cached,0LL,input);
    return ((input-cached)*rate.value("price_in",0.0) + cached*rate.value("price_cached_in",0.0)
        + output*rate.value("price_out",0.0))/1.0e6;
}
inline bool officialDeepSeek(const std::string& base) {
    return base == "https://api.deepseek.com" || base.rfind("https://api.deepseek.com/",0) == 0;
}
inline void applyModelOptions(const OptionsJson& options, const std::string& base, const std::string& model,
        OptionsJson& body, const std::string& purpose = {}, const std::string& thinkingOverride = "auto") {
    const int cap = options.value("max_tokens",0);
    if (cap > 0) body["max_tokens"] = purpose == "vision" ? (std::min)(body.value("max_tokens",cap),cap) : cap;
    std::string thinking = thinkingOverride == "auto" ? options.value("thinking",std::string("auto")) : thinkingOverride;
    // Existing models keep their provider defaults; official DeepSeek vision
    // defaults to a short non-thinking description unless explicitly overridden.
    if (thinking == "auto" && purpose == "vision" && officialDeepSeek(base)) thinking = "disabled";
    if (thinking == "auto") return;
    auto protocol = options.value("thinking_protocol",std::string("auto"));
    if (protocol == "auto") protocol = officialDeepSeek(base) || model.rfind("deepseek",0) == 0 ? "deepseek" : "reasoning_effort";
    const auto effort = options.value("reasoning_effort",std::string("high"));
    if (protocol == "deepseek") {
        body["thinking"] = {{"type",thinking}};
        if (thinking == "enabled") body["reasoning_effort"] = effort;
    } else body["reasoning_effort"] = thinking == "disabled" ? "none" : effort;
}
} // namespace dice::ai
