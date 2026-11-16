#include "test_framework.h"
#include "../src/service/qq_media_index.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace dice::qq_media;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("dice-qq-media-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

Entry cache(const char* id, int64_t used, const char* md5 = "a") {
    Entry e; e.imageId = id; e.url = std::string("qqbot-media://") + id;
    e.md5 = md5; e.source = kCache; e.lastUsed = used; e.createdAt = used;
    return e;
}
}

TEST(QqMediaIndex, EvictsOldestCacheAndNeverPicksManual) {
    Index index({}, "app");
    index.upsert(cache("old", 10, "md1"));
    index.upsert(cache("mid", 20, "md2"));
    Entry keep; keep.imageId = "keep"; keep.url = "qqbot-media://keep";
    keep.source = kManual; keep.lastUsed = 1; keep.md5 = "md3";
    index.upsert(keep);
    const auto victim = index.oldestCache();
    ASSERT_TRUE(victim.has_value());
    ASSERT_EQ(victim->imageId, std::string("old"));
    index.remove("old");
    const auto next = index.oldestCache();
    ASSERT_TRUE(next.has_value());
    ASSERT_EQ(next->imageId, std::string("mid"));
    index.remove("mid");
    ASSERT_FALSE(index.oldestCache().has_value());
}

TEST(QqMediaIndex, CacheUploadDoesNotDowngradeManualAndPersists) {
    TempDir dir;
    {
        Index index(dir.path, "bot-1");
        Entry manual; manual.imageId = "img"; manual.url = "qqbot-media://img";
        manual.md5 = std::string(32, 'a'); manual.source = kManual; manual.fileName = "a.png";
        index.upsert(manual);
        Entry cacheHit; cacheHit.imageId = "img"; cacheHit.url = "qqbot-media://img";
        cacheHit.md5 = std::string(32, 'a'); cacheHit.source = kCache;
        index.upsert(cacheHit);
        const auto stored = index.byMd5(std::string(32, 'a'));
        ASSERT_TRUE(stored.has_value());
        ASSERT_EQ(stored->source, std::string(kManual));
        index.markUsed("img");
    }
    Index reloaded(dir.path, "bot-1");
    const auto stored = reloaded.byId("img");
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->source, std::string(kManual));
    ASSERT_TRUE(stored->lastUsed > 0);
}

TEST(QqMediaIndex, FileStemSanitizesAppId) {
    ASSERT_EQ(fileStem("abc"), std::string("abc"));
    ASSERT_EQ(fileStem("a/b\\c"), std::string("a_b_c"));
}

TEST(QqMediaIndex, LoadsNumericStringsWithoutThrowing) {
    TempDir dir;
    {
        std::ofstream out(dir.path / "bot.json", std::ios::binary);
        out << R"({"images":[{"image_id":"x","url":"qqbot-media://x","md5":"","source":"cache","file_name":"a.png","last_used":"12","created_at":"3"}]})";
    }
    Index index(dir.path, "bot");
    const auto stored = index.byId("x");
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->lastUsed, int64_t(12));
    ASSERT_EQ(stored->createdAt, int64_t(3));
}
