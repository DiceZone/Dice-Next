#pragma once
// Per-AppID index of QQ Official 素材管理 images. The platform holds at most
// 100 images; Dice remembers whether each slot is a send-time cache (LRU) or a
// file-manager upload that must not be evicted automatically.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <limits>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dice::qq_media {

using json = nlohmann::json;
namespace fs = std::filesystem;

inline constexpr int kQuota = 100;
inline constexpr const char* kCache = "cache";
inline constexpr const char* kManual = "manual";
inline constexpr const char* kUntracked = "untracked";

inline std::string normalizeSource(std::string_view value) {
    return value == kCache ? kCache : kManual;
}

inline std::string fileStem(const std::string& appId) {
    std::string out;
    out.reserve(appId.size());
    for (unsigned char c : appId) {
        if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
        else out += '_';
    }
    return out.empty() ? "unknown" : out;
}

struct Entry {
    std::string imageId;
    std::string url;
    std::string md5;
    std::string source = kCache;
    std::string fileName;
    int64_t lastUsed = 0;
    int64_t createdAt = 0;
};

inline int64_t nowSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

inline int64_t jsonInt64(const json& row, const char* key, int64_t fallback = 0) {
    const auto it = row.find(key);
    if (it == row.end()) return fallback;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) {
        const auto number = it->get<uint64_t>();
        return number <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ? static_cast<int64_t>(number) : fallback;
    }
    if (it->is_string()) {
        try {
            std::size_t idx = 0;
            const auto number = std::stoll(it->get_ref<const std::string&>(), &idx, 10);
            if (idx == it->get_ref<const std::string&>().size()) return number;
        } catch (...) {}
    }
    return fallback;
}

class Index {
public:
    Index() = default;
    Index(fs::path directory, std::string appId)
        : dir_(std::move(directory)), appId_(std::move(appId)) {
        load();
    }

    const std::string& appId() const noexcept { return appId_; }

    std::optional<Entry> byMd5(const std::string& md5) const {
        if (md5.empty()) return std::nullopt;
        std::lock_guard lock(mu_);
        for (const auto& entry : items_) if (entry.md5 == md5) return entry;
        return std::nullopt;
    }

    std::optional<Entry> byId(const std::string& imageId) const {
        if (imageId.empty()) return std::nullopt;
        std::lock_guard lock(mu_);
        for (const auto& entry : items_) if (entry.imageId == imageId) return entry;
        return std::nullopt;
    }

    std::vector<Entry> all() const {
        std::lock_guard lock(mu_);
        return items_;
    }

    void upsert(Entry entry) {
        if (entry.imageId.empty()) return;
        entry.source = normalizeSource(entry.source);
        const auto now = nowSeconds();
        if (entry.createdAt <= 0) entry.createdAt = now;
        std::lock_guard lock(mu_);
        for (auto& existing : items_) {
            const bool same = existing.imageId == entry.imageId
                || (!entry.md5.empty() && existing.md5 == entry.md5);
            if (!same) continue;
            if (existing.source == kManual && entry.source == kCache) entry.source = kManual;
            if (entry.lastUsed <= 0) entry.lastUsed = existing.lastUsed;
            if (entry.url.empty()) entry.url = existing.url;
            if (entry.md5.empty()) entry.md5 = existing.md5;
            if (entry.fileName.empty()) entry.fileName = existing.fileName;
            if (entry.createdAt > existing.createdAt && existing.createdAt > 0)
                entry.createdAt = existing.createdAt;
            existing = std::move(entry);
            saveLocked();
            return;
        }
        if (entry.lastUsed <= 0) entry.lastUsed = now;
        items_.push_back(std::move(entry));
        saveLocked();
    }

    void remove(const std::string& imageId) {
        if (imageId.empty()) return;
        std::lock_guard lock(mu_);
        const auto size = items_.size();
        items_.erase(std::remove_if(items_.begin(), items_.end(),
            [&](const Entry& entry) { return entry.imageId == imageId; }), items_.end());
        if (items_.size() != size) saveLocked();
    }

    void markUsed(const std::string& imageId) {
        if (imageId.empty()) return;
        std::lock_guard lock(mu_);
        for (auto& entry : items_) if (entry.imageId == imageId) {
            entry.lastUsed = nowSeconds();
            saveLocked();
            return;
        }
    }

    void setSource(const std::string& imageId, std::string source) {
        if (imageId.empty()) return;
        source = normalizeSource(source);
        std::lock_guard lock(mu_);
        for (auto& entry : items_) if (entry.imageId == imageId) {
            entry.source = std::move(source);
            saveLocked();
            return;
        }
    }

    std::optional<Entry> oldestCache() const {
        std::lock_guard lock(mu_);
        const Entry* best = nullptr;
        for (const auto& entry : items_) {
            if (entry.source != kCache) continue;
            if (!best
                || entry.lastUsed < best->lastUsed
                || (entry.lastUsed == best->lastUsed && entry.createdAt < best->createdAt))
                best = &entry;
        }
        return best ? std::optional<Entry>(*best) : std::nullopt;
    }

private:
    fs::path filePath() const {
        return dir_.empty() || appId_.empty() ? fs::path() : dir_ / (fileStem(appId_) + ".json");
    }

    void load() {
        const auto path = filePath();
        if (path.empty()) return;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) return;
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        json body = json::parse(in, nullptr, false);
        if (!body.is_object() || !body["images"].is_array()) return;
        items_.clear();
        for (const auto& row : body["images"]) {
            if (!row.is_object()) continue;
            Entry entry;
            entry.imageId = row.value("image_id", std::string());
            entry.url = row.value("url", std::string());
            entry.md5 = row.value("md5", std::string());
            entry.source = normalizeSource(row.value("source", std::string(kCache)));
            entry.fileName = row.value("file_name", std::string());
            entry.lastUsed = jsonInt64(row, "last_used");
            entry.createdAt = jsonInt64(row, "created_at");
            if (!entry.imageId.empty()) items_.push_back(std::move(entry));
        }
    }

    void saveLocked() const {
        const auto path = filePath();
        if (path.empty()) return;
        json images = json::array();
        for (const auto& entry : items_) {
            images.push_back({
                {"image_id", entry.imageId}, {"url", entry.url}, {"md5", entry.md5},
                {"source", entry.source}, {"file_name", entry.fileName},
                {"last_used", entry.lastUsed}, {"created_at", entry.createdAt}
            });
        }
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const auto tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << json{{"images", std::move(images)}}.dump();
        }
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(path, ec);
            fs::rename(tmp, path, ec);
        }
    }

    fs::path dir_;
    std::string appId_;
    mutable std::mutex mu_;
    std::vector<Entry> items_;
};

}  // namespace dice::qq_media
