#pragma once
#include <string>
#include <chrono>
#include "cortrix/auth/auth_context.h"

namespace cortrix {

struct RequestContext {
    AuthContext auth;
    std::string request_id;
    std::chrono::steady_clock::time_point start_time;
};

/// Generate request_id: 16 random bytes -> 32-char hex string
std::string GenerateRequestId();

}  // namespace cortrix
