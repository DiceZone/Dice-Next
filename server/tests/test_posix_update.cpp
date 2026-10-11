#ifndef _WIN32
#include "test_framework.h"
#include "../src/platform/posix_update.h"
#include "../src/service/update_archive.h"
#include <zlib.h>
#include <array>
#include <cstdio>
#include <functional>

namespace {
namespace fs = std::filesystem;
namespace posix = dice::update::posix;
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("dice-posix-update-" + std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(root); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
void file(const fs::path& path, const std::string& text, bool executable = false) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
    fs::permissions(path, executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
        : fs::perms::owner_read | fs::perms::owner_write);
}
std::string contents(const fs::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void package(const fs::path& root) {
    for (const auto* item : {"dice-next", "dice-next-server", "start.sh"}) file(root / item, "new", true);
    for (const auto* item : {"i18n/zh-Hans.json", "web/dist/index.html", "docs/roadmap.md",
                             "data/plugins/example.js", "data/helpdoc/example.json"}) file(root / item, "new");
    file(root / "update.json", R"({"schema":1,"tag":"v99.0.0-beta.900","version":"99.0.0","build":900})");
}
bool throws(const std::function<void()>& callback) {
    try { callback(); return false; } catch (...) { return true; }
}
struct Entry {
    std::string name, content;
    char type;
    std::string link;
    bool gnu;
    Entry(std::string name, std::string content, char type = '0', std::string link = {}, bool gnu = false)
        : name(std::move(name)), content(std::move(content)), type(type), link(std::move(link)), gnu(gnu) {}
};
void archive(const fs::path& target, const std::vector<Entry>& entries) {
    auto gz = gzopen(target.c_str(), "wb");
    for (const auto& entry : entries) {
        std::array<char, 512> header{};
        std::memcpy(header.data(), entry.name.data(), std::min<size_t>(entry.name.size(), 100));
        std::snprintf(header.data() + 100, 8, "%07o", 0755);
        std::snprintf(header.data() + 124, 12, "%011llo", static_cast<unsigned long long>(entry.content.size()));
        header[156] = entry.type;
        std::memcpy(header.data() + 157, entry.link.data(), std::min<size_t>(entry.link.size(), 100));
        std::memcpy(header.data() + 257, "ustar", 5);
        if (entry.gnu) {
            header[262] = ' ';
            std::memcpy(header.data() + 345, "00000000042", 11); // GNU atime, not a pathname prefix.
        }
        unsigned sum = 0;
        for (size_t i = 0; i < header.size(); ++i) sum += i >= 148 && i < 156 ? ' ' : static_cast<unsigned char>(header[i]);
        std::snprintf(header.data() + 148, 8, "%06o", sum);
        header[155] = ' ';
        gzwrite(gz, header.data(), header.size());
        gzwrite(gz, entry.content.data(), entry.content.size());
        std::array<char, 512> pad{};
        gzwrite(gz, pad.data(), (512 - entry.content.size() % 512) % 512);
    }
    std::array<char, 1024> end{};
    gzwrite(gz, end.data(), end.size());
    gzclose(gz);
}
std::string paxPath(const std::string& path) {
    const std::string body = " path=" + path + '\n';
    size_t length = body.size() + 1;
    while (std::to_string(length).size() + body.size() != length) length = std::to_string(length).size() + body.size();
    return std::to_string(length) + body;
}
}

TEST(PosixUpdate, ReplacesProgramAndBundledPluginsButPreservesUserData) {
    Fixture f;
    package(f.root / "updates/pending");
    file(f.root / "dice-next-server", "old", true);
    file(f.root / "config/dice.json", "private configuration");
    file(f.root / "data/dice.db", "database");
    file(f.root / "data/decks/custom.json", "user deck");
    file(f.root / "data/plugins/example.js", "old example");
    file(f.root / "data/plugins/custom.js", "private plugin");
    file(f.root / "data/helpdoc/custom.json", "private help");
    file(f.root / "data/helpdoc/example.json", "old example");
    posix::Transaction transaction(f.root);
    transaction.begin();
    ASSERT_TRUE(transaction.active());
    ASSERT_EQ(contents(f.root / "dice-next-server"), std::string("new"));
    ASSERT_EQ(contents(f.root / "data/plugins/example.js"), std::string("new"));
    transaction.commit();
    ASSERT_FALSE(transaction.active());
    ASSERT_FALSE(fs::exists(f.root / "updates/pending"));
    ASSERT_EQ(contents(f.root / "data/dice.db"), std::string("database"));
    ASSERT_EQ(contents(f.root / "config/dice.json"), std::string("private configuration"));
    ASSERT_EQ(contents(f.root / "data/decks/custom.json"), std::string("user deck"));
    ASSERT_EQ(contents(f.root / "data/plugins/custom.js"), std::string("private plugin"));
    ASSERT_EQ(contents(f.root / "data/helpdoc/custom.json"), std::string("private help"));
    ASSERT_EQ(contents(f.root / "data/helpdoc/example.json"), std::string("new"));
}

TEST(PosixUpdate, InterruptedReplacementRollsBackInReverseAndDoesNotRetry) {
    Fixture f;
    package(f.root / "updates/pending");
    file(f.root / "dice-next-server", "old", true);
    file(f.root / "web/dist/index.html", "old frontend");
    posix::Transaction first(f.root);
    first.begin();
    posix::Transaction recovered(f.root);
    recovered.recover();
    ASSERT_EQ(contents(f.root / "dice-next-server"), std::string("old"));
    ASSERT_EQ(contents(f.root / "web/dist/index.html"), std::string("old frontend"));
    ASSERT_FALSE(fs::exists(f.root / "dice-next"));
    ASSERT_FALSE(fs::exists(f.root / "updates/pending"));
    ASSERT_FALSE(recovered.active());
    ASSERT_FALSE(posix::readJson(f.root / "updates/last-result.json").at("success").get<bool>());
    recovered.recover(); // Repeated recovery is harmless.
}

TEST(PosixUpdate, RecoveryHandlesCrashBetweenBackupAndReplacement) {
    Fixture f;
    package(f.root / "updates/pending");
    file(f.root / "dice-next-server", "old", true);
    fs::create_directories(f.root / "updates/rollbacks/test");
    posix::writeJson(f.root / "updates/transaction.json", {
        {"backup", "updates/rollbacks/test"}, {"metadata", nlohmann::json::object()},
        {"moves", nlohmann::json::array({{{"path", "dice-next-server"}, {"existing", true}}})}});
    fs::rename(f.root / "dice-next-server", f.root / "updates/rollbacks/test/dice-next-server");
    posix::Transaction(f.root).recover();
    ASSERT_EQ(contents(f.root / "dice-next-server"), std::string("old"));
}

TEST(PosixUpdate, RefusesSymlinkedTargetsAndUnsafeRecoveryRecords) {
    Fixture f, outside;
    package(f.root / "updates/pending");
    file(outside.root / "custom.js", "private");
    fs::create_directory(f.root / "data");
    fs::create_directory_symlink(outside.root, f.root / "data/plugins");
    posix::Transaction transaction(f.root);
    ASSERT_TRUE(throws([&] { transaction.begin(); }));
    ASSERT_FALSE(transaction.active());
    ASSERT_EQ(contents(outside.root / "custom.js"), std::string("private"));
    posix::writeJson(f.root / "updates/transaction.json", {
        {"backup", "updates/rollbacks/test"}, {"metadata", nlohmann::json::object()},
        {"moves", nlohmann::json::array({{{"path", "config/dice.json"}, {"existing", true}}})}});
    ASSERT_TRUE(throws([&] { transaction.recover(); }));
}

TEST(PosixUpdate, AllowsOnlyContainedLibrarySymlinks) {
    Fixture f;
    package(f.root);
    file(f.root / "lib/libtest.so.1", "library");
    fs::create_symlink("libtest.so.1", f.root / "lib/libtest.so");
    posix::validatePackage(f.root);
    fs::create_symlink("../../config/dice.json", f.root / "lib/escape.so");
    ASSERT_TRUE(throws([&] { posix::validatePackage(f.root); }));
}

TEST(PosixArchive, ExtractsFilesAndPaxUnicodePathsWithoutShell) {
    Fixture f;
    fs::create_directory(f.root / "out");
    const std::string path = "DiceNext-beta/i18n/中文文件.json";
    archive(f.root / "test.tar.gz", {{"pax", paxPath(path), 'x'}, {"fallback", "text"},
        {"DiceNext-beta/lib/libtest.so.1", "library"}, {"DiceNext-beta/lib/libtest.so", "", '2', "libtest.so.1"}});
    std::string error;
    ASSERT_TRUE(dice::update::extractPosixUpdate(f.root / "test.tar.gz", f.root / "out", error));
    ASSERT_EQ(contents(f.root / "out" / path), std::string("text"));
    ASSERT_TRUE(fs::is_symlink(f.root / "out/DiceNext-beta/lib/libtest.so"));
}

TEST(PosixArchive, ExtractsGnuLongPathsWithoutConfusingTimestampsWithUstarPrefix) {
    Fixture f;
    fs::create_directory(f.root / "out");
    const std::string path = "DiceNext-beta/docs/" + std::string(150, 'a') + ".md";
    archive(f.root / "gnu.tar.gz", {{"././@LongLink", path + '\0', 'L', {}, true},
        {"short", "long-name content", '0', {}, true}, {"DiceNext-beta/README.txt", "readme", '0', {}, true}});
    std::string error;
    ASSERT_TRUE(dice::update::extractPosixUpdate(f.root / "gnu.tar.gz", f.root / "out", error));
    ASSERT_EQ(contents(f.root / "out" / path), std::string("long-name content"));
    ASSERT_EQ(contents(f.root / "out/DiceNext-beta/README.txt"), std::string("readme"));
}

TEST(PosixArchive, RejectsTraversalPaxTraversalDuplicateLinksAndSpecialFiles) {
    for (const auto& entries : std::vector<std::vector<Entry>>{
        {{"../escape", "bad"}}, {{"/absolute", "bad"}},
        {{"pax", paxPath("../escape"), 'x'}, {"safe", "bad"}},
        {{"same", "one"}, {"same", "two"}},
        {{"link", "", '2', "../../outside"}},
        {{"link", "", '1', "file"}}, {{"device", "", '3'}}, {{"pipe", "", '6'}}}) {
        Fixture f;
        fs::create_directory(f.root / "out");
        archive(f.root / "test.tar.gz", entries);
        std::string error;
        ASSERT_FALSE(dice::update::extractPosixUpdate(f.root / "test.tar.gz", f.root / "out", error));
        ASSERT_FALSE(error.empty());
        ASSERT_FALSE(fs::exists(f.root / "escape"));
    }
}

TEST(PosixArchive, RejectsCorruptionAndSupportsCancellation) {
    Fixture f;
    fs::create_directory(f.root / "out");
    archive(f.root / "test.tar.gz", {{"file", "safe"}});
    std::string error;
    ASSERT_FALSE(dice::update::extractPosixUpdate(f.root / "test.tar.gz", f.root / "out", error, [] { return true; }));
    ASSERT_EQ(error, std::string("update operation cancelled"));
    fs::resize_file(f.root / "test.tar.gz", fs::file_size(f.root / "test.tar.gz") - 8);
    ASSERT_FALSE(dice::update::extractPosixUpdate(f.root / "test.tar.gz", f.root / "out", error));
}

TEST(PosixArchive, RejectsTruncatedHeadersAndEveryTrailerByteAndDamagedChecksums) {
    Fixture original;
    archive(original.root / "complete.tar.gz", {{"file", "safe"}});
    const auto bytes = contents(original.root / "complete.tar.gz");
    std::vector<std::string> invalid;
    for (size_t length = 0; length < 10; ++length) invalid.push_back(bytes.substr(0, length));
    for (size_t missing = 1; missing <= 16; ++missing)
        invalid.push_back(bytes.substr(0, bytes.size() - missing));
    for (const auto trailerOffset : {8, 4}) { // CRC32 and ISIZE, not just the deflate payload.
        auto corrupt = bytes;
        corrupt[corrupt.size() - trailerOffset] ^= 1;
        invalid.push_back(std::move(corrupt));
    }
    invalid.push_back(bytes + "trailing garbage");
    for (const auto& input : invalid) {
        Fixture f;
        fs::create_directory(f.root / "out");
        file(f.root / "invalid.tar.gz", input);
        std::string error;
        ASSERT_FALSE(dice::update::extractPosixUpdate(f.root / "invalid.tar.gz", f.root / "out", error));
        ASSERT_FALSE(error.empty());
    }
}

TEST(PosixArchive, ValidatesEveryConcatenatedGzipMemberAndRejectsHiddenArchives) {
    Fixture original;
    archive(original.root / "complete.tar.gz", {{"file", "safe"}});
    std::array<char, 1024> padding{};
    auto gz = gzopen((original.root / "padding.gz").c_str(), "wb");
    ASSERT_TRUE(gz != nullptr);
    ASSERT_EQ(gzwrite(gz, padding.data(), padding.size()), static_cast<int>(padding.size()));
    ASSERT_EQ(gzclose(gz), Z_OK);
    const auto first = contents(original.root / "complete.tar.gz");
    const auto second = contents(original.root / "padding.gz");
    {
        Fixture f;
        fs::create_directory(f.root / "out");
        file(f.root / "valid.tar.gz", first + second);
        std::string error;
        ASSERT_TRUE(dice::update::extractPosixUpdate(f.root / "valid.tar.gz", f.root / "out", error));
        ASSERT_EQ(contents(f.root / "out/file"), std::string("safe"));
    }
    // A valid first member must never conceal a truncated later member or a
    // second tar archive hidden after the first archive's end marker.
    for (const auto& input : {first + second.substr(0, second.size() - 8), first + first}) {
        Fixture f;
        fs::create_directory(f.root / "out");
        file(f.root / "invalid.tar.gz", input);
        std::string error;
        ASSERT_FALSE(dice::update::extractPosixUpdate(f.root / "invalid.tar.gz", f.root / "out", error));
        ASSERT_FALSE(error.empty());
    }
}

TEST(PosixArchive, StreamsLargePayloadsAcrossCompressedInputBlocks) {
    Fixture f;
    fs::create_directory(f.root / "out");
    std::string body;
    uint32_t state = 0x12345678;
    for (size_t i = 0; i < 200000; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        body.push_back(static_cast<char>(state & 255));
    }
    archive(f.root / "large.tar.gz", {{"file", body}});
    ASSERT_TRUE(fs::file_size(f.root / "large.tar.gz") > 65536);
    std::string error;
    ASSERT_TRUE(dice::update::extractPosixUpdate(f.root / "large.tar.gz", f.root / "out", error));
    ASSERT_TRUE(contents(f.root / "out/file") == body);
}
#endif
