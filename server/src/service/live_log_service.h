#pragma once

#include <memory>
#include <string>
namespace dice {
class ConfigManager;
struct Message;
class LiveLogService {
public:
    LiveLogService(const std::string& logDb, ConfigManager&, std::string origin = "https://log-api.dice.zone");
    ~LiveLogService();
    std::string start(const Message&, const std::string& name, int& localId);
    std::string fork(const Message&, const std::string& arguments, int& localId);
    std::string link(int localId);
    bool belongsTo(int localId, const std::string& adapter);
    void stop();
private:
    std::string open(const Message&, std::string name, const std::string& sourceKey,
                     const std::string& password, int& localId);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
