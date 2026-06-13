#include "cortrix/observability/observability_context.h"

#include <chrono>
#include <cstdio>
#include <utility>

#include <nlohmann/json.hpp>

namespace cortrix::observability {

namespace {

// CX_ERR_F13_INVALID_FILTER token (F13 §9.2). Carried in the Status message so
// the exact F13 identity survives to the API boundary, which re-inflates the
// full Agent-friendly body (the §3.1 4-field schema). Kept as a literal here so
// this widely-included shared TU does not pull in agent_trace_error.h (S7).
constexpr const char* kInvalidFilterToken = "CX_ERR_F13_INVALID_FILTER";

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo:  return "info";
        case LogLevel::kWarn:  return "warn";
        case LogLevel::kError: return "error";
    }
    return "info";  // unreachable; defensive default
}

ObservabilityContext& ObservabilityContext::ThreadLocal() {
    thread_local ObservabilityContext instance;
    return instance;
}

const TraceContext* ObservabilityContext::GetTraceContext() const {
    return trace_.has_value() ? &*trace_ : nullptr;
}

void ObservabilityContext::SetTraceContext(TraceContext ctx) {
    trace_ = std::move(ctx);
}

void ObservabilityContext::ClearTraceContext() {
    trace_.reset();
}

std::string ObservabilityContext::FormatStructured(LogLevel level, const std::string& msg) const {
    nlohmann::json j;
    j["level"] = ToString(level);
    j["msg"] = msg;
    if (trace_.has_value()) {
        j["trace_id"] = trace_->trace_id;
        j["span_id"] = trace_->span_id;
    } else {
        j["trace_id"] = nullptr;
        j["span_id"] = nullptr;
    }
    return j.dump();
}

void ObservabilityContext::LogStructured(LogLevel level, const std::string& msg) {
    const std::string line = FormatStructured(level, msg);
    std::fprintf(stderr, "%s\n", line.c_str());
}

// ===== F13 identity extension (§5.1 v1.0.1) =====

ObservabilityContext ObservabilityContext::FromHttpHeaders(const HttpHeaders& headers) {
    ObservabilityContext ctx;
    ctx.created_at = NowMs();

    // Each header is optional; an invalid value is dropped (left unset). The
    // middleware (S2) emits the warning + cortrix_invalid_header_total metric —
    // the factory stays pure (no I/O) so it is unit-testable in isolation.
    if (auto r = ObservabilityValidator::ValidateSessionId(headers.Get("X-Session-Id")); r.ok()) {
        ctx.session_id = r.value();
    }
    if (auto r = ObservabilityValidator::ValidateTraceId(headers.Get("X-Trace-Id")); r.ok()) {
        // X-Trace-Id seeds the trace chain; span_id is generated downstream. The
        // server-generated fallback when this is unset is the middleware's job (S2).
        TraceContext tc;
        tc.trace_id = r.value();
        ctx.trace_ = std::move(tc);
    }
    if (auto r = ObservabilityValidator::ValidateAgentId(headers.Get("X-Agent-Id")); r.ok()) {
        ctx.agent_id = r.value();
    }
    return ctx;
}

ObservabilityContext ObservabilityContext::FromMcpCapability(const McpSession& session) {
    ObservabilityContext ctx;
    ctx.created_at = NowMs();
    // session_id is already server-resolved (generated or validated) by
    // McpSessionHandler (§7.1) — taken as-is, not re-validated here.
    if (!session.session_id.empty()) ctx.session_id = session.session_id;
    ctx.agent_id = session.agent_id;
    ctx.namespace_id = session.namespace_id;
    return ctx;
}

void ObservabilityContext::SetThreadLocal() const {
    ObservabilityContext& tl = ThreadLocal();
    tl.session_id = session_id;
    tl.agent_id = agent_id;
    tl.user_id = user_id;
    tl.namespace_id = namespace_id;
    tl.created_at = created_at;
    if (trace_.has_value()) {
        tl.trace_ = trace_;
    } else {
        tl.trace_.reset();
    }
}

// ===== ObservabilityValidator (topic 4; template A) =====

bool ObservabilityValidator::IsValidFormat(const std::string& value, int max_length) {
    if (value.empty()) return false;
    if (static_cast<int>(value.size()) > max_length) return false;
    // Whitelist [a-zA-Z0-9_.:/-] (F13 §6.1). ASCII-only by construction.
    for (unsigned char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                        c == ':' || c == '/' || c == '-';
        if (!ok) return false;
    }
    return true;
}

namespace {

// Shared failure path: build the InvalidArgument Status with the CX_ERR_F13
// token + which field failed (the boundary re-inflates value_preview from the
// caller's raw value; we keep PII out of the Status message).
Status InvalidFilterStatus(const char* field) {
    return Status::InvalidArgument(std::string(kInvalidFilterToken) + ": invalid " + field);
}

}  // namespace

Result<std::string> ObservabilityValidator::ValidateSessionId(const std::string& value) {
    if (!IsValidFormat(value, kMaxIdentityLength)) return InvalidFilterStatus("X-Session-Id");
    return value;
}

Result<std::string> ObservabilityValidator::ValidateTraceId(const std::string& value) {
    if (!IsValidFormat(value, kMaxIdentityLength)) return InvalidFilterStatus("X-Trace-Id");
    return value;
}

Result<std::string> ObservabilityValidator::ValidateAgentId(const std::string& value) {
    if (!IsValidFormat(value, kMaxIdentityLength)) return InvalidFilterStatus("X-Agent-Id");
    return value;
}

}  // namespace cortrix::observability
