#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace dice {
class Database;
class ConfigManager;
class CharacterCardStore;
struct Message;

namespace cloud_cards {
using Json = nlohmann::json;
struct Request {
    std::string path, method = "GET", body, apiKey, accessToken;
    bool form = false;
};
struct Response { int status = 0; Json body; };
using Transport = std::function<Response(const Request&)>;
using Now = std::function<std::chrono::steady_clock::time_point()>;
struct Result {
    std::string key;
    std::map<std::string, std::string> args;
    bool secret = false; // Private adapter delivery only; never ordinary reply persistence.
};

// BDC schema v1 codec. Throws on malformed/unsupported input; preserves unknown fields.
Json toDocument(const Json& raw, const std::string& name, const std::string& system = "");
Json fromDocument(const Json& document);
// Credential-free diagnostic for the production HTTPS transport.
Response probeOfficialMetadata();

class Service {
public:
    Service(Database&, ConfigManager&, CharacterCardStore&, Transport = {}, Now = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Result handle(const Message&, const std::string& args);
    // Bounded, single-worker queue. Never run cloud I/O on adapter event threads.
    bool dispatch(Message, std::string args, std::function<void(Result)> completed);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cloud_cards
} // namespace dice
