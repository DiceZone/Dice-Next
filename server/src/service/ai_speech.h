#pragma once
// ─── Dice!Next — AI 语音识别 ─────────────────────────────
// 群里发语音时，把语音转成文字交给 AI 对话（触发判断、上下文、回复都用转写结果）。
// 模型由用途分配的「语音识别」决定（OpenAI 兼容 /audio/transcriptions，如 Whisper、
// SenseVoice）。
//
// 语音来源：
//   · OneBot（NapCat 等）：适配器把语音段记在 msg.extra.records；这里调用 get_record
//     要求转成 mp3，依次尝试返回的 base64、本机文件路径、http 链接。
//   · Discord：msg.extra.attachments 里 content_type 为 audio/* 的附件，直接下载。
//
// 配置 dice/ai.speech：{ enabled }

#include "../adapter/adapter_interface.h"
#include "../config/config_manager.h"
#include "ai_usage.h"
#include "chat_image.h"
#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace dice::aispeech {

using json = nlohmann::json;

inline json conf(ConfigManager& cfg) {
    json a = ai::conf(cfg);
    return (a.is_object() && a.contains("speech") && a["speech"].is_object()) ? a["speech"] : json::object();
}
inline bool enabled(ConfigManager& cfg) {
    return ai::enabled(cfg) && conf(cfg).value("enabled", false) && ai::hasModel(cfg, "speech");
}

// 消息里是否带语音（不发网络请求，可在消息线程调用）。
inline bool hasVoice(const Message& msg) {
    if (!msg.extra.is_object()) return false;
    if (msg.extra.contains("records") && msg.extra["records"].is_array() && !msg.extra["records"].empty()) return true;
    if (msg.extra.contains("attachments") && msg.extra["attachments"].is_array())
        for (auto& a : msg.extra["attachments"])
            if (a.is_object() && a.value("content_type", std::string()).rfind("audio/", 0) == 0) return true;
    return false;
}

// 同一条语音只转一次（如消息重放、测试台重复发送）。
inline std::mutex& cacheMutex() { static std::mutex m; return m; }
inline std::map<std::string, std::string>& cache() { static std::map<std::string, std::string> c; return c; }

inline std::filesystem::path tempAudio(const std::string& ext) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto dir = fs::temp_directory_path() / "dicenext-voice";
    fs::create_directories(dir, ec);
    return dir / (std::to_string((long long)std::time(nullptr)) + "_" + chatimg::randToken() + ext);
}

// 取得语音文件到本地临时路径；失败返回空路径。
inline std::filesystem::path fetchAudio(IAdapter* adapter, const Message& msg, std::string& key) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (msg.extra.is_object() && msg.extra.contains("records") && msg.extra["records"].is_array()) {
        const json& rec = msg.extra["records"][0];
        std::string file = rec.value("file", std::string());
        std::string url = rec.value("url", std::string());
        key = !file.empty() ? file : url;
        if (adapter && !file.empty()) {
            json resp = adapter->invokeAction("get_record", {{"file", file}, {"out_format", "mp3"}}, 15000);
            json data = resp.is_object() && resp.contains("data") && resp["data"].is_object() ? resp["data"] : json::object();
            std::string b64 = data.value("base64", std::string());
            if (!b64.empty()) {
                auto out = tempAudio(".mp3");
                std::ofstream(out, std::ios::binary) << drogon::utils::base64Decode(b64);
                if (fs::file_size(out, ec) > 0) return out;
            }
            std::string local = data.value("file", std::string());
            if (!local.empty() && fs::exists(fs::u8path(local), ec)) {
                auto out = tempAudio(fs::u8path(local).extension().string());
                fs::copy_file(fs::u8path(local), out, fs::copy_options::overwrite_existing, ec);
                if (!ec) return out;
            }
            std::string dl = data.value("url", std::string());
            if (dl.rfind("http", 0) == 0) url = dl;
            DICE_LOG_DEBUG_M("AI 语音", "get_record 返回：base64 {}，文件「{}」{}，链接 {}。", b64.empty() ? "无" : "有",
                             local, local.empty() ? "" : (fs::exists(fs::u8path(local), ec) ? "（本机可读）" : "（本机读不到）"),
                             dl.empty() ? "无" : dl);
        }
        if (url.rfind("http", 0) == 0) {
            auto out = tempAudio(".amr");
            if (chatimg::download(url, out.string())) return out;
        }
        return {};
    }
    if (msg.extra.is_object() && msg.extra.contains("attachments") && msg.extra["attachments"].is_array())
        for (auto& a : msg.extra["attachments"]) {
            if (!a.is_object() || a.value("content_type", std::string()).rfind("audio/", 0) != 0) continue;
            std::string url = a.value("url", std::string());
            key = url.substr(0, url.find('?'));
            std::string name = a.value("filename", std::string("voice.ogg"));
            auto dot = name.rfind('.');
            auto out = tempAudio(dot == std::string::npos ? std::string(".ogg") : name.substr(dot));
            if (url.rfind("http", 0) == 0 && chatimg::download(url, out.string())) return out;
            return {};
        }
    return {};
}

// 把消息里的语音转成文字（阻塞，含网络请求：只能在后台线程调用）。失败返回空。
inline std::string transcribe(ConfigManager& cfg, IAdapter* adapter, const Message& msg) {
    namespace fs = std::filesystem;
    if (!enabled(cfg) || !hasVoice(msg)) return "";
    std::string key;
    fs::path audio = fetchAudio(adapter, msg, key);
    if (!key.empty()) {
        std::lock_guard<std::mutex> lk(cacheMutex());
        auto it = cache().find(key);
        if (it != cache().end()) {
            std::error_code ec; if (!audio.empty()) fs::remove(audio, ec);
            DICE_LOG_DEBUG_M("AI 语音", "语音识别命中缓存：{}", it->second);
            return it->second;
        }
    }
    if (audio.empty()) { DICE_LOG_DEBUG_M("AI 语音", "取不到语音文件，跳过语音识别。"); return ""; }
    std::string error;
    std::string text = ai::transcribeFor(cfg, audio, error);
    std::error_code ec; fs::remove(audio, ec);
    if (text.empty()) { DICE_LOG_WARN("语音识别失败：{}", error); return ""; }
    if (!key.empty()) {
        std::lock_guard<std::mutex> lk(cacheMutex());
        if (cache().size() > 500) cache().clear();
        cache()[key] = text;
    }
    DICE_LOG_DEBUG_M("AI 语音", "语音转文字：{}", text);
    return text;
}

}  // namespace dice::aispeech
