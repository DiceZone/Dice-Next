#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace dice::identity_email {
using Json = nlohmann::json;
using Now = std::function<std::chrono::steady_clock::time_point()>;
using Sender = std::function<bool(const Json&, const std::string&, const std::string&)>;
enum class Status { Queued, Sent, Disabled, BadConfig, RateLimited, Busy, SendFailed,
                    Missing, NotReady, WrongCode, Locked, Verified };

// Credentials are never returned by the settings API. Empty passwords preserve the saved value.
Json publicSettings(const Json&);
bool updateSettings(const Json& old, const Json& input, Json& out);
bool validSettings(const Json&);
bool validQQ(const std::string&);
std::string verificationBody(const std::string& qq, const std::string& code);
std::string statusKey(Status);

// Server-owned, in-memory, single-use challenges. Restart invalidates all outstanding codes.
// SMTP is sent by one bounded worker, never by the adapter event thread.
class Service {
public:
    explicit Service(Sender = {}, Now = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Status begin(const std::string& context, const std::string& qq, const Json& smtp,
                 std::function<void(Status)> completed);
    Status verify(const std::string& context, const std::string& qq, const std::string& code);
    void cancel(const std::string& context);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dice::identity_email
