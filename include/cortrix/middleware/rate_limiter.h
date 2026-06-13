#pragma once
// F20 Security Hardening — rate limiting baseline (design topic 8, sec 8.bis).
//
// Three independent strata, each a token bucket (the design's recommended
// algorithm):
//   per IP:      60 req/min   (burst 100)
//   per API Key: 300 req/min  (burst 500)
//   global:      1000 req/s   (burst 1500)
// On breach a stratum locks for 5 minutes (each of IP / Key / global locks
// independently). A locked or empty bucket yields 429 CX_ERR_RATE_LIMITED with
// retry_after_ms and a structured_data block (stratum/limit/window/remaining/
// reset_at/locked_until), per design sec 8.bis.3.
//
// A 4th per-tenant stratum (design sec 8.bis.6) is Cloud V1.5+ and intentionally
// not built here. Wiring the limiter into the request path is a single ADD in
// main.cpp; the limiter itself is standalone and fully unit-tested.

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib.h"

namespace cortrix::middleware {

/// Which stratum a decision applies to. Serializes to the design's
/// per_ip / per_api_key / global strings.
enum class RateLimitStratum {
    kPerIp,
    kPerApiKey,
    kGlobal,
};

const char* ToString(RateLimitStratum stratum);

/// Per-stratum token-bucket parameters.
struct RateLimitRule {
    double refill_per_sec = 0.0;  ///< sustained rate (tokens added per second)
    double burst = 0.0;           ///< bucket capacity (max tokens)
    std::string window;           ///< human label for the error body (e.g. "60s")
    int64_t limit = 0;            ///< nominal limit for the error body
};

/// Default Phase 1 rules (design sec 8.bis.1).
RateLimitRule DefaultIpRule();
RateLimitRule DefaultApiKeyRule();
RateLimitRule DefaultGlobalRule();

/// Outcome of an Allow() check.
struct RateLimitDecision {
    bool allowed = true;
    RateLimitStratum stratum = RateLimitStratum::kGlobal;  ///< first stratum that denied
    int64_t limit = 0;
    std::string window;
    double remaining = 0.0;
    int64_t retry_after_ms = 0;
    int64_t reset_at_unix_ms = 0;     ///< when the denying bucket refills enough
    int64_t locked_until_unix_ms = 0; ///< end of the 5-min lock (0 if not locked)
};

/// Thread-safe 3-strata token-bucket limiter. One instance is shared across all
/// request threads. The clock is injectable for deterministic tests.
class RateLimiter {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    RateLimiter();
    RateLimiter(RateLimitRule ip_rule, RateLimitRule key_rule,
                RateLimitRule global_rule);

    /// Override the clock (tests). Default is std::chrono::steady_clock::now.
    void SetClock(Clock clock) { clock_ = std::move(clock); }

    /// Check + consume one token across all applicable strata for this request.
    /// `client_ip` and `api_key` may be empty (that stratum is skipped). Strata
    /// are checked global-last; the first denying stratum is reported and no
    /// token is consumed from any stratum on denial (atomic decision).
    RateLimitDecision Allow(const std::string& client_ip,
                            const std::string& api_key);

    /// Build the CX_ERR_RATE_LIMITED error body (design sec 8.bis.3) for a denied
    /// decision. retry_after_ms / structured_data follow the GEN-Agent schema.
    static nlohmann::json BuildErrorBody(const RateLimitDecision& decision);

private:
    struct Bucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point last_refill;
        std::chrono::steady_clock::time_point locked_until;  // epoch -> not locked
        bool initialized = false;
    };

    // Returns true if allowed; fills `out` with denial detail when not.
    bool CheckBucket(Bucket& bucket, const RateLimitRule& rule,
                     RateLimitStratum stratum,
                     std::chrono::steady_clock::time_point now,
                     RateLimitDecision* out);

    RateLimitRule ip_rule_;
    RateLimitRule key_rule_;
    RateLimitRule global_rule_;

    std::mutex mu_;
    std::unordered_map<std::string, Bucket> ip_buckets_;
    std::unordered_map<std::string, Bucket> key_buckets_;
    Bucket global_bucket_;
    Clock clock_;
};

}  // namespace cortrix::middleware
