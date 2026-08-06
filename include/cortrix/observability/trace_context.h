#pragma once
#include <cstdint>
#include <string>

namespace cortrix::observability {

/// Trace context propagated through the call chain (the observability spec).
/// Phase 1: carries trace_id/span_id strings for log correlation. Phase 2:
/// promoted to a full OpenTelemetry SpanContext (flags + baggage).
///
/// This is the definition behind the forward declaration used by pointer in
/// shared interfaces (e.g. cortrix::store::IIndex, the OBS_SPEC `const
/// TraceContext* ctx = nullptr` signature convention).
struct TraceContext {
    std::string trace_id;            ///< W3C trace-id (32 hex chars)
    std::string span_id;             ///< W3C span-id (16 hex chars)
    std::uint8_t trace_flags = 0;    ///< sampling flags
    // Phase 2 reserved: std::map<std::string, std::string> baggage;
};

}  // namespace cortrix::observability
