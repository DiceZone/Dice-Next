#pragma once
#include "notice_manager.h"
#include <drogon/HttpRequest.h>

namespace dice::notice {
inline ProcessingContext webContext(const drogon::HttpRequestPtr &req, AdapterManager &adapters) {
    ProcessingContext context{{{"网页后台", "管理操作", "", ""}},
                              std::string(req->methodString()) + " " + req->path(),
                              "管理操作已执行",
                              ProcessingOrigin{}};
    auto &o = context.origin;
    try {
        o.userId = req->attributes()->get<std::string>("dn_qq");
    } catch (...) {
    }
    o.entry = o.userId.empty() ? "网页后台（密码／API 授权，未提供可识别的 QQ 身份）"
                               : "网页后台（已验证的第三方登录身份）";
    if (!o.userId.empty()) {
        o.platform = "onebot_v11";
        adapters.enrichLogOrigin(o, true);
    }
    return context;
}
// Run only after a successful save. Failure to notify must never fail that save.
template<class MakeText>
inline void webManagementNotice(const drogon::HttpRequestPtr& req, ConfigManager& cfg,
                                AdapterManager& adapters, MakeText makeText,
                                const std::string& platform = "", const std::string& adapterId = "",
                                const std::string& targetType = "", const std::string& targetId = "") noexcept {
    try {
        auto context = webContext(req, adapters);
        if (!platform.empty()) context.origin.platform = platform;
        context.origin.adapterId = adapterId;
        context.origin.targetType = targetType; context.origin.targetId = targetId;
        context.origin.targetName.clear(); enrichOrigin(context.origin, adapters);
        const auto text = makeText(context.origin.targetName);
        ProcessingScope scope(std::move(context));
        emitSafely(cfg, adapters, text, adapterId);
    } catch (...) {
        try { DICE_LOG_WARN("管理操作已保存，但未能生成变更通知。"); } catch (...) {}
    }
}
} // namespace dice::notice
