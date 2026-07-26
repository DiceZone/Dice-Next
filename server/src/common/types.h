#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>

namespace dice {

// ─── Locale (i18n) ───────────────────────────────────────────
// Supported UI / reply languages. Codes follow BCP-47.
//   kZhHant — 繁體中文 (Traditional Chinese)
//   kZhHans — 简体中文 (Simplified Chinese)
//   kEn     — English
//   kJa     — 日本語 (Japanese)

enum class Locale {
    kZhHant = 0,
    kZhHans = 1,
    kEn     = 2,
    kJa     = 3,
};

// ─── C#46: 自定义语言（动态 Locale 注册表）────────────────────
// i18n/ 目录下除内置四语外的任意 <code>.json 会被 I18n::load() 注册为一个
// 动态 Locale 值（kCustomLocaleBase 起递增）。localeToString/localeFromString
// 都先查注册表，因此所有以 Locale 传递语言的现有代码无需改动。
// 注册表只增不删（热重载重复注册返回同一值），故 c_str() 指针稳定。
inline constexpr int kCustomLocaleBase = 100;
inline std::mutex& customLocaleMutex() { static std::mutex m; return m; }
inline std::map<std::string, int>& customLocaleByCode() { static std::map<std::string, int> m; return m; }
inline std::map<int, std::string>& customLocaleById()   { static std::map<int, std::string> m; return m; }

/// Register (or look up) a custom locale by its BCP-47-ish code (file stem).
inline Locale registerCustomLocale(const std::string& code) {
    std::lock_guard<std::mutex> lk(customLocaleMutex());
    auto& byCode = customLocaleByCode();
    if (auto it = byCode.find(code); it != byCode.end())
        return static_cast<Locale>(it->second);
    int id = kCustomLocaleBase + static_cast<int>(byCode.size());
    byCode[code] = id;
    customLocaleById()[id] = code;
    return static_cast<Locale>(id);
}

inline const char* localeToString(Locale loc) {
    switch (loc) {
        case Locale::kZhHant: return "zh-Hant";
        case Locale::kZhHans: return "zh-Hans";
        case Locale::kEn:     return "en";
        case Locale::kJa:     return "ja";
        default: break;
    }
    // C#46: dynamic custom locale
    {
        std::lock_guard<std::mutex> lk(customLocaleMutex());
        auto& byId = customLocaleById();
        if (auto it = byId.find(static_cast<int>(loc)); it != byId.end())
            return it->second.c_str();
    }
    return "zh-Hans";
}

/// Parse a locale code. Accepts common aliases (zh-TW, zh-CN, en-US, ja-JP, ...)
/// and any registered custom locale code (C#46).
/// Falls back to Simplified Chinese for anything unrecognized.
inline Locale localeFromString(const std::string& s) {
    // Traditional Chinese variants
    if (s == "zh-Hant" || s == "zh-TW" || s == "zh-HK" || s == "zh-MO" ||
        s == "zh_TW" || s == "zh_HK")
        return Locale::kZhHant;
    // English variants
    if (s == "en" || s.rfind("en-", 0) == 0 || s.rfind("en_", 0) == 0)
        return Locale::kEn;
    // Japanese variants
    if (s == "ja" || s.rfind("ja-", 0) == 0 || s.rfind("ja_", 0) == 0)
        return Locale::kJa;
    // C#46: registered custom locales (exact code match, e.g. "ko"/"fr")
    if (s != "zh-Hans" && !s.empty()) {
        std::lock_guard<std::mutex> lk(customLocaleMutex());
        auto& byCode = customLocaleByCode();
        if (auto it = byCode.find(s); it != byCode.end())
            return static_cast<Locale>(it->second);
    }
    // Simplified Chinese variants (default for any other zh-* or unknown)
    return Locale::kZhHans;
}

// ─── Adapter Types ───────────────────────────────────────────

enum class AdapterType {
    kUnknown = 0,
    kOneBotV11 = 1,
    kMilky = 2,
    kQQOfficial = 3,
};

inline const char* adapterTypeToString(AdapterType type) {
    switch (type) {
        case AdapterType::kOneBotV11: return "onebot_v11";
        case AdapterType::kMilky:     return "milky";
        case AdapterType::kQQOfficial: return "qq_official";
        case AdapterType::kUnknown:
        default:                      return "unknown";
    }
}

inline AdapterType adapterTypeFromString(const std::string& s) {
    if (s == "onebot_v11") return AdapterType::kOneBotV11;
    if (s == "milky")      return AdapterType::kMilky;
    if (s == "qq_official") return AdapterType::kQQOfficial;
    return AdapterType::kUnknown;
}

// ─── Connection Mode ─────────────────────────────────────────

enum class ConnectionMode {
    kForwardWS = 0,
    kReverseWS = 1,
    kHTTP      = 2,
};

inline const char* connectionModeToString(ConnectionMode mode) {
    switch (mode) {
        case ConnectionMode::kForwardWS: return "forward_ws";
        case ConnectionMode::kReverseWS: return "reverse_ws";
        case ConnectionMode::kHTTP:      return "http";
        default:                         return "unknown";
    }
}

inline ConnectionMode connectionModeFromString(const std::string& s) {
    if (s == "forward_ws") return ConnectionMode::kForwardWS;
    if (s == "reverse_ws") return ConnectionMode::kReverseWS;
    if (s == "http")       return ConnectionMode::kHTTP;
    return ConnectionMode::kForwardWS;
}

// ─── Adapter Status ──────────────────────────────────────────

enum class AdapterStatus {
    kStopped = 0,
    kConnecting = 1,
    kConnected = 2,
    kError = 3,
};

inline const char* adapterStatusToString(AdapterStatus status) {
    switch (status) {
        case AdapterStatus::kStopped:    return "stopped";
        case AdapterStatus::kConnecting: return "connecting";
        case AdapterStatus::kConnected:  return "connected";
        case AdapterStatus::kError:      return "error";
        default:                         return "unknown";
    }
}

// ─── Match Type (Reply Matching) ─────────────────────────────

enum class MatchType {
    kKeyword = 0,
    kPrefix  = 1,
    kRegex   = 2,
    kSearch  = 3,
};

inline const char* matchTypeToString(MatchType type) {
    switch (type) {
        case MatchType::kKeyword: return "keyword";
        case MatchType::kPrefix:  return "prefix";
        case MatchType::kRegex:   return "regex";
        case MatchType::kSearch:  return "search";
        default:                  return "unknown";
    }
}

inline MatchType matchTypeFromString(const std::string& s) {
    if (s == "keyword") return MatchType::kKeyword;
    if (s == "prefix")  return MatchType::kPrefix;
    if (s == "regex")   return MatchType::kRegex;
    if (s == "search")  return MatchType::kSearch;
    return MatchType::kKeyword;
}

// ─── Dice Type ───────────────────────────────────────────────

enum class DiceType {
    kStandard  = 0,   // XdY
    kCOC       = 1,   // Call of Cthulhu
    kDND       = 2,   // Dungeons & Dragons
    kFate      = 3,   // Fate RPG
    kL5R       = 4,   // Legend of the Five Rings
};

inline const char* diceTypeToString(DiceType type) {
    switch (type) {
        case DiceType::kStandard: return "standard";
        case DiceType::kCOC:      return "coc";
        case DiceType::kDND:      return "dnd";
        case DiceType::kFate:     return "fate";
        case DiceType::kL5R:      return "l5r";
        default:                  return "unknown";
    }
}

// ─── Session Status ──────────────────────────────────────────

enum class GameLogStatus {
    kActive    = 0,
    kPaused    = 1,
    kCompleted = 2,
};

inline const char* gameLogStatusToString(GameLogStatus status) {
    switch (status) {
        case GameLogStatus::kActive:    return "active";
        case GameLogStatus::kPaused:    return "paused";
        case GameLogStatus::kCompleted: return "completed";
        default:                        return "unknown";
    }
}

// ─── API Error Codes ─────────────────────────────────────────

enum class ApiErrorCode {
    // Success
    kSuccess = 0,

    // HTTP-level errors
    kUnauthorized  = 401,
    kNotFound      = 404,
    kConflict      = 409,
    kInternalError = 500,

    // Adapter errors (1001-1999)
    kAdapterNotFound      = 1001,
    kAdapterAlreadyExists = 1002,
    kAdapterConnectFailed = 1003,
    kAdapterTimeout       = 1004,
    kAdapterInvalidConfig = 1005,
    kAdapterDisabled      = 1006,

    // Dice engine errors (2001-2999)
    kDiceInvalidExpression = 2001,
    kDiceRuleNotFound      = 2002,
    kDiceInvalidRuleValue  = 2003,

    // Migration errors (3001-3099)
    kMigrationVersionMismatch = 3001,
    kMigrationSchemaFailed    = 3002,
    kMigrationDataCorrupted   = 3003,
    kMigrationLegacyNotFound  = 3004,
    kMigrationFormatUnknown   = 3005,
    kMigrationImportFailed    = 3006,
    kMigrationBackupFailed    = 3007,
    kMigrationAlreadyRunning  = 3008,
    kMigrationPartialSuccess  = 3009,
};

inline int toInt(ApiErrorCode code) {
    return static_cast<int>(code);
}

inline ApiErrorCode intToApiErrorCode(int code) {
    switch (code) {
        case 0:    return ApiErrorCode::kSuccess;
        case 401:  return ApiErrorCode::kUnauthorized;
        case 404:  return ApiErrorCode::kNotFound;
        case 409:  return ApiErrorCode::kConflict;
        case 500:  return ApiErrorCode::kInternalError;
        case 1001: return ApiErrorCode::kAdapterNotFound;
        case 1002: return ApiErrorCode::kAdapterAlreadyExists;
        case 1003: return ApiErrorCode::kAdapterConnectFailed;
        case 1004: return ApiErrorCode::kAdapterTimeout;
        case 1005: return ApiErrorCode::kAdapterInvalidConfig;
        case 1006: return ApiErrorCode::kAdapterDisabled;
        case 2001: return ApiErrorCode::kDiceInvalidExpression;
        case 2002: return ApiErrorCode::kDiceRuleNotFound;
        case 2003: return ApiErrorCode::kDiceInvalidRuleValue;
        case 3001: return ApiErrorCode::kMigrationVersionMismatch;
        case 3002: return ApiErrorCode::kMigrationSchemaFailed;
        case 3003: return ApiErrorCode::kMigrationDataCorrupted;
        case 3004: return ApiErrorCode::kMigrationLegacyNotFound;
        case 3005: return ApiErrorCode::kMigrationFormatUnknown;
        case 3006: return ApiErrorCode::kMigrationImportFailed;
        case 3007: return ApiErrorCode::kMigrationBackupFailed;
        case 3008: return ApiErrorCode::kMigrationAlreadyRunning;
        case 3009: return ApiErrorCode::kMigrationPartialSuccess;
        default:   return ApiErrorCode::kInternalError;
    }
}

inline const char* apiErrorCodeMessage(ApiErrorCode code) {
    switch (code) {
        case ApiErrorCode::kSuccess:                  return "success";
        case ApiErrorCode::kUnauthorized:             return "unauthorized";
        case ApiErrorCode::kNotFound:                 return "not found";
        case ApiErrorCode::kConflict:                 return "conflict";
        case ApiErrorCode::kInternalError:            return "internal server error";
        case ApiErrorCode::kAdapterNotFound:          return "adapter not found";
        case ApiErrorCode::kAdapterAlreadyExists:     return "adapter already exists";
        case ApiErrorCode::kAdapterConnectFailed:     return "adapter connection failed";
        case ApiErrorCode::kAdapterTimeout:           return "adapter operation timed out";
        case ApiErrorCode::kAdapterInvalidConfig:     return "invalid adapter configuration";
        case ApiErrorCode::kAdapterDisabled:          return "adapter is disabled";
        case ApiErrorCode::kDiceInvalidExpression:    return "invalid dice expression";
        case ApiErrorCode::kDiceRuleNotFound:         return "dice rule not found";
        case ApiErrorCode::kDiceInvalidRuleValue:     return "invalid dice rule value";
        case ApiErrorCode::kMigrationVersionMismatch: return "migration version mismatch";
        case ApiErrorCode::kMigrationSchemaFailed:    return "migration schema operation failed";
        case ApiErrorCode::kMigrationDataCorrupted:   return "migration data is corrupted";
        case ApiErrorCode::kMigrationLegacyNotFound:  return "legacy data path not found";
        case ApiErrorCode::kMigrationFormatUnknown:   return "unknown legacy data format";
        case ApiErrorCode::kMigrationImportFailed:    return "legacy import failed";
        case ApiErrorCode::kMigrationBackupFailed:    return "database backup failed";
        case ApiErrorCode::kMigrationAlreadyRunning:  return "migration already in progress";
        case ApiErrorCode::kMigrationPartialSuccess:  return "migration partially succeeded";
        default:                                      return "unknown error";
    }
}

// ─── Convenience aliases ─────────────────────────────────────

using Timestamp = std::chrono::system_clock::time_point;

}  // namespace dice
