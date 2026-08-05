#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/memory/memory_error.h"
#include "cortrix/memory/memory_extractor.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/observability/trace_context.h"

// Memory transparency. Extends the
// existing cortrix::memory module with a NEW namespace cortrix::memory::transparency;
// it does NOT replace the extractor / memory_store. It is the ordinary-user
// view layer — list / create / edit / delete CRUD self-service over a user's own memories +
// `?explain=true` to surface the LLM-extraction provenance, all over the same
// blocks.metadata_json store extraction writes.
//
// ⚠️ Contract reconciliation vs the D1 detail design (C_R1_BRIEFING §2 — the detail
// design predates the implemented headers and drifts):
//   - the real memory-block store seam is cortrix::memory::IMemoryBlockStore
//     (memory_extractor.h), NOT `storage::IBlockStore` (no such name in tree).
//     But that seam has NO list-by-user query, so this layer declares a minimal
//     IMemoryBlockLister seam (existing-header style) for the GET /memory path; the
//     real MemoryStore-backed adapter is wired in D3.5.
//   - the operation logger is the real cortrix::observability::IOperationLogger
//     (NOT `cortrix::operation_log::IOperationLogger`, which is stale).
//   - status / type / extraction_method / timestamps are NOT struct fields on
//     MemoryBlockRecord — extraction stamps them as keys inside metadata_json (JSONB).
//     Transparency reads/writes those same keys (status / memory_type / extraction_method /
//     user_id / source_session_id / source_interaction_id / invalidated_by_block_id /
//     invalidated_at), and ADDS §6.1 keys (revoked_at / deleted_by_user_id /
//     deleted_at / last_modified_at). `extraction_method` is a free string, so the
//     new value "user_edit" needs NO extractor enum change.
//   - F-FREEZE-1: the design's `Result<T, MemoryTransparencyError>` (double-template)
//     is forbidden. We use `Result<T>` (StatusOr) + `Status`; the domain identity is
//     carried via the CX_ERR_MEMORY_* token (memory_error.h MemoryStatus) and re-inflated
//     to the Agent-friendly body at the API boundary.
namespace cortrix::memory::transparency {

/// Transparency operation type. DELETE keeps the REST method name; the audit
/// action / metric op / MCP tool are all named `invalidate` (V8 G2 M1, soft-delete).
enum class MemoryTransparencyOp {
    kList,
    kCreate,
    kEdit,
    kDelete,
};

/// GET /memory filter parameters.
struct MemoryListFilter {
    std::string user_id;                       ///< L1 required (must == session user)
    bool include_invalidated = false;          ///< L2/L3 default false (active+tentative only)
    std::optional<std::string> memory_type;    ///< fact / preference / event (optional filter)
    int page = 0;
    int page_size = 50;                        ///< capped at kMaxPageSize
};

/// Max page_size accepted by List (`maximum: 200`). Larger requests are
/// clamped to this.
constexpr int kMaxPageSize = 200;

/// Max content length for create / edit (`maxLength: 2000`).
constexpr int kMaxContentLength = 2000;

/// One memory in the list response (MemoryListItem). The A-class fields are
/// always returned; the B-class fields are populated only when `explain=true` (the
/// §4.4 phased-rollout split). Values are projected from the block's metadata_json.
struct MemoryListItem {
    // A-class (default)
    std::string memory_id;
    std::string content;
    std::string memory_type;
    std::string status;                        ///< active / tentative / invalidated
    std::string user_id;
    std::optional<int64_t> extracted_at;       ///< epoch ms (nullopt if unstamped)
    std::optional<int64_t> invalidated_at;
    std::optional<int64_t> revoked_at;         ///< v1.0.6 schema addition (admin revoke time)

    // B-class (only when explain=true)
    std::optional<std::string> invalidated_by_block_id;
    std::optional<std::string> extraction_method;   ///< llm / explicit / user_edit / system
    std::optional<std::string> source_session_id;
    std::optional<std::string> source_interaction_id;
};

/// GET /memory response. `meta` carries the GEN-Agent A-class
/// data-integrity fields (total / page / coverage_ratio / latency_ms / warnings).
struct MemoryListResponse {
    std::vector<MemoryListItem> memories;
    int64_t total = 0;
    int page = 0;
    int page_size = 0;
    double coverage_ratio = 1.0;               ///< 1.0 in standalone (single store, no NS fan-out)
    int latency_ms = 0;
    std::vector<std::string> warnings;
};

/// PATCH /memory/{id} request.
struct MemoryEditRequest {
    std::string memory_id;
    std::optional<std::string> new_content;
    std::optional<std::string> new_memory_type;
    std::optional<int64_t> expected_modified_at;  ///< optimistic lock (vs last_modified_at)
};

/// POST /memory request. No LLM validation.
struct MemoryCreateRequest {
    std::string user_id;
    std::string content;
    std::string memory_type;                   ///< fact / preference / event
};

/// Result of Edit (200 body): the new block id + the old (now
/// invalidated) block id (issue-2 B model).
struct MemoryEditResult {
    std::string new_memory_id;
    std::string old_memory_id;
};

/// triggered_by enum for the operation_log hooks (the 4
/// values that distinguish the invalidation paths in the audit trail so they are not
/// lumped together).
///   user_manual  — user soft-delete (DELETE path)
///   user_edit    — PATCH cascade (Edit invalidates the old block)
///   admin_revoke — admin path (writes revoked_at; reverse-audited)
///   llm_auto     — LLM extraction auto-invalidate
enum class TriggeredBy {
    kUserManual,
    kUserEdit,
    kAdminRevoke,
    kLlmAuto,
};

const char* ToString(TriggeredBy triggered_by);

/// Minimal list-query seam over the memory blocks (existing-header interface style,
/// cf. IMemoryBlockStore / ILlmClient). IMemoryBlockStore only fetches by id;
/// the GET /memory path needs to enumerate a user's blocks, so this layer adds this thin
/// seam. Standalone UT mocks it; the real MemoryStore-backed adapter (a `SELECT ...
/// WHERE metadata_json->>'user_id'=? AND metadata_json->>'memory_type' IS NOT NULL`)
/// is wired in D3.5. NOT the forbidden `IBlockStore`.
class IMemoryBlockLister {
public:
    virtual ~IMemoryBlockLister() = default;

    /// Return every memory block owned by `user_id` (i.e. metadata_json carries that
    /// user_id and a non-null memory_type). The service applies status / memory_type
    /// filtering + pagination on the result, so the seam returns the full owned set
    /// (the real adapter pushes the predicates into SQL in D3.5). Returns an error
    /// Status on store failure.
    virtual Result<std::vector<MemoryBlockRecord>> ListByUser(const std::string& user_id) = 0;
};

/// MemoryTransparency — the transparency main service class. Holds the list seam,
/// the block store (read/write metadata_json), and the operation logger. The
/// MemoryExtractor is NOT a hard dependency in standalone ("reuse the extraction
/// invalidate flow" is satisfied directly via the block store + metadata_json stamping,
/// the same fields extraction writes); the real wiring to extraction and per-user isolation + the
/// server memory_handler endpoints is DEFERRED → D3.5.
///
/// Standalone-D3: every collaborator is an injected seam, so all four operations run
/// fully in unit tests against mocks.
class MemoryTransparency {
public:
    /// @param lister      list-by-user seam (D3.5 real adapter)
    /// @param block_store memory-block read/write seam (IMemoryBlockStore)
    /// @param op_logger   operation logger (CE; real observability::IOperationLogger)
    MemoryTransparency(std::shared_ptr<IMemoryBlockLister> lister,
                       std::shared_ptr<IMemoryBlockStore> block_store,
                       std::shared_ptr<observability::IOperationLogger> op_logger);

    /// GET /memory (issue-1 A' + issue-5 A + issue-6 phased rollout). Lists the
    /// session user's memories with status / memory_type filtering + pagination.
    /// `filter.user_id` MUST equal `session_user_id` (L1) — a mismatch is FORBIDDEN.
    /// `explain` adds the B-class provenance fields (§4.4).
    Result<MemoryListResponse> List(const MemoryListFilter& filter,
                                    const std::string& session_user_id,
                                    bool explain = false,
                                    const observability::TraceContext* ctx = nullptr);

    /// POST /memory (issue-1 A' + issue-3 A). Creates one user-explicit memory with
    /// no LLM validation; extraction_method = "explicit". Returns the new memory_id.
    /// Validates memory_type ∈ {fact,preference,event} + content length ≤ 2000.
    Result<std::string> Create(const MemoryCreateRequest& req,
                               const std::string& session_user_id,
                               const observability::TraceContext* ctx = nullptr);

    /// PATCH /memory/{id} (issue-1 A' + issue-2 B). Edits a memory by inserting a NEW
    /// block (extraction_method="user_edit") and invalidating the old one (status=
    /// invalidated, invalidated_by_block_id=new). Optimistic-locked on
    /// last_modified_at. Cross-user → 404 mask (L1.bis). Returns {new, old} ids.
    Result<MemoryEditResult> Edit(const MemoryEditRequest& req,
                                  const std::string& session_user_id,
                                  const observability::TraceContext* ctx = nullptr);

    /// DELETE /memory/{id} (issue-1 A' + L5 soft delete). Soft-deletes a memory
    /// (status=invalidated + deleted_by_user_id + deleted_at). Idempotent: a repeat
    /// on an already-invalidated block returns Ok (§4.2 step 2). Cross-user → 404 mask.
    /// Audit action = memory_invalidate, triggered_by = user_manual.
    Status Delete(const std::string& memory_id,
                  const std::string& session_user_id,
                  const observability::TraceContext* ctx = nullptr);

    // --- Internals exposed for unit testing (pure / deterministic where possible) ---

    /// Project a block's metadata_json onto a MemoryListItem (A-class always; B-class
    /// only when `explain`). Pure given the record.
    static MemoryListItem ProjectListItem(const MemoryBlockRecord& rec, bool explain);

    /// True iff `rec`'s metadata_json status counts as a valid/effective memory
    /// (active or tentative, per J5). Used by the include_invalidated=false filter.
    static bool IsEffectiveStatus(const MemoryBlockRecord& rec);

private:
    /// issue-5 A + L1.bis: fetch a block by id and verify the session user owns it;
    /// a cross-user (or absent) block returns CX_ERR_MEMORY_NOT_FOUND (404 mask
    /// — the response cannot distinguish "absent" from "not yours"). Records the
    /// cross_user_blocked metric when masking a real cross-user hit.
    Result<MemoryBlockRecord> FetchOwnedMemory(const std::string& memory_id,
                                               const std::string& session_user_id);

    std::shared_ptr<IMemoryBlockLister> lister_;
    std::shared_ptr<IMemoryBlockStore> block_store_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
};

}  // namespace cortrix::memory::transparency
