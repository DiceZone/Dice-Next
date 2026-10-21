#pragma once
#include "notice_content.h"
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
} // namespace dice::notice
