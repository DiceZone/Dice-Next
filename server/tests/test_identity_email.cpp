#include "test_framework.h"
#include "../src/service/identity_email_service.h"
#include "../src/core/identity/identity_binding.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>

using namespace dice;
using namespace dice::identity_email;
namespace {
Json smtp() { return {{"enabled", true}, {"host", "smtp.example.com"}, {"port", 465}, {"ssl", true},
                     {"user", "bot@example.com"}, {"pass", "test-only-password"}, {"from", "bot@example.com"}}; }
struct Fixture {
    std::mutex mu; std::condition_variable cv; unsigned completions = 0;
    std::string recipient, code; Status completed = Status::Missing;
    std::atomic<long long> seconds{0}; std::atomic<bool> fail{false};
    Service service{[this](const Json&, const std::string& to, const std::string& body) {
        std::lock_guard lock(mu); recipient = to;
        const auto p = body.find(".bind confirm ");
        std::istringstream input(body.substr(p)); std::string command, sub;
        input >> command >> sub >> code;
        return !fail.load();
    }, [this] { return std::chrono::steady_clock::time_point(std::chrono::seconds(seconds.load())); }};
    Status begin(const std::string& context = "official/app/native", const std::string& qq = "160703953") {
        return service.begin(context, qq, smtp(), [this](Status s) {
            std::lock_guard lock(mu); completed = s; ++completions; cv.notify_all();
        });
    }
    bool wait(unsigned count = 1) {
        std::unique_lock lock(mu); return cv.wait_for(lock, std::chrono::seconds(5), [&] { return completions >= count; });
    }
};
struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("dice-email-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { std::filesystem::create_directory(path); }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
}

TEST(IdentityEmail, ValidatesQQAndSmtpAndNeverReturnsPassword) {
    ASSERT_TRUE(validQQ("160703953"));
    for (const auto* bad : {"1234", "00012345", "12345@qq.com", "12345\r\n", "-123456", "1234567890123"}) ASSERT_FALSE(validQQ(bad));
    auto s = smtp(); ASSERT_TRUE(validSettings(s));
    const auto visible = publicSettings(s);
    ASSERT_FALSE(visible.contains("pass")); ASSERT_TRUE(visible["password_configured"].get<bool>());
    Json updated; ASSERT_TRUE(updateSettings(s, {{"pass", ""}, {"port", 587}, {"ssl", false}}, updated));
    ASSERT_EQ(updated["pass"].get<std::string>(), std::string("test-only-password"));
    ASSERT_TRUE(updateSettings(s, {{"pass", "replacement"}}, updated));
    ASSERT_EQ(updated["pass"].get<std::string>(), std::string("replacement"));
    ASSERT_FALSE(updateSettings(s, {{"host", "smtp.example.com/path"}}, updated));
    ASSERT_FALSE(updateSettings(s, {{"from", "bot@example.com\r\nBcc: victim@example.com"}}, updated));
    ASSERT_FALSE(updateSettings(s, {{"user", "user:extra"}}, updated));
    ASSERT_FALSE(updateSettings(s, {{"pass", "secret\nurl=evil"}}, updated));
    ASSERT_FALSE(updateSettings(s, {{"port", 0}}, updated));
    ASSERT_FALSE(updateSettings(s, {{"enabled", "true"}}, updated));
    ASSERT_TRUE(updateSettings(Json::object(), {{"enabled", false}}, updated));
}

TEST(IdentityEmail, SwitchingToOAuthInvalidatesTheOutstandingEmailCode) {
    Fixture f;
    ASSERT_TRUE(f.begin() == Status::Queued);
    ASSERT_TRUE(f.wait());
    const auto code = f.code;
    f.service.cancel("official/app/native");
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", code) == Status::Missing);
    ASSERT_TRUE(f.begin() == Status::RateLimited); // Switching methods must not reset SMTP abuse limits.
}
TEST(IdentityEmail, PrivateSingleUseCodeIsTiedToContextAndTarget) {
    Fixture f; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait());
    ASSERT_TRUE(f.completed == Status::Sent); ASSERT_EQ(f.recipient, std::string("160703953@qq.com"));
    ASSERT_EQ(f.code.size(), size_t(8));
    ASSERT_TRUE(f.service.verify("other-adapter/native", "160703953", f.code) == Status::Missing);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703954", f.code) == Status::WrongCode);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", f.code) == Status::Verified);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", f.code) == Status::Missing);
}
TEST(IdentityEmail, ExpiresAndLocksAfterFiveErrors) {
    Fixture f; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait());
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(f.service.verify("official/app/native", "160703953", "invalid") == Status::WrongCode);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", "invalid") == Status::Locked);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", f.code) == Status::Missing);
    f.seconds = 60; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait(2));
    f.seconds = 660; ASSERT_TRUE(f.service.verify("official/app/native", "160703953", f.code) == Status::Missing);
}
TEST(IdentityEmail, ResendInvalidatesOldCodeAndThrottlesBothIdentityAndRecipient) {
    Fixture f; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait()); const auto old = f.code;
    ASSERT_TRUE(f.begin("other-context") == Status::RateLimited);
    ASSERT_TRUE(f.begin("official/app/native", "160703954") == Status::RateLimited);
    f.seconds = 60; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait(2));
    if (old != f.code) ASSERT_TRUE(f.service.verify("official/app/native", "160703953", old) == Status::WrongCode);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", f.code) == Status::Verified);
    for (unsigned count = 3; count <= 5; ++count) {
        f.seconds = 60 * (count - 1); ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait(count));
    }
    f.seconds = 300; ASSERT_TRUE(f.begin() == Status::RateLimited);
    f.seconds = 3600; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait(6));
}
TEST(IdentityEmail, FailedDeliveryNeverMakesACodeUsable) {
    Fixture f; f.fail = true; ASSERT_TRUE(f.begin() == Status::Queued); ASSERT_TRUE(f.wait());
    ASSERT_TRUE(f.completed == Status::SendFailed);
    ASSERT_TRUE(f.service.verify("official/app/native", "160703953", f.code) == Status::Missing);
    auto disabled = smtp(); disabled["enabled"] = false;
    ASSERT_TRUE(f.service.begin("ctx", "160703953", disabled, {}) == Status::Disabled);
    ASSERT_TRUE(f.service.begin("ctx", "1234", smtp(), {}) == Status::BadConfig);
}
TEST(IdentityEmail, VerifiedBindCannotMoveAnAlreadyBoundAccount) {
    TempDir dir; Database db; ASSERT_TRUE(db.open((dir.path / "dice.db").string()));
    auto& bindings = identity::BindingStore::instance();
    bindings.observeOfficial(db, "test", "openid", identity::Kind::User);
    std::string error;
    ASSERT_TRUE(bindings.bindVerifiedOfficialToQQ(db, "QQ-Official-test:openid", "160703953", error));
    ASSERT_FALSE(bindings.bindVerifiedOfficialToQQ(db, "QQ-Official-test:openid", "160703954", error));
    ASSERT_EQ(bindings.publicForOfficial(db, "test", "openid", identity::Kind::User), std::string("160703953"));
    const auto id = bindings.observeVirtual(db, "discord", "adapter", "native", identity::Kind::User);
    ASSERT_TRUE(identity::BindingStore::isVirtual(id));
    ASSERT_TRUE(bindings.bindVerifiedPlatformToQQ(db, "discord", "native", "160703953", error));
    ASSERT_FALSE(bindings.bindVerifiedPlatformToQQ(db, "discord", "native", "160703954", error));
    ASSERT_EQ(bindings.transportEndpoint(db, "discord", "160703953", identity::Kind::User), std::string("native"));
}
