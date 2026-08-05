#pragma once
#include <memory>
#include <optional>
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/memory/memory_opt_out_error.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix {
class MemoryStore;  // forward decl — MemoryStoreOptOutAdapter holds a reference; the
                    // full definition (memory_store.h) is pulled in by the .cpp only.
}  // namespace cortrix

// Memory immunity. Extends the existing
// cortrix::memory module with a NEW namespace cortrix::memory::immunity; it does NOT
// replace the extractor / transparency surface / memory_store. It is the opt-out
// layer — mark a session "do not remember" so the extraction worker skips it
// (interaction_log is still written; only blocks-table extraction is skipped, D3).
//
// ⚠️ Contract reconciliation vs the D1 detail design (C_R2_BRIEFING §1 — the detail
// design predates the implemented headers and drifts):
//   - §3.2 names the session seam `IMemorySessionStore`; there is no such type in the
//     tree. The concrete session store is cortrix::MemoryStore (memory_store.h). Like
//     the transparency IMemoryBlockLister, this layer declares a minimal ISessionOptOutStore seam
//     (existing-header style) covering exactly the opt-out reads/writes; the real
//     MemoryStore-backed adapter (MemoryStoreOptOutAdapter, opt_out_manager.cpp) is
//     wired here, and standalone UT mocks the seam.
//   - the operation logger is the real cortrix::observability::IOperationLogger
//     (the CE operation-log contract). It is OPTIONAL (nullptr ok): the audit
//     audit hooks (memory_opt_out / memory_opt_out_revoke) are recorded when present,
//     but the real server wiring of the logger + the Python-middleware HTTP
//     check path are DEFERRED → D3.5.
//   - F-FREEZE-1: the design's `Result<T, ...>` (double-template) is forbidden. We use
//     `Result<T>` (StatusOr) + `Status`; the domain identity is carried via the
//     CX_ERR_MEMOPTOUT_* token (memory_opt_out_error.h MemoryOptOutStatus) and re-inflated to the
//     Agent-friendly body at the API boundary.
namespace cortrix::memory::immunity {

/// Who triggered an opt-out (D4 `opted_out_by` enum + §6.2 request enum). Stamped on
/// memory_sessions.opted_out_by and collapsed to the metric's triggered_by label.
enum class OptOutActor {
    kUserManual,   ///< "user_manual" — user asked explicitly (default)
    kAgentAuto,    ///< "agent_auto"  — Agent intent-detection self-service
    kSystemAuto,   ///< "system_auto" — a system policy opted the session out
    kTest,         ///< "test"        — a test/dev session, kept out of long-term memory
};

const char* ToString(OptOutActor actor);

/// The opt-out state of one session as the store sees it (ISessionOptOutStore read).
/// `exists` distinguishes "no such session" (→ SESSION_NOT_FOUND) from "active
/// session" (opted_out=false). When opted_out is true, opt_out_at/opted_out_by carry
/// the stamped values (for the ALREADY_OPTED_OUT structured_data).
struct SessionOptOutState {
    bool exists = false;
    bool opted_out = false;
    std::string opt_out_at;     ///< ISO 8601 (set when opted_out)
    std::string opted_out_by;   ///< actor string (set when opted_out)
};

/// Minimal opt-out seam over the session store (existing-header interface style,
/// cf. IMemoryBlockLister). MemoryStore has full session CRUD; the opt-out layer needs only
/// these three opt-out operations, so it depends on this thin seam. Standalone UT
/// mocks it; the real MemoryStore-backed adapter (MemoryStoreOptOutAdapter) pushes
/// these onto the memory_sessions opt_out_at / opted_out_by columns. All three return
/// an error Status on store failure.
class ISessionOptOutStore {
public:
    virtual ~ISessionOptOutStore() = default;

    /// Read the opt-out state of `session_id` (incl. whether the session row exists).
    virtual Result<SessionOptOutState> GetOptOutState(const std::string& session_id) = 0;

    /// Stamp `session_id` as opted-out at `opt_out_at` by `opted_out_by`. Caller has
    /// already verified existence + not-already-opted-out, so this is an
    /// unconditional UPDATE of the two columns.
    virtual Status SetOptOut(const std::string& session_id,
                             const std::string& opt_out_at,
                             const std::string& opted_out_by) = 0;

    /// Clear the opt-out stamp on `session_id` (opt_out_at / opted_out_by → empty).
    /// Caller has already verified the session exists + is currently opted-out.
    virtual Status ClearOptOut(const std::string& session_id) = 0;
};

/// Result of OptOut (200 body, minus the interactions_* counts which are a
/// query-time enrichment over the extraction queue). Carries the stamped values back.
struct OptOutResult {
    std::string session_id;
    std::string opt_out_at;
    std::string opted_out_by;
};

/// Result of OptOutRevoke (200 body).
struct RevokeResult {
    std::string session_id;
    std::string revoked_at;
};

/// OptOutManager — the opt-out main service class. Holds the opt-out seam and
/// (optionally) the operation logger. The extraction-worker collaboration (the
/// Python middleware HTTP-calling is_session_opted_out + writing the
/// memory_extract_skipped oplog) is NOT a hard dependency in standalone: this class
/// exposes is_session_opted_out() for that path, but the cross-process wiring is
/// DEFERRED → D3.5.
///
/// `mem04.enabled` (cortrix.yaml §4.3) gates opt_out/opt_out_revoke: when disabled the
/// mutating calls fail OPT_OUT_DISABLED (503). is_session_opted_out is NOT gated — the
/// worker must still honor existing opt-out stamps even if new opt-outs are off.
///
/// Standalone-D3: every collaborator is an injected seam, so all three operations run
/// fully in unit tests against a mock store (+ an optional mock logger).
class OptOutManager {
public:
    /// @param store      opt-out seam (D3.5 real MemoryStore adapter; UT mock)
    /// @param op_logger  operation logger (CE; real observability::IOperationLogger).
    ///                   OPTIONAL — nullptr skips the GEN-OperationLog audit hooks.
    /// @param enabled    mem04.enabled (cortrix.yaml §4.3) — false ⇒ mutating calls 503.
    explicit OptOutManager(std::shared_ptr<ISessionOptOutStore> store,
                           std::shared_ptr<observability::IOperationLogger> op_logger = nullptr,
                           bool enabled = true);

    /// D2 trigger — opt a session out of memory extraction. Validates session_id
    /// format (ULID/UUID), checks the feature is enabled + the session exists + is
    /// not already opted-out, stamps opt_out_at/opted_out_by, records the
    /// memory_opt_out oplog (GEN-OperationLog) + the opt_out metric. Idempotent at the
    /// API layer is the caller's choice; here a repeat is ALREADY_OPTED_OUT (409) per
    /// ARCH §4.1.11 (the §11.x idempotent-200 variant is an HTTP-handler concern, D3.5).
    /// `reason` (optional) is recorded in the oplog summary.
    Result<OptOutResult> OptOut(const std::string& session_id,
                                OptOutActor actor = OptOutActor::kUserManual,
                                const std::string& reason = "",
                                const observability::TraceContext* ctx = nullptr);

    /// D5 revoke — clear a session's opt-out (admin path). Only affects future
    /// interactions; already-skipped interactions are NOT re-processed (the §5.2
    /// note). Validates session_id format, checks enabled + exists + currently
    /// opted-out, clears the stamp, records the memory_opt_out_revoke oplog + metric.
    /// `reason` is REQUIRED (§6.2 requestBody) and recorded in the oplog summary.
    Result<RevokeResult> OptOutRevoke(const std::string& session_id,
                                      const std::string& reason,
                                      const observability::TraceContext* ctx = nullptr);

    /// Extraction-worker check (skip extraction). True iff `session_id` is currently
    /// opted-out. NOT gated by mem04.enabled (existing stamps are always honored).
    /// A non-existent session is treated as not-opted-out (false) — the worker has an
    /// interaction for it, so absence is a benign race, not an error. On store failure
    /// returns the error Status (the worker fails open per its own policy).
    Result<bool> is_session_opted_out(const std::string& session_id,
                                      const observability::TraceContext* ctx = nullptr);

    // --- Internals exposed for unit testing (pure / deterministic) ---

    /// True iff `session_id` is a recognizable id (non-empty ULID/UUID-ish token).
    /// Pure. The §11.x boundary matrix only requires rejecting empty / structurally
    /// invalid ids; a permissive "looks like ULID or UUID" check (length + charset)
    /// is used so real client ids (which MemoryStore also accepts) pass.
    static bool IsValidSessionId(const std::string& session_id);

    /// OptOutActor → the memory_sessions.opted_out_by string (D4 enum). Pure.
    static const char* ActorString(OptOutActor actor);

private:
    std::shared_ptr<ISessionOptOutStore> store_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
    bool enabled_;
};

/// Adapter that backs ISessionOptOutStore with the concrete cortrix::MemoryStore
/// (memory_sessions opt_out_at / opted_out_by columns). This is the real (non-mock)
/// store binding; the server wiring that constructs an OptOutManager over it lives in
/// the memory handler and is DEFERRED → D3.5. Declared here so the binding is a
/// first-class, testable unit now.
class MemoryStoreOptOutAdapter : public ISessionOptOutStore {
public:
    explicit MemoryStoreOptOutAdapter(::cortrix::MemoryStore& store);

    Result<SessionOptOutState> GetOptOutState(const std::string& session_id) override;
    Status SetOptOut(const std::string& session_id,
                     const std::string& opt_out_at,
                     const std::string& opted_out_by) override;
    Status ClearOptOut(const std::string& session_id) override;

private:
    ::cortrix::MemoryStore& store_;
};

}  // namespace cortrix::memory::immunity
