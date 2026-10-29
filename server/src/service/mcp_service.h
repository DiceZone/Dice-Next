#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <chrono>
#include <stdexcept>

namespace dice {
class ConfigManager;
namespace mcp {
using Json = nlohmann::json;

// HTTP-neutral boundary, also exercised without starting a production server.
struct Request {
    std::string method = "POST", peer = "127.0.0.1", body;
    bool loopback = true, secure = false, forwarded = false;
    std::map<std::string, std::string> headers;
};
struct Response {
    int status = 200;
    Json body;
    std::map<std::string, std::string> headers;
};
class QueryCancelled : public std::runtime_error {
public:
    explicit QueryCancelled(bool timeout) : std::runtime_error(timeout ? "查询超时，未返回结果" : "服务正在退出，查询已取消"), timedOut(timeout) {}
    bool timedOut;
};
// Shared cancellation state using facilities available in the macOS release SDK.
// Copies share a signal; contexts keep it alive independently of the service.
class QueryCancellation {
public:
    QueryCancellation();
    void requestStop() const;
    bool stopRequested() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    void waitUntil(std::chrono::steady_clock::time_point) const;
    friend class QueryContext;
};
class QueryContext {
public:
    explicit QueryContext(QueryCancellation = {}, std::chrono::milliseconds budget = std::chrono::seconds(5));
    void checkpoint() const;
    void waitFor(std::chrono::milliseconds) const;
private:
    QueryCancellation stop_;
    std::chrono::steady_clock::time_point deadline_;
};
struct Queries {
    std::function<Json(const QueryContext&)> status;
    std::function<Json(const std::string&, int, const QueryContext&)> help;
    std::function<Json(int, const QueryContext&)> card;
    std::function<Json(const std::string&, const QueryContext&)> cardOptions;
    std::string version = "development";
};

// No Message, Adapter, command execution, template expansion or plugin executor
// crosses this boundary. Card IDs are granted by the owner, not caller identity.
class Service {
public:
    Service(ConfigManager&, std::string auditPath, Queries = {}, std::chrono::milliseconds queryBudget = std::chrono::seconds(5));
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Json settings() const;
    Json configure(const Json&);
    Json createGrant(const Json&);
    Json updateGrant(const std::string&, const Json&);
    Json rotateGrant(const std::string&);
    Json revokeGrant(const std::string&);
    Json audit(int page, int pageSize, const std::string& clientId = {}, const std::string& outcome = {}) const;
    Json cardOptions(const std::string&) const;
    Json cardDetails(const Json&) const;
    Json selfCheck(const Json&);
    void snapshotAudit(const std::filesystem::path&) const;
    Response handle(const Request&);
    void stop();
    static Json tools();
    static Response validateManagement(const Request&, bool apiKey);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Keep this handle alive until the HTTP listener has stopped. Destruction
// cancels queued work and joins active workers before query dependencies die.
class Transport {
public:
    ~Transport();
    struct Impl;
    explicit Transport(std::shared_ptr<Impl>);
private:
    std::shared_ptr<Impl> impl_;
};
std::unique_ptr<Transport> registerRoutes(const std::shared_ptr<Service>&);
// Used by the existing backup workflow to snapshot a live WAL database.
void snapshotDefaultAudit(const std::filesystem::path&);
// Restore barrier survives a failed/crashed startup and covers database-snapshot
// recovery as well as server.json. No listener is started before it is cleared.
void markRestoreReset();
bool restoreResetPending();
bool resetRestoredAccess(ConfigManager&, const std::function<void(const Json&)>& durableSnapshot);
} // namespace mcp
} // namespace dice
