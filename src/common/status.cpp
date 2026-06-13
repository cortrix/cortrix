#include "cortrix/common/status.h"

namespace cortrix {

int Status::http_status() const {
    switch (code_) {
        case StatusCode::kOk:               return 200;
        case StatusCode::kNotFound:         return 404;
        case StatusCode::kInvalidArgument:  return 400;
        case StatusCode::kInternal:         return 500;
        case StatusCode::kUnauthenticated:  return 401;
        case StatusCode::kPermissionDenied: return 403;
        case StatusCode::kUnavailable:      return 503;
        case StatusCode::kAlreadyExists:    return 409;
        default:                            return 500;
    }
}

std::string Status::error_code_string() const {
    switch (code_) {
        case StatusCode::kOk:               return "OK";
        case StatusCode::kNotFound:         return "NOT_FOUND";
        case StatusCode::kInvalidArgument:  return "INVALID_ARGUMENT";
        case StatusCode::kInternal:         return "INTERNAL_ERROR";
        case StatusCode::kUnauthenticated: {
            // Per API_GATEWAY_DESIGN.md section 2.5.2:
            //   "INVALID_API_KEY" for invalid keys
            //   "EXPIRED_API_KEY" for expired keys
            //   "UNAUTHORIZED" for missing auth header
            const auto& msg = message_;
            if (msg.find("expired") != std::string::npos ||
                msg.find("Expired") != std::string::npos) {
                return "EXPIRED_API_KEY";
            }
            if (msg.find("Invalid API key") != std::string::npos ||
                msg.find("invalid API key") != std::string::npos ||
                msg.find("Invalid api key") != std::string::npos) {
                return "INVALID_API_KEY";
            }
            return "UNAUTHORIZED";
        }
        case StatusCode::kPermissionDenied: {
            // Per API_GATEWAY_DESIGN.md section 2.5.2:
            //   "NAMESPACE_DENIED" for namespace access denied
            //   "FORBIDDEN" for general permission denied
            const auto& msg = message_;
            if (msg.find("namespace") != std::string::npos ||
                msg.find("Namespace") != std::string::npos) {
                return "NAMESPACE_DENIED";
            }
            return "FORBIDDEN";
        }
        case StatusCode::kUnavailable:      return "SERVICE_UNAVAILABLE";
        case StatusCode::kAlreadyExists:    return "CONFLICT";
        default:                            return "UNKNOWN";
    }
}

}  // namespace cortrix
