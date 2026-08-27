#pragma once
// ─── Dice!Next — Game-log rendering + log-site upload ─────────
// Renders a recorded transcript in the portable .txt format and uploads it.
//
// One entry is emitted for each message:
//   <昵称>(<QQ>) YYYY/MM/DD HH:MM:SS
//   <内容>
//   <blank line>
//
// Upload contract (probed): POST multipart/form-data with fields
//   name        — log display name
//   uniform_id  — stable conversation identifier, e.g. "QQ-Group:<groupId>"
//   file        — the .txt
// Success → {"url": "..."}.

#include "../storage/database.h"
#include "../config/config_manager.h"
#include "../common/logger.h"
#include "../common/utils.h"
#include "../common/subprocess.h"
#include "image_host.h"   // 图片占位符替换为稳定图床 URL
#include "parquet_writer.h"   // 日志导出为 Parquet（zstd）

#include <drogon/HttpAppFramework.h>
#include <nlohmann/json.hpp>
#include <zlib.h>   // 上传 JSON 载荷的压缩实现
#include <sqlite_orm/sqlite_orm.h>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <random>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <array>
#include <ctime>
#include <sstream>

namespace dice::logsvc {

namespace orm = sqlite_orm;
using json = nlohmann::json;

/// 存储时间均为 UTC ISO（含或不含 Z），此处统一换算到配置时区后显示：
/// "2026-06-16T11:27:38" → "2026/06/16 19:27:38"（UTC+8）。无法解析时原样返回。
inline std::string slashTime(const std::string& iso) {
    const std::time_t epoch = utils::parseIsoUtcToEpoch(iso);
    if (epoch == 0) {
        if (iso.size() < 19 || iso[4] != '-' || iso[10] != 'T') return iso;
        std::string s = iso.substr(0, 19);
        s[4] = '/'; s[7] = '/'; s[10] = ' ';
        return s;
    }
    return utils::formatTimeInTimezone(epoch, "%Y/%m/%d %H:%M:%S");
}

/// Render a log transcript in the portable .txt format. @p cfg lets us swap临时图片
/// 链接为稳定图床 URL；传 nullptr 则按原 content 输出（不替换）。
inline std::string renderSealdice(Database& db, int logId, ConfigManager* cfg = nullptr) {
    auto* st = db.getLogStorage();
    if (!st) return "";
    std::string out;
    const std::string selfId = cfg ? cfg->get<std::string>("dice/self_qq", "") : std::string();
    try {
        auto msgs = st->get_all<GameLogMessageRow>(
            orm::where(orm::c(&GameLogMessageRow::logId) == logId),
            orm::order_by(&GameLogMessageRow::id).asc());
        for (auto& m : msgs) {
            std::string uid = m.userId.empty() ? std::string("0") : m.userId;
            // 导出格式以 <骰娘名> 标记骰娘发言；普通玩家保持原格式。
            std::string sender = (!selfId.empty() && uid == selfId) ? "<" + m.sender + ">" : m.sender;
            out += sender + "(" + uid + ") " + slashTime(m.createdAt) + "\n";
            std::string content = (cfg && !m.images.empty())
                ? imghost::substituteImages(m.content, m.images, *cfg) : m.content;
            out += content + "\n\n";
        }
    } catch (...) {}
    return out;
}

/// 把日志渲染成自包含网页（图片 base64 内嵌 / 远端 URL 原样引用）。
/// 网页导出与 `.log type html` 的群文件上传共用这一实现。
inline std::string renderHtml(Database& db, int logId) {
    auto* st = db.getLogStorage();
    if (!st) return "";
    std::string logName = "log" + std::to_string(logId);
    try { auto r = st->get<GameLogRow>(logId); if (!r.name.empty()) logName = r.name; } catch (...) {}
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else o += c; }
        return o;
    };
    auto fileToDataSrc = [](const std::string& path, const std::string& nameForExt) -> std::string {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "";
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string mimeImg = "image/jpeg";
        if (nameForExt.size() > 4 && nameForExt.substr(nameForExt.size() - 4) == ".png") mimeImg = "image/png";
        else if (nameForExt.size() > 4 && nameForExt.substr(nameForExt.size() - 4) == ".gif") mimeImg = "image/gif";
        return "data:" + mimeImg + ";base64," + drogon::utils::base64Encode(
            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    };
    std::ostringstream h;
    // 重复图片只内嵌一份 base64，占位 <img data-i> 由末尾脚本按下标水合，
    // 避免同一张图（如反复出现的表情）被重复存成多个 base64 撑大文件。
    std::map<std::string, int> imgIndex;   // data URI → 数组下标
    std::vector<std::string> imgData;      // 唯一 data URI 列表
    try {
        auto msgs = st->get_all<GameLogMessageRow>(
            orm::where(orm::c(&GameLogMessageRow::logId) == logId),
            orm::order_by(&GameLogMessageRow::id).asc());
        h << "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\"><title>" << esc(logName) << "</title>"
          << "<style>body{font-family:system-ui,sans-serif;max-width:820px;margin:24px auto;padding:0 16px;color:#222;}"
          << "h2{border-bottom:2px solid #6d28d9;padding-bottom:6px;}.msg{margin:8px 0;padding:8px 12px;border-radius:8px;background:#f5f5f7;}"
          << ".meta{font-size:12px;color:#888;}.sender{font-weight:600;color:#6d28d9;}"
          << ".content{white-space:pre-wrap;margin-top:2px;}img{max-width:320px;max-height:320px;display:block;margin-top:6px;border-radius:6px;border:1px solid #ddd;}</style>"
          << "</head><body><h2>" << esc(logName) << "</h2>";
        for (auto& m : msgs) {
            h << "<div class=\"msg\"><div class=\"meta\"><span class=\"sender\">" << esc(m.sender)
              << "</span> &nbsp;" << esc(slashTime(m.createdAt)) << "</div><div class=\"content\">" << esc(m.content) << "</div>";
            if (!m.images.empty()) {
                try {
                    auto arr = json::parse(m.images);
                    if (arr.is_array()) for (auto& e : arr) {
                        std::string u = e.is_string() ? e.get<std::string>() : "";
                        if (u.empty()) continue;
                        std::string src;
                        std::string norm = u; for (auto& ch : norm) if (ch == '\\') ch = '/';
                        if (u.rfind("http", 0) == 0) src = u;                          // 远端
                        else if (norm.rfind("data/assets/", 0) == 0) src = fileToDataSrc(norm, norm);   // 本地资产
                        else src = fileToDataSrc("data/logs/images/" + u, u);          // 本地落地图
                        if (src.empty()) continue;
                        if (src.rfind("data:", 0) == 0) {   // 本地图 base64 去重引用
                            auto it = imgIndex.find(src);
                            int idx;
                            if (it == imgIndex.end()) { idx = (int)imgData.size(); imgIndex[src] = idx; imgData.push_back(src); }
                            else idx = it->second;
                            h << "<img data-i=\"" << idx << "\" alt=\"image\">";
                        } else {
                            h << "<img src=\"" << src << "\" alt=\"image\">";   // 远端 URL 原样引用
                        }
                    }
                } catch (...) {}
            }
            h << "</div>";
        }
        // 唯一图片 base64 一次性声明为 JS 数组，占位 img 按下标水合（结果不变、省体积）。
        if (!imgData.empty()) {
            h << "<script>var D=[";
            for (size_t i = 0; i < imgData.size(); ++i) { if (i) h << ','; h << '"' << imgData[i] << '"'; }
            h << "];document.querySelectorAll('img[data-i]').forEach(function(e){e.src=D[+e.getAttribute('data-i')];});</script>";
        }
        h << "</body></html>";
    } catch (...) { return ""; }
    return h.str();
}

/// 官方日志站（SealDice 兼容端点）。上传目标非此地址时提示「并非官方日志站」。
inline constexpr const char* kOfficialLogsite = "https://log-api.dice.zone/api/dice/log";

/// The configured upload endpoint (系统设置可改，默认官方站)。
inline std::string uploadUrl(ConfigManager& cfg) {
    std::string u = cfg.get<std::string>("dice/logsite_url", std::string(kOfficialLogsite));
    return u.empty() ? std::string(kOfficialLogsite) : u;
}

/// 上传协议：dicenext（DiceNext 专属 zstd JSON，默认）/ seal（SealDice V1）/
/// seal_v105（Parquet）/ legacy（旧版多段 txt POST，自建旧端点用）。
inline std::string uploadFormat(ConfigManager& cfg) {
    std::string f = cfg.get<std::string>("dice/logsite_format", std::string("dicenext"));
    if (f == "legacy" || f == "seal_v105" || f == "seal" || f == "dicenext") return f;
    return std::string("dicenext");
}

/// Build the upload uniform_id. The log site requires it to be UNIQUE per upload;
/// matching Shia's reference plugin we use "<groupId>:<unix timestamp>".
inline std::string makeUniformId(const std::string& groupId) {
    return groupId + ":" + std::to_string((long long)std::time(nullptr));
}

/// 把日志渲染成 SealDice 标准 items 数组（海豹染色器/日志站可解析）。
/// 与 sealdice-core model/log.go 的 LogOneItem json tag 逐字段对齐。
/// @p selfId 骰娘账号，用于标注 isDice（骰娘消息染色）。
inline json renderSealItems(Database& db, int logId, ConfigManager* cfg, const std::string& selfId) {
    json items = json::array();
    auto* st = db.getLogStorage();
    if (!st) return items;
    // 存储时间统一为 UTC ISO → unix 秒（此前误按本地时间 mktime 解析，
    // 导致上传日志的时间被时区偏移错位——issue #16）。
    auto toEpoch = [](const std::string& iso) -> long long {
        return (long long)utils::parseIsoUtcToEpoch(iso);
    };
    try {
        auto msgs = st->get_all<GameLogMessageRow>(
            orm::where(orm::c(&GameLogMessageRow::logId) == logId),
            orm::order_by(&GameLogMessageRow::id).asc());
        long long i = 0;
        for (auto& m : msgs) {
            std::string uid = m.userId.empty() ? std::string("0") : m.userId;
            std::string content = (cfg && !m.images.empty())
                ? imghost::substituteImages(m.content, m.images, *cfg) : m.content;
            items.push_back(json{
                {"id", ++i},
                {"nickname", m.sender},
                {"IMUserId", "QQ:" + uid},
                {"time", toEpoch(m.createdAt)},
                {"message", content},
                {"isDice", !selfId.empty() && uid == selfId},
                {"commandId", 0},
                {"commandInfo", nullptr},
                {"rawMsgId", ""},
                {"uniformId", "QQ:" + uid},
                {"channel", ""},
            });
        }
    } catch (...) {}
    return items;
}

/// zlib 整块压缩（与 Go zlib.NewWriter 同格式）。失败返回空串。
inline std::string zlibCompress(const std::string& in) {
    uLongf cap = compressBound((uLong)in.size());
    std::string out;
    out.resize(cap);
    if (compress2(reinterpret_cast<Bytef*>(&out[0]), &cap,
                  reinterpret_cast<const Bytef*>(in.data()), (uLong)in.size(),
                  Z_BEST_COMPRESSION) != Z_OK)
        return "";
    out.resize(cap);
    return out;
}

/// SealDice V1 协议上传（与海豹核心 storylog/upload_v1.go 对齐）：
/// PUT multipart {name, uniform_id="QQ-Group:<gid>", client="SealDice", version="101",
/// file=<zlib(JSON{items,version})> filename="log-zlib-compressed"}。响应 {"url":...}。
inline void uploadSeal(const std::string& fullUrl, const std::string& name,
                       const std::string& groupId, json items,
                       std::function<void(bool, std::string)> cb) {
    std::thread([fullUrl, name, groupId, items = std::move(items), cb]() {
        namespace fs = std::filesystem;
        bool ok = false;
        std::string res;
        std::string tag;
        { std::random_device rd; std::mt19937 g(rd()); tag = std::to_string(g()); }
        fs::path binPath  = fs::temp_directory_path() / ("dnseal_" + tag + ".bin");
        fs::path namePath = fs::temp_directory_path() / ("dnname_" + tag + ".txt");
        try {
            json payload{{"items", std::move(items)}, {"version", 101}};
            std::string z = zlibCompress(payload.dump());
            if (z.empty()) throw std::runtime_error("zlib compress failed");
            { std::ofstream f(binPath, std::ios::binary); f.write(z.data(), (std::streamsize)z.size()); }
            { std::ofstream f(namePath, std::ios::binary); f << name; }   // value-from-file 免转义
            std::string out = dice::proc::curl({
                "-s", "-S", "-X", "PUT",
                "-F", "name=<" + namePath.string(),
                "-F", "uniform_id=QQ-Group:" + groupId,
                "-F", "client=SealDice",
                "-F", "version=101",
                "-F", "file=@" + binPath.string() + ";filename=log-zlib-compressed",
                fullUrl}).output;
            try {
                auto j = json::parse(out);
                std::string url = j.value("url", std::string());
                if (!url.empty()) { ok = true; res = url; }
                else res = j.value("message", out.substr(0, 200));
            } catch (...) { res = out.empty() ? std::string("no response (curl missing?)") : ("bad response: " + out.substr(0, 200)); }
        } catch (const std::exception& e) { res = std::string("error: ") + e.what(); }
        std::error_code ec; fs::remove(binPath, ec); fs::remove(namePath, ec);
        drogon::app().getLoop()->queueInLoop([cb, ok, res]() { cb(ok, res); });
    }).detach();
}

/// SealDice V105：把日志渲染成 Parquet 行（列名/类型对齐 model/log.go LogOneItemParquet）。
inline std::vector<parquetw::LogRow> renderLogRows(Database& db, int logId, ConfigManager* cfg, const std::string& selfId) {
    std::vector<parquetw::LogRow> rows;
    auto* st = db.getLogStorage();
    if (!st) return rows;
    auto toEpoch = [](const std::string& iso) -> long long {
        return (long long)utils::parseIsoUtcToEpoch(iso);
    };
    try {
        auto msgs = st->get_all<GameLogMessageRow>(
            orm::where(orm::c(&GameLogMessageRow::logId) == logId),
            orm::order_by(&GameLogMessageRow::id).asc());
        long long i = 0;
        for (auto& m : msgs) {
            std::string uid = m.userId.empty() ? std::string("0") : m.userId;
            std::string content = (cfg && !m.images.empty())
                ? imghost::substituteImages(m.content, m.images, *cfg) : m.content;
            parquetw::LogRow r;
            r.id = (uint64_t)(++i);
            r.nickname = m.sender;
            r.imUserId = "QQ:" + uid;
            r.time = (int64_t)toEpoch(m.createdAt);
            r.message = content;
            r.isDice = (!selfId.empty() && uid == selfId);
            r.commandId = 0;
            r.commandInfo = "";
            r.uniformId = "QQ:" + uid;
            rows.push_back(std::move(r));
        }
    } catch (...) {}
    return rows;
}

/// SealDice V105：日志 → Parquet 文件字节（zstd 页压缩）。空返回空串。
inline std::string renderSealParquet(Database& db, int logId, ConfigManager* cfg, const std::string& selfId) {
    return parquetw::buildLogParquet(renderLogRows(db, logId, cfg, selfId));
}

/// SealDice V105 协议上传：PUT multipart {name, uniform_id, client="Parquet",
/// version="105", file=<parquet(zstd)> filename="log-zlib-compressed"}。响应 {"url":...}。
/// 与海豹 storylog/upload_v105.go 的 uploadToBackendParquet 对齐。
inline void uploadSealV105(const std::string& fullUrl, const std::string& name,
                           const std::string& groupId, std::string parquetBytes,
                           std::function<void(bool, std::string)> cb) {
    std::thread([fullUrl, name, groupId, parquetBytes = std::move(parquetBytes), cb]() {
        namespace fs = std::filesystem;
        bool ok = false;
        std::string res;
        std::string tag;
        { std::random_device rd; std::mt19937 g(rd()); tag = std::to_string(g()); }
        fs::path binPath  = fs::temp_directory_path() / ("dnpq_" + tag + ".parquet");
        fs::path namePath = fs::temp_directory_path() / ("dnname_" + tag + ".txt");
        try {
            if (parquetBytes.empty()) throw std::runtime_error("empty parquet");
            { std::ofstream f(binPath, std::ios::binary); f.write(parquetBytes.data(), (std::streamsize)parquetBytes.size()); }
            { std::ofstream f(namePath, std::ios::binary); f << name; }   // value-from-file 免转义
            std::string out = dice::proc::curl({
                "-s", "-S", "-X", "PUT",
                "-F", "name=<" + namePath.string(),
                "-F", "uniform_id=QQ-Group:" + groupId,
                "-F", "client=Parquet",
                "-F", "version=105",
                "-F", "file=@" + binPath.string() + ";filename=log-zlib-compressed",
                fullUrl}).output;
            try {
                auto j = json::parse(out);
                std::string url = j.value("url", std::string());
                if (!url.empty()) { ok = true; res = url; }
                else res = j.value("message", out.substr(0, 200));
            } catch (...) { res = out.empty() ? std::string("no response (curl missing?)") : ("bad response: " + out.substr(0, 200)); }
        } catch (const std::exception& e) { res = std::string("error: ") + e.what(); }
        std::error_code ec; fs::remove(binPath, ec); fs::remove(namePath, ec);
        drogon::app().getLoop()->queueInLoop([cb, ok, res]() { cb(ok, res); });
    }).detach();
}

/// DiceNext 专属格式：items JSON（{items,version:105}）→ zstd 压缩字节。
/// 供普通日志与跨团聚合日志共用，避免跨团上传退回不兼容的旧协议。
inline std::string packDiceNextItems(json items) {
    json payload{{"items", std::move(items)}, {"version", 105}};
    std::string js = payload.dump();
    if (js.empty()) return {};
    size_t bound = ZSTD_compressBound(js.size());
    std::string out; out.resize(bound ? bound : 1);
    size_t n = ZSTD_compress(&out[0], out.size(), js.data(), js.size(), 19);   // 高压缩级（日志小、异步）
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

/// 比 SealDice V1 的 zlib 更小，且三端都轻量（前端用 fzstd 解压，无需 parquet/WASM）。
inline std::string renderDiceNextData(Database& db, int logId, ConfigManager* cfg, const std::string& selfId) {
    return packDiceNextItems(renderSealItems(db, logId, cfg, selfId));
}

/// DiceNext 专属上传：PUT multipart {name, uniform_id, client="DiceNext", file=<zstd(JSON)>}。
/// 日志站透传 client=DiceNext，染色器据此用 fzstd 解压。响应 {"url":...}。
inline void uploadDiceNext(const std::string& fullUrl, const std::string& name,
                           const std::string& groupId, std::string zdata,
                           std::function<void(bool, std::string)> cb) {
    std::thread([fullUrl, name, groupId, zdata = std::move(zdata), cb]() {
        namespace fs = std::filesystem;
        bool ok = false;
        std::string res;
        std::string tag;
        { std::random_device rd; std::mt19937 g(rd()); tag = std::to_string(g()); }
        fs::path binPath  = fs::temp_directory_path() / ("dndn_" + tag + ".zst");
        fs::path namePath = fs::temp_directory_path() / ("dnname_" + tag + ".txt");
        try {
            if (zdata.empty()) throw std::runtime_error("empty data");
            { std::ofstream f(binPath, std::ios::binary); f.write(zdata.data(), (std::streamsize)zdata.size()); }
            { std::ofstream f(namePath, std::ios::binary); f << name; }
            std::string out = dice::proc::curl({
                "-s", "-S", "-X", "PUT",
                "-F", "name=<" + namePath.string(),
                "-F", "uniform_id=QQ-Group:" + groupId,
                "-F", "client=DiceNext",
                "-F", "file=@" + binPath.string() + ";filename=log-zstd-json",
                fullUrl}).output;
            try {
                auto j = json::parse(out);
                std::string url = j.value("url", std::string());
                if (!url.empty()) { ok = true; res = url; }
                else res = j.value("message", out.substr(0, 200));
            } catch (...) { res = out.empty() ? std::string("no response (curl missing?)") : ("bad response: " + out.substr(0, 200)); }
        } catch (const std::exception& e) { res = std::string("error: ") + e.what(); }
        std::error_code ec; fs::remove(binPath, ec); fs::remove(namePath, ec);
        drogon::app().getLoop()->queueInLoop([cb, ok, res]() { cb(ok, res); });
    }).detach();
}

/// Async upload of @p txt. Calls @p cb(success, urlOrError) on the drogon loop.
/// @p uniformId e.g. "QQ-Group:123456". Uses the `curl` CLI (bundled on Win10/11,
/// standard on Linux) — proven reliable against the log site and avoids drogon's
/// flaky Windows async-DNS for outbound HTTPS. Runs in a detached thread and
/// posts the result back onto the app loop.
inline void upload(const std::string& fullUrl, const std::string& name,
                   const std::string& uniformId, const std::string& txt,
                   std::function<void(bool, std::string)> cb) {
    std::thread([fullUrl, name, uniformId, txt, cb]() {
        namespace fs = std::filesystem;
        bool ok = false;
        std::string res;
        std::string tag;
        { std::random_device rd; std::mt19937 g(rd()); tag = std::to_string(g()); }
        fs::path txtPath  = fs::temp_directory_path() / ("dnlog_" + tag + ".txt");
        fs::path namePath = fs::temp_directory_path() / ("dnname_" + tag + ".txt");
        try {
            { std::ofstream f(txtPath, std::ios::binary);  f << txt; }
            { std::ofstream f(namePath, std::ios::binary); f << name; }   // value-from-file avoids quoting issues
            // curl reads the `name` field value from a file (-F "name=<path"),
            // so arbitrary names (Chinese/spaces) need no shell escaping.
            std::string out = dice::proc::curl({
                "-s", "-S", "-X", "POST",
                "-F", "name=<" + namePath.string(),
                "-F", "uniform_id=" + uniformId,
                "-F", "file=@" + txtPath.string() + ";type=text/plain",
                fullUrl}).output;
            try {
                auto j = json::parse(out);
                if (j.value("success", true) != false && j.contains("url")) {
                    ok = true; res = j.value("url", std::string());
                } else {
                    res = j.value("message", std::string("upload failed"));
                }
            } catch (...) { res = out.empty() ? std::string("no response (curl missing?)") : ("bad response: " + out.substr(0, 200)); }
        } catch (const std::exception& e) { res = std::string("error: ") + e.what(); }
        std::error_code ec; fs::remove(txtPath, ec); fs::remove(namePath, ec);
        drogon::app().getLoop()->queueInLoop([cb, ok, res]() { cb(ok, res); });
    }).detach();
}

}  // namespace dice::logsvc
