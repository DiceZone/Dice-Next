#pragma once
#include "../common/legacy_lua_event.h"
#include <nlohmann/json.hpp>
#include <mutex>

namespace dice {
/// A protocol event can establish the account before get_login_info returns.
/// Missing/zero IDs and unrelated API objects must not erase or replace it.
class OneBotAccountIdentity {
public:
    void observeEvent(const nlohmann::json& event) {
        if (!event.is_object() || (!event.contains("post_type") && !event.contains("meta_event_type"))) return;
        update(readId(event, "self_id"));
    }
    void observeLoginInfo(const nlohmann::json& data) {
        const auto id = readId(data, "user_id");
        if (id.empty()) return;
        std::lock_guard lock(mutex_);
        if (id_ != id) name_.clear();
        id_ = id;
        if (data.contains("nickname") && data["nickname"].is_string()) name_ = data["nickname"].get<std::string>();
    }
    std::string id() const { std::lock_guard lock(mutex_); return id_; }
    std::string name() const { std::lock_guard lock(mutex_); return name_; }
    static bool validId(const nlohmann::json& value) {
        if (value.is_string()) return positiveQQId(value.get<std::string>()).has_value();
        return (value.is_number_integer() || value.is_number_unsigned()) && positiveQQId(value.dump()).has_value();
    }
private:
    static std::string readId(const nlohmann::json& data, const char* key) {
        if (!data.is_object() || !data.contains(key) || !validId(data[key])) return {};
        return std::to_string(*positiveQQId(data[key].is_string() ? data[key].get<std::string>() : data[key].dump()));
    }
    void update(const std::string& id) {
        if (id.empty()) return;
        std::lock_guard lock(mutex_);
        if (id_ != id) { id_ = id; name_.clear(); }
    }
    mutable std::mutex mutex_;
    std::string id_, name_;
};
}
