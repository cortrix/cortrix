#include <cstdint>
#include "cortrix/agent_trace/http_observability_middleware.h"

#include <array>
#include <chrono>
#include <random>

#include "cortrix/agent_trace/agent_trace_metrics.h"

namespace cortrix::agent_trace {

using observability::HttpHeaders;
using observability::ObservabilityContext;
using observability::ObservabilityValidator;
using observability::TraceContext;

namespace {

// One process-wide RNG for UUID generation (thread-safe via the mutex). UUIDs are
// not security-sensitive here (trace ids), so a Mersenne engine is fine.
std::mt19937_64& Rng() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

}  // namespace

std::string GenerateUuidV4() {
    auto& rng = Rng();
    const uint64_t hi = rng();
    const uint64_t lo = rng();
    // Lay out 16 bytes, set version (4) + variant (10xx) bits per RFC 4122.
    std::array<uint8_t, 16> b{};
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((hi >> (8 * i)) & 0xFF);
    for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<uint8_t>((lo >> (8 * i)) & 0xFF);
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);

    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    auto push = [&](uint8_t v) {
        s.push_back(kHex[(v >> 4) & 0xF]);
        s.push_back(kHex[v & 0xF]);
    };
    for (int i = 0; i < 16; ++i) {
        push(b[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) s.push_back('-');
    }
    return s;
}

HttpObservabilityMiddleware::HttpObservabilityMiddleware()
    : gen_(&GenerateUuidV4) {}

HttpObservabilityMiddleware::HttpObservabilityMiddleware(TraceIdGenerator gen)
    : gen_(std::move(gen)) {}

HttpObservabilityResult HttpObservabilityMiddleware::Process(
    const HttpHeaders& headers) const {
    HttpObservabilityResult out;
    ObservabilityContext& ctx = out.context;
    // created_at: the factory stamps it, but we build the context field-by-field
    // here (to record per-header warnings/metrics), so stamp it explicitly via the
    // helper. We reuse FromHttpHeaders' validation by re-validating per field so we
    // know WHICH header failed (the factory silently drops invalid ones).
    auto& metrics = AgentTraceMetrics::Instance();

    // X-Session-Id
    if (headers.Has("X-Session-Id")) {
        auto r = ObservabilityValidator::ValidateSessionId(headers.Get("X-Session-Id"));
        if (r.ok()) {
            ctx.session_id = r.value();
        } else {
            out.warnings.push_back("X-Session-Id");
            metrics.RecordInvalidHeader(AgentTraceMetrics::HeaderName::kSessionId);
        }
    }

    // X-Agent-Id
    if (headers.Has("X-Agent-Id")) {
        auto r = ObservabilityValidator::ValidateAgentId(headers.Get("X-Agent-Id"));
        if (r.ok()) {
            ctx.agent_id = r.value();
        } else {
            out.warnings.push_back("X-Agent-Id");
            metrics.RecordInvalidHeader(AgentTraceMetrics::HeaderName::kAgentId);
        }
    }

    // X-Trace-Id — validate; on absent/invalid, generate a fallback (topic 4).
    std::string trace_id;
    bool have_trace = false;
    if (headers.Has("X-Trace-Id")) {
        auto r = ObservabilityValidator::ValidateTraceId(headers.Get("X-Trace-Id"));
        if (r.ok()) {
            trace_id = r.value();
            have_trace = true;
        } else {
            out.warnings.push_back("X-Trace-Id");
            metrics.RecordInvalidHeader(AgentTraceMetrics::HeaderName::kTraceId);
        }
    }
    if (!have_trace) {
        trace_id = gen_();
        out.generated_trace_id = true;
    }
    TraceContext tc;
    tc.trace_id = trace_id;
    ctx.SetTraceContext(std::move(tc));

    // Stamp created_at (the field-by-field build skipped the factory).
    ctx.created_at = []{
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }();
    return out;
}

const std::vector<std::string>& HttpObservabilityMiddleware::CorsAllowedHeaders() {
    // §6.2 config/cors.yaml allowed_headers.
    static const std::vector<std::string> kHeaders = {
        "X-Session-Id", "X-Trace-Id", "X-Agent-Id", "Content-Type", "Authorization"};
    return kHeaders;
}

const std::vector<std::string>& HttpObservabilityMiddleware::CorsAllowedMethods() {
    // §6.2 — GET / POST / OPTIONS (preflight).
    static const std::vector<std::string> kMethods = {"GET", "POST", "OPTIONS"};
    return kMethods;
}

}  // namespace cortrix::agent_trace
