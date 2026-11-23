#pragma once
#include <nlohmann/json.hpp>
#include <string_view>

namespace dice::onebot_login {
// Only the known pre-login response to bootstrap reads is expected. Permission,
// authentication and message-send failures must retain their normal severity.
inline bool expectedWait(bool managed, bool online, std::string_view echo, const nlohmann::json& response) {
    if (!managed || online || !response.is_object()) return false;
    if (echo != "peer_status" && echo != "login_info" && echo != "login_info_retry"
        && echo != "glist" && echo != "flist") return false;
    const auto code = response.find("retcode");
    if (code == response.end() || !code->is_number_integer() || *code != 100) return false;
    for (const auto* key : {"message", "wording"}) {
        const auto text = response.find(key);
        if (text != response.end() && text->is_string()
            && text->get_ref<const std::string&>() == "not connected: nothing has been signed in") return true;
    }
    return false;
}
}
