#pragma once

#include <stdexcept>
#include <string>
#include <sstream>
#include "types.h"

namespace dice {

// ─── Application Exception ───────────────────────────────────
// Carries an ApiErrorCode for uniform REST API error responses.

class AppException : public std::runtime_error {
public:
    explicit AppException(ApiErrorCode code, const std::string& message = "")
        : std::runtime_error(buildMessage(code, message))
        , code_(code)
        , userMessage_(message.empty() ? std::string(apiErrorCodeMessage(code)) : message) {}

    explicit AppException(ApiErrorCode code, const std::string& message, const std::string& detail)
        : std::runtime_error(buildMessage(code, message) + " | detail: " + detail)
        , code_(code)
        , userMessage_(message)
        , detail_(detail) {}

    ApiErrorCode errorCode() const noexcept { return code_; }
    int errorCodeInt() const noexcept { return toInt(code_); }
    const std::string& userMessage() const noexcept { return userMessage_; }
    const std::string& detail() const noexcept { return detail_; }

private:
    ApiErrorCode code_;
    std::string userMessage_;
    std::string detail_;

    static std::string buildMessage(ApiErrorCode code, const std::string& extra) {
        std::ostringstream oss;
        oss << "[" << toInt(code) << "] " << apiErrorCodeMessage(code);
        if (!extra.empty()) {
            oss << ": " << extra;
        }
        return oss.str();
    }
};

// ─── Convenience factory functions ───────────────────────────

inline AppException makeNotFoundError(const std::string& resource, const std::string& id = "") {
    std::ostringstream msg;
    msg << resource;
    if (!id.empty()) msg << " '" << id << "'";
    msg << " not found";
    return AppException(ApiErrorCode::kNotFound, msg.str());
}

inline AppException makeConflictError(const std::string& detail) {
    return AppException(ApiErrorCode::kConflict, "resource conflict", detail);
}

inline AppException makeInternalError(const std::string& detail = "") {
    return AppException(ApiErrorCode::kInternalError, "internal server error", detail);
}

inline AppException makeBadRequestError(int code, const std::string& detail) {
    return AppException(intToApiErrorCode(code), detail);
}

}  // namespace dice
