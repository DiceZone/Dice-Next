#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace dice::notice_accounts {
using Json = nlohmann::json;

// 配置类型用于创建适配器；通知使用其实际消息平台，与群和用户记录一致。
inline std::string platform(const Json& configured) {
    const auto type = configured.value("type", std::string("onebot_v11"));
    return type == "openopq" ? "onebot_v11" : type;
}

inline std::string id(const Json& configured) {
    const auto& value = configured.at("id");
    return value.is_string() ? value.get<std::string>() : value.dump();
}

inline bool matches(const Json& configured, const std::string& account,
                    const std::string& destinationPlatform) {
    return id(configured) == account && platform(configured) == destinationPlatform;
}

inline Json describe(const Json& configured, const std::string& loginId = {},
                     const std::string& loginName = {}) {
    return {{"id", id(configured)}, {"platform", platform(configured)},
            {"name", configured.value("name", std::string())},
            {"login_id", loginId}, {"login_name", loginName}};
}
} // namespace dice::notice_accounts
