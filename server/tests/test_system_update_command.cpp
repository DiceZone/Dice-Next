#include "test_framework.h"
#include "core/command_router.h"

#include <chrono>
#include <filesystem>

using namespace dice;

namespace {

struct UpdateTestDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dicenext-system-update-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UpdateTestDir() {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("cannot create test directory");
    }
    ~UpdateTestDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

class ReplyAdapter final : public IAdapter {
public:
    std::string id() const override { return "update-test"; }
    std::string name() const override { return id(); }
    std::string platform() const override { return "onebot_v11"; }
    std::string version() const override { return "test"; }
    bool configure(const json&) override { return true; }
    bool start() override { return true; }
    void stop() override {}
    bool isConnected() const override { return true; }
    std::string lastError() const override { return {}; }
    void sendMessage(const Message&) override {}
    void sendReply(const Message&, const std::string&) override {}
    void onMessage(MessageCallback) override {}
    std::string getLoginId() const override { return "9000"; }
    std::string getLoginName() const override { return "骰娘"; }
    std::string getGroupName(const std::string&) const override { return {}; }
    std::vector<std::string> getGroupMemberList(const std::string&) const override { return {}; }
    bool isGroupAdmin(const std::string&, const std::string&) const override { return false; }
    bool isGroupOwner(const std::string&, const std::string&) const override { return false; }
    void setGroupKick(const std::string&, const std::string&) override {}
    void setGroupBan(const std::string&, const std::string&, int) override {}
};

struct UpdateFixture {
    UpdateTestDir dir;
    Database db;
    ConfigManager cfg{u8str(dir.path / "config")};
    DiceEngine engine{cfg};
    I18n i18n{u8str(std::filesystem::path(__FILE__).parent_path().parent_path() / "i18n")};
    LocaleResolver resolver{db, cfg};
    CharacterCardStore cards{db};
    CardDeck deck;
    AdapterManager adapters{db};
    CommandRouter router{db, cfg, engine, i18n, resolver, cards, deck, adapters};
    Message msg;
    json status = {
        {"current", {{"tag", "v1.0.0"}, {"build", 100}}},
        {"latest", nullptr}, {"updateAvailable", false}, {"phase", "idle"},
        {"pending", false}, {"installSupported", true}, {"selfUpdateBlockedReason", ""},
    };
    int checks = 0, downloads = 0, installs = 0;

    UpdateFixture() {
        if (!db.open(u8str(dir.path / "test.db"))) throw std::runtime_error("cannot open test database");
        cfg.resetDefault();
        adapters.registerAdapter(std::make_shared<ReplyAdapter>());
        msg.platform = "onebot_v11";
        msg.adapterId = "update-test";
        msg.selfId = "9000";
        msg.senderId = "1000";
        msg.senderName = "希亚";
        msg.type = MessageType::kPrivate;
        msg.targetId = "1000";
    }
    void master() {
        cfg.set<json>("dice/masters", json::array({{
            {"platform", msg.platform}, {"adapter_id", msg.adapterId}, {"id", msg.senderId}
        }}));
    }
    void wire() {
        router.updateControl = {
            [this] { return status; },
            [this](std::string&) { ++checks; status["phase"] = "checking"; return true; },
            [this](std::string&) { ++downloads; status["phase"] = "connecting"; return true; },
            [this](std::string&) { ++installs; return true; },
        };
    }
    void newerRelease() {
        status["updateAvailable"] = true;
        status["phase"] = "available";
        status["latest"] = {{"tag", "v1.1.0"}, {"build", 120},
            {"releaseUrl", "https://example.test/v1.1.0"},
            {"asset", {{"name", "dice-next.zip"}, {"size", 52428800}}}};
    }
    std::string run(const std::string& content) {
        msg.content = content;
        return router.handleMessage(msg, Locale::kZhHans);
    }
    std::string tr(const std::string& key, const I18n::Args& args = {}) {
        return i18n.tr(Locale::kZhHans, "system." + key, args);
    }
};

}  // namespace

TEST(SystemUpdateCommand, OnlyMastersCanUseItAndAnUnwiredServiceIsReported) {
    UpdateFixture f;
    f.wire();
    ASSERT_EQ(f.run(".system update"), std::string());
    ASSERT_EQ(f.checks, 0);

    UpdateFixture unwired;
    unwired.master();
    ASSERT_EQ(unwired.run(".system update"), unwired.tr("update_unavailable"));
    ASSERT_EQ(unwired.run(".system update now"), unwired.tr("usage"));
}

TEST(SystemUpdateCommand, CheckIsQueuedOnceAndASecondRequestWaits) {
    UpdateFixture f;
    f.master();
    f.wire();
    ASSERT_EQ(f.run(".system update"), f.tr("update_checking"));
    ASSERT_EQ(f.checks, 1);
    ASSERT_EQ(f.run(".system update"), f.tr("update_busy", {{"phase", f.tr("update_phase_checking")}}));
    ASSERT_EQ(f.checks, 1);
    ASSERT_EQ(f.run(".system update install"), f.tr("update_busy", {{"phase", f.tr("update_phase_checking")}}));
    ASSERT_EQ(f.downloads, 0);
}

TEST(SystemUpdateCommand, InstallDownloadsAKnownReleaseWithoutCheckingAgain) {
    UpdateFixture f;
    f.master();
    f.wire();
    f.newerRelease();
    ASSERT_EQ(f.run(".system update install"),
              f.tr("update_downloading", {{"latest", "v1.1.0 (build 120)"}, {"size", "50.0"}}));
    ASSERT_EQ(f.checks, 0);
    ASSERT_EQ(f.downloads, 1);
}

TEST(SystemUpdateCommand, InstallRefusesWhereTheProgramCannotReplaceItself) {
    UpdateFixture container;
    container.master();
    container.wire();
    container.newerRelease();
    container.status["selfUpdateBlockedReason"] = "container";
    container.status["installSupported"] = false;
    ASSERT_EQ(container.run(".system update install"), container.tr("update_container"));

    UpdateFixture unmanaged;
    unmanaged.master();
    unmanaged.wire();
    unmanaged.newerRelease();
    unmanaged.status["installSupported"] = false;
    ASSERT_EQ(unmanaged.run(".system update install"), unmanaged.tr("update_no_installer"));
    ASSERT_EQ(container.downloads + unmanaged.downloads, 0);
}

TEST(SystemUpdateCommand, AStagedPackageGoesStraightToInstallation) {
    UpdateFixture f;
    f.master();
    f.wire();
    f.newerRelease();
    f.status["phase"] = "staged";
    f.status["pending"] = true;
    ASSERT_EQ(f.run(".system update install"), f.tr("update_installing", {{"latest", "v1.1.0 (build 120)"}}));
    ASSERT_EQ(f.downloads, 0);
    // The installation itself is queued two seconds later on the event loop,
    // so the reply is sent before the process restarts.
    ASSERT_EQ(f.installs, 0);
}
