#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>
#include "../src/core/deck/card_deck.h"

// Importer-only storage double. Never starts adapters or executes plugins.
// All importer test translation units use this one definition to avoid ODR
// mismatches. Real Lua behavior is tested in dice-next-lua-tests separately.
namespace dice {
class LuaPluginManager {
public:
    bool failWrites = false;
    void reload() {}
    void confSet(const std::string& scope, const std::string& key, const std::string& value) {
        conf_[scope][key] = value;
    }
    bool confSetBatch(const std::vector<std::tuple<std::string, std::string, std::string>>& values) {
        if (failWrites) return false;
        for (const auto& [scope, key, value] : values) {
            if (value.empty()) conf_[scope].erase(key);
            else conf_[scope][key] = value;
        }
        return true;
    }
    std::string confGet(const std::string& scope, const std::string& key) const {
        auto it = conf_.find(scope); if (it == conf_.end()) return {};
        auto kv = it->second.find(key); return kv == it->second.end() ? std::string() : kv->second;
    }
private:
    std::map<std::string, std::map<std::string, std::string>> conf_;
};
}
