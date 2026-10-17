#include "test_framework.h"
#include "adapter/adapter_manager.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct StartTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-adapter-start-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    StartTestDir() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create test directory");
    }
    ~StartTestDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Counts start() calls. A real reverse-WS adapter started twice recreates its
// TcpServer and trantor aborts the process, so the count is what matters.
class CountingAdapter final : public IAdapter {
public:
    explicit CountingAdapter(std::string id) : id_(std::move(id)) {}
    int starts = 0;
    std::string id() const override { return id_; }
    std::string name() const override { return id_; }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "test"; }
    bool configure(const json&) override { return true; }
    bool start() override { ++starts; return true; }
    void stop() override {}
    bool isConnected() const override { return false; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message&) override {}
    void sendReply(const Message&, const std::string&) override {}
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return {}; }
    std::string getLoginName() const override { return {}; }
    std::string getGroupName(const std::string&) const override { return {}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
private:
    std::string id_;
};

struct StartFixture {
    StartTestDir dir;
    Database db;
    StartFixture() {
        if (!db.open((dir.path / "test.db").string())) throw std::runtime_error("cannot open test database");
    }
};

} // namespace

// BDC provisioning creates the adapter through the management API within the
// first half second; the deferred startAll() must not start it a second time.
TEST(AdapterManagerStart, StartAllSkipsAdapterTheApiAlreadyStarted) {
    StartFixture f;
    AdapterManager manager(f.db);
    auto adapter = std::make_shared<CountingAdapter>("1");
    manager.registerAdapter(adapter);
    ASSERT_TRUE(manager.startAdapter("1"));
    manager.startAll();
    ASSERT_EQ(adapter->starts, 1);
}

TEST(AdapterManagerStart, StartAllStartsEachRegisteredAdapterOnce) {
    StartFixture f;
    AdapterManager manager(f.db);
    auto first = std::make_shared<CountingAdapter>("1");
    auto second = std::make_shared<CountingAdapter>("2");
    manager.registerAdapter(first);
    manager.registerAdapter(second);
    manager.startAll();
    manager.startAll();
    ASSERT_EQ(first->starts, 1);
    ASSERT_EQ(second->starts, 1);
}

// Changing an adapter's settings registers a new object under the same id;
// that new object has never been started and must start normally.
TEST(AdapterManagerStart, ReplacementAdapterUnderSameIdStillStarts) {
    StartFixture f;
    AdapterManager manager(f.db);
    auto original = std::make_shared<CountingAdapter>("1");
    manager.registerAdapter(original);
    ASSERT_TRUE(manager.startAdapter("1"));
    manager.stopAdapter("1");
    manager.unregisterAdapter("1");
    auto replacement = std::make_shared<CountingAdapter>("1");
    manager.registerAdapter(replacement);
    ASSERT_TRUE(manager.startAdapter("1"));
    manager.startAll();
    ASSERT_EQ(original->starts, 1);
    ASSERT_EQ(replacement->starts, 1);
}

TEST(AdapterManagerStart, StoppedAdapterCanBeStartedAgain) {
    StartFixture f;
    AdapterManager manager(f.db);
    auto adapter = std::make_shared<CountingAdapter>("1");
    manager.registerAdapter(adapter);
    ASSERT_TRUE(manager.startAdapter("1"));
    manager.stopAdapter("1");
    manager.startAll();
    ASSERT_EQ(adapter->starts, 2);
}
