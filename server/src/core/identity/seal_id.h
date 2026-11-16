#pragma once

#include <string>

namespace dice::identity {

// SealDice's plugin-visible address space. The adapter account and database
// identity stay separate, even when multiple endpoints serve the same platform.
struct SealAddress {
    std::string platform, bot, guild;
    bool channel = false;

    std::string name() const {
        if (platform == "onebot_v11" || platform == "onebot" || platform == "milky") return "QQ";
        if (platform == "qq_official") return channel ? "OpenQQCH" : "OpenQQ";
        if (platform == "discord") return "DISCORD";
        if (platform == "kook") return "KOOK";
        return platform;
    }
    std::string endpointPlatform() const {
        return platform == "qq_official" ? "QQ" : name();
    }
    std::string protocol() const {
        if (platform == "onebot_v11") return "onebot";
        if (platform == "qq_official") return "official";
        return platform;
    }
    std::string prefix(bool group) const {
        const auto p = name();
        if (p == "QQ") {
            if (channel) return group ? "QQ-CH-Group:" + guild + "-" : "QQ-CH:";
            return group ? "QQ-Group:" : "QQ:";
        }
        if (p == "OpenQQ") return (group ? "OpenQQ-Group:" : "OpenQQ:") + bot + "-";
        if (p == "OpenQQCH") return group ? "OpenQQCH-Channel:" + guild + "-" : "OpenQQCH:";
        if (p == "DISCORD" || p == "KOOK") return p + (group ? "-CH-Group:" : ":");
        return {};
    }
    std::string id(const std::string& raw, bool group) const {
        if (raw.empty()) return {};
        return prefix(group) + raw;
    }
    std::string self(const std::string& raw) const {
        if (platform == "qq_official" && !bot.empty()) return "OpenQQ:" + bot;
        if (name() == "QQ") return raw.empty() ? raw : "QQ:" + raw;
        return id(raw, false);
    }
    std::string guildId() const {
        if (guild.empty()) return {};
        const auto p = name();
        if (p == "KOOK" || p == "OpenQQCH") return p + "-Guild:" + guild;
        // Discord's ordinary message handler exposes the native guild snowflake.
        return guild;
    }
};
}
