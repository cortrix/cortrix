#pragma once
// The credential a request presents, read one way for every caller: WithAuth
// authenticates it, and the rate limiter's per-API-key bucket is keyed by it. Two
// readings with different precedence would let a caller authenticate with one
// key while the limiter counts another.

#include <string>

#include "httplib.h"

namespace cortrix {

/// The token of an `Authorization: Bearer <token>` header; otherwise the
/// X-API-Key header; otherwise "". A Bearer header with nothing after the prefix,
/// or another scheme, falls through to X-API-Key.
inline std::string RequestCredential(const httplib::Request& req) {
    static const std::string kBearer = "Bearer ";
    const std::string authorization = req.get_header_value("Authorization");
    if (authorization.size() > kBearer.size() &&
        authorization.compare(0, kBearer.size(), kBearer) == 0) {
        return authorization.substr(kBearer.size());
    }
    return req.get_header_value("X-API-Key");
}

}  // namespace cortrix
