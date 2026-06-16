#include <cstdint>
#include "cortrix/middleware/rate_limiter.h"

#include <algorithm>
#include <chrono>
#include <ctime>

#include "cortrix/agent_friendly/error.h"

namespace cortrix::middleware {

namespace {

// Lock window after a stratum is breached (design sec 8.bis.2): 5 minutes.
constexpr int64_t kLockMs = 5 * 60 * 1000;

int64_t SteadyToWallMs(std::chrono::steady_clock::time_point steady_tp,
                       std::chrono::steady_clock::time_point steady_now) {
    // Translate a steady_clock point into an approximate wall-clock (system)
    // epoch-ms for the error body's reset_at / locked_until. We anchor on "now"
    // in both clocks so the offset is consistent within a request.
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                     steady_tp - steady_now).count();
    auto wall_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    return wall_now + delta;
}

}  // namespace

const char* ToString(RateLimitStratum stratum) {
    switch (stratum) {
        case RateLimitStratum::kPerIp:     return "per_ip";
        case RateLimitStratum::kPerApiKey: return "per_api_key";
        case RateLimitStratum::kGlobal:    return "global";
    }
    return "global";  // unreachable; defensive default
}

RateLimitRule DefaultIpRule() {
    return RateLimitRule{/*refill_per_sec=*/60.0 / 60.0, /*burst=*/100.0,
                         /*window=*/"60s", /*limit=*/60};
}

RateLimitRule DefaultApiKeyRule() {
    return RateLimitRule{/*refill_per_sec=*/300.0 / 60.0, /*burst=*/500.0,
                         /*window=*/"60s", /*limit=*/300};
}

RateLimitRule DefaultGlobalRule() {
    return RateLimitRule{/*refill_per_sec=*/1000.0, /*burst=*/1500.0,
                         /*window=*/"1s", /*limit=*/1000};
}

RateLimiter::RateLimiter()
    : RateLimiter(DefaultIpRule(), DefaultApiKeyRule(), DefaultGlobalRule()) {}

RateLimiter::RateLimiter(RateLimitRule ip_rule, RateLimitRule key_rule,
                         RateLimitRule global_rule)
    : ip_rule_(std::move(ip_rule)),
      key_rule_(std::move(key_rule)),
      global_rule_(std::move(global_rule)),
      clock_([] { return std::chrono::steady_clock::now(); }) {}

bool RateLimiter::CheckBucket(Bucket& bucket, const RateLimitRule& rule,
                              RateLimitStratum stratum,
                              std::chrono::steady_clock::time_point now,
                              RateLimitDecision* out) {
    if (!bucket.initialized) {
        bucket.tokens = rule.burst;
        bucket.last_refill = now;
        bucket.locked_until = std::chrono::steady_clock::time_point{};  // epoch
        bucket.initialized = true;
    }

    // Still inside a lock window from a prior breach -> deny.
    if (bucket.locked_until > now) {
        if (out) {
            out->allowed = false;
            out->stratum = stratum;
            out->limit = rule.limit;
            out->window = rule.window;
            out->remaining = 0.0;
            out->retry_after_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      bucket.locked_until - now).count();
            out->locked_until_unix_ms = SteadyToWallMs(bucket.locked_until, now);
            out->reset_at_unix_ms = out->locked_until_unix_ms;
        }
        return false;
    }

    // Refill proportional to elapsed time, capped at burst.
    double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(
                             now - bucket.last_refill).count();
    bucket.tokens = std::min(rule.burst, bucket.tokens + elapsed_sec * rule.refill_per_sec);
    bucket.last_refill = now;

    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return true;
    }

    // Bucket empty -> breach: start a 5-minute lock and deny.
    bucket.locked_until = now + std::chrono::milliseconds(kLockMs);
    if (out) {
        out->allowed = false;
        out->stratum = stratum;
        out->limit = rule.limit;
        out->window = rule.window;
        out->remaining = 0.0;
        out->retry_after_ms = kLockMs;
        out->locked_until_unix_ms = SteadyToWallMs(bucket.locked_until, now);
        out->reset_at_unix_ms = out->locked_until_unix_ms;
    }
    return false;
}

RateLimitDecision RateLimiter::Allow(const std::string& client_ip,
                                     const std::string& api_key) {
    auto now = clock_();
    std::lock_guard<std::mutex> lock(mu_);

    RateLimitDecision decision;

    // Evaluate denial without consuming first, so a denial in any stratum does
    // not burn tokens in the others (atomic decision). We check IP -> Key ->
    // global, then, if all pass, consume one token from each applicable stratum.
    // To keep it simple and correct under the lock, we consume as we go but only
    // when we know every prior stratum allowed; on the first denial we return
    // and leave the remaining strata untouched. Already-consumed earlier strata
    // are refunded so the request as a whole consumes nothing on denial.
    struct Consumed { Bucket* bucket; };
    std::vector<Consumed> consumed;
    auto refund = [&]() {
        for (auto& c : consumed) c.bucket->tokens += 1.0;
    };

    if (!client_ip.empty()) {
        Bucket& b = ip_buckets_[client_ip];
        if (!CheckBucket(b, ip_rule_, RateLimitStratum::kPerIp, now, &decision)) {
            return decision;  // nothing consumed yet
        }
        consumed.push_back({&b});
    }

    if (!api_key.empty()) {
        Bucket& b = key_buckets_[api_key];
        if (!CheckBucket(b, key_rule_, RateLimitStratum::kPerApiKey, now, &decision)) {
            refund();
            return decision;
        }
        consumed.push_back({&b});
    }

    if (!CheckBucket(global_bucket_, global_rule_, RateLimitStratum::kGlobal, now,
                     &decision)) {
        refund();
        return decision;
    }

    decision.allowed = true;
    return decision;
}

nlohmann::json RateLimiter::BuildErrorBody(const RateLimitDecision& decision) {
    auto unix_ms_to_iso = [](int64_t ms) -> std::string {
        std::time_t secs = static_cast<std::time_t>(ms / 1000);
        struct tm tm_buf;
        gmtime_r(&secs, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    };

    agent_friendly::AgentFriendlyError err;
    err.code = "CX_ERR_RATE_LIMITED";
    err.message = std::string("Rate limit exceeded for stratum ") +
                  ToString(decision.stratum);
    err.retryable = true;
    err.category = agent_friendly::ErrorCategory::kQuota;
    err.retry_after_ms = static_cast<int>(decision.retry_after_ms);
    err.structured_data = nlohmann::json{
        {"stratum", ToString(decision.stratum)},
        {"limit", decision.limit},
        {"window", decision.window},
        {"remaining", 0},
        {"reset_at", unix_ms_to_iso(decision.reset_at_unix_ms)},
        {"locked_until", unix_ms_to_iso(decision.locked_until_unix_ms)},
    };
    return agent_friendly::ToJson(err);
}

}  // namespace cortrix::middleware
