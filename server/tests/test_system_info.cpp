#include "test_framework.h"
#include "platform/system_info.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using dice::sysinfo::detail::linuxMountForPath;
std::string utf8(const fs::path& p) { const auto s = p.u8string(); return {s.begin(), s.end()}; }
struct TempData {
    fs::path root, file;
    TempData() {
        static std::atomic<unsigned> seq{0};
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 20; ++i) {
            const auto candidate = base / ("dice-next-sysinfo-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++seq));
            if (fs::create_directory(candidate)) { root = candidate; break; }
        }
        if (root.empty()) throw std::runtime_error("Cannot create isolated sysinfo fixture");
        const auto data = root / fs::path(u8"骰娘 数据");
        fs::create_directory(data);
        file = data / "dice.db";
        std::ofstream(file) << "isolated sysinfo fixture";
    }
    ~TempData() { std::error_code ec; if (!root.empty()) fs::remove_all(root, ec); }
};
const char* mounts =
    "1 0 0:1 / / rw - overlay overlay rw\n"
    "2 1 8:1 /docker/hosts /etc/hosts rw - ext4 /dev/sda1 rw\n"
    "3 1 8:1 /docker/hostname /etc/hostname rw - ext4 /dev/sda1 rw\n"
    "4 1 8:1 /docker/resolv /etc/resolv.conf rw - ext4 /dev/sda1 rw\n"
    "5 1 8:1 /instances /data rw shared:1 - ext4 /dev/sda1 rw\n"
    "6 5 8:2 / /data/archives rw - xfs /dev/sdb1 rw\n";
}

TEST(SystemInfo, LinuxDuplicateDeviceMountsDoNotSelectUnrelatedFilesystems) {
    std::istringstream input(mounts);
    auto result = linuxMountForPath(input, "/data/dice.db");
    ASSERT_EQ(result.mount, std::string("/data"));
    ASSERT_EQ(result.fs, std::string("ext4"));
    std::istringstream root(mounts);
    ASSERT_EQ(linuxMountForPath(root, "/opt/dice/data/dice.db").fs, std::string("overlay"));
}

TEST(SystemInfo, LinuxUsesLongestComponentPrefixAndSupportsFileBindMounts) {
    for (const auto& path : {"/data/archives", "/data/archives/dice.db"}) {
        std::istringstream input(mounts);
        ASSERT_EQ(linuxMountForPath(input, path).mount, std::string("/data/archives"));
    }
    std::istringstream neighbor(mounts);
    ASSERT_EQ(linuxMountForPath(neighbor, "/database/dice.db").mount, std::string("/"));
    std::istringstream file(std::string(mounts) + "7 5 8:3 /db/dice.db /data/dice.db rw - btrfs /dev/sdc1 rw\n");
    ASSERT_EQ(linuxMountForPath(file, "/data/dice.db").mount, std::string("/data/dice.db"));
}

TEST(SystemInfo, LinuxMountEscapesAndOptionalFieldsAreDecoded) {
    std::istringstream input("1 0 8:1 / /dice\\040data\\134next\\011tab rw shared:1 master:2 - ext4 /dev/sda rw\n");
    auto result = linuxMountForPath(input, "/dice data\\next\ttab/dice.db");
    ASSERT_EQ(result.mount, std::string("/dice data\\next\ttab"));
    ASSERT_EQ(result.fs, std::string("ext4"));
}

TEST(SystemInfo, MalformedOrMissingMountMetadataNeverInventsARootMatch) {
    for (const auto& value : {"", "broken line\n", "1 0 8:1 / /data rw missing_separator ext4\n", "1 0 8:1 / relative rw - ext4 /dev/sda rw\n"}) {
        std::istringstream input(value);
        ASSERT_TRUE(linuxMountForPath(input, "/data/dice.db").mount.empty());
    }
    std::istringstream input(mounts);
    ASSERT_TRUE(linuxMountForPath(input, "relative/dice.db").mount.empty());
}

TEST(SystemInfo, ExistingUnicodeDataFileAndDirectoryQueryTheirOwnVolume) {
    TempData data;
    const auto file = dice::sysinfo::diskForPath(data.file);
    const auto directory = dice::sysinfo::diskForPath(data.file.parent_path());
    ASSERT_TRUE(file.has_value()); ASSERT_TRUE(directory.has_value());
    ASSERT_EQ(file->path, utf8(fs::canonical(data.file)));
    ASSERT_EQ(file->mount, directory->mount);
    ASSERT_EQ(file->totalGB, directory->totalGB);
    ASSERT_TRUE(!file->mount.empty());
    ASSERT_TRUE(file->usedGB <= file->totalGB);
    ASSERT_TRUE(file->loadPct >= 0 && file->loadPct <= 100);
    const auto expected = fs::space(data.file.parent_path());
    ASSERT_EQ(file->totalGB, expected.capacity / (1024ull * 1024ull * 1024ull));
}

TEST(SystemInfo, InvalidDataPathsDoNotFallBackToOtherDisks) {
    TempData data;
    ASSERT_FALSE(dice::sysinfo::diskForPath({}).has_value());
    ASSERT_FALSE(dice::sysinfo::diskForPath(data.root / "missing" / "dice.db").has_value());
    ASSERT_TRUE(dice::sysinfo::gather(data.root / "missing" / "dice.db").disks.empty());
}

TEST(SystemInfo, DashboardSnapshotContainsExactlyOneDataVolume) {
    TempData data;
    const auto result = dice::sysinfo::gather(data.file);
    ASSERT_EQ(result.disks.size(), size_t(1));
    ASSERT_EQ(result.disks.front().path, utf8(fs::canonical(data.file)));
}

TEST(SystemInfo, SymlinkDataUsesTheResolvedTargetWhenSupported) {
    TempData data;
    std::error_code ec;
    const auto link = data.root / "linked-data";
    fs::create_directory_symlink(data.file.parent_path(), link, ec);
    if (ec) {
#if defined(_WIN32)
        // Windows may require Developer Mode/admin rights; tests never enable it.
        std::cout << "(symlink unavailable on this host; no permission changes) ";
        return;
#else
        ASSERT_FALSE(bool(ec));
#endif
    }
    const auto disk = dice::sysinfo::diskForPath(link / "dice.db");
    ASSERT_TRUE(disk.has_value());
    ASSERT_EQ(disk->path, utf8(fs::canonical(data.file)));
}
