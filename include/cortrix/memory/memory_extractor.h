#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/memory/interaction_log.h"
#include "cortrix/memory/mem02_error.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/observability/trace_context.h"

// LLM memory extraction. Extends the existing
// cortrix::memory module (it does NOT replace interaction_log / memory_store /
// memory_writer). Standalone-D3 scope: the LLM extraction algorithm, the
// three-class memory model, the contradiction-judgment helper (mock QueryPipeline),
// invalidation writes to blocks.metadata_json, and operation_log emission — all
// against injected seams (ILlmClient, IMemoryBlockStore, IOperationLogger,
// IContradictionQuery) so it unit-tests with no live endpoint / DB / server.
//
// ⚠️ Contract reconciliation vs the D1 detail design (per B_R3_BRIEFING §2 — the
// detail design predates the implemented headers and drifts):
//   - input type is the real cortrix::InteractionLog (NOT a placeholder `Interaction`)
//   - there is NO `IBlockStore` in the tree; this file declares a minimal
//     IMemoryBlockStore seam in the existing-header style (MemoryStore is concrete +
//     SQLite-bound, so a thin interface is the standalone test seam; the real
//     MemoryStore-backed adapter is wired in D3.5)
//   - the operation logger is the real cortrix::observability::IOperationLogger
//     (NOT `cortrix::operation_log::IOperationLogger`)
//   - the contradiction path consumes the real single-arg
//     QueryPipeline::Execute(const QueryRequest&) via the IContradictionQuery seam
//     (NOT the stale two-arg AuthContext form)
namespace cortrix::memory {

/// Three-class memory model (2026-04-06 redesign). kUnknown is the
/// internal sentinel for an LLM `type` that does not parse to one of the three
/// classes; the code-layer fallback (D4) coerces it to kEvent before persistence.
enum class MemoryType {
    kFact = 0,    ///< objective, long-term — permanent immunity (D7)
    kPreference,  ///< personal preference — immune after N mentions in window (D8)
    kEvent,       ///< event/dynamic — exponential decay; also the D4 fallback class
    kUnknown,     ///< unparseable LLM type (never persisted; coerced to kEvent)
};

const char* ToString(MemoryType type);

/// Parse an LLM `type` string to a MemoryType. Unrecognized → kUnknown (the caller
/// applies the D4 fallback to kEvent). Case-insensitive on the three known values.
MemoryType ParseMemoryType(const std::string& s);

/// Memory lifecycle status persisted in blocks.metadata_json.
enum class MemoryStatus {
    kActive = 0,
    kTentative,
    kInvalidated,
};

const char* ToString(MemoryStatus status);

/// One memory the LLM extracted from the interaction window. Before
/// persistence the extractor mints `block_id` (ULID, id/ulid.h) and stamps the
/// source/method provenance.
struct ExtractedMemory {
    std::string block_id;             ///< minted ULID (empty until persisted)
    MemoryType type = MemoryType::kEvent;
    std::string content;
    double confidence = 0.0;
    std::string source_interaction_id;
    std::string source_session_id;
    std::string extraction_method = "llm";
    std::vector<std::string> invalidates;  ///< old block_ids this one invalidates (D6)
};

/// extract_meta block returned alongside the memories.
struct ExtractMeta {
    std::string llm_model;
    int llm_latency_ms = 0;
    int tokens_used = 0;
    double tokens_cost_usd = 0.0;
    int interaction_window_size = 0;
    int contradictions_found = 0;
    std::string extracted_at;         ///< ISO 8601
};

/// Result of extract() for one interaction. On the failure paths the
/// error is the Agent-friendly boundary type carrying a CX_ERR_MEMEXTRACT_* code (see
/// mem02_error.h); `ok()` mirrors error.has_value() == false.
struct MemoryExtractionResult {
    std::vector<ExtractedMemory> extracted_memories;
    std::optional<ExtractMeta> extract_meta;
    std::optional<agent_friendly::AgentFriendlyError> error;

    bool ok() const { return !error.has_value(); }
};

/// Batch result for extract_batch — one MemoryExtractionResult per
/// input turn, in order.
struct MemoryExtractionBatchResult {
    std::vector<MemoryExtractionResult> results;
};

/// One contradiction the judge confirmed (ContradictionPair). Pairs a
/// newly extracted memory with the existing block it invalidates.
struct ContradictionPair {
    std::string new_block_id;     ///< the new memory (may be empty pre-persist; matched by content)
    std::string new_content;
    std::string old_block_id;     ///< existing block being invalidated
    std::string old_content;
    std::string reason;
    double confidence = 0.0;
};

/// A memory block as stored (the subset extraction reads/writes). `metadata_json` is the
/// JSONB field extraction stamps with memory_type/status/provenance/invalidation state
/// This mirrors the shape the real `blocks` row exposes; the
/// D3.5 adapter maps it onto the concrete store.
struct MemoryBlockRecord {
    std::string block_id;
    std::string ns_id;
    std::string user_id;
    std::string content;
    nlohmann::json metadata_json;  ///< object; extraction writes the memory fields
};

/// Minimal memory-block persistence seam (existing-header interface style, cf.
/// ILlmClient / IOperationLogger). NOT the forbidden `IBlockStore` — it is scoped to
/// the metadata-block operations extraction needs and is mockable for standalone UT. The
/// real MemoryStore-backed adapter is wired in D3.5.
class IMemoryBlockStore {
public:
    virtual ~IMemoryBlockStore() = default;

    /// Persist a freshly extracted memory block. Implementations assign nothing
    /// (the caller has already minted block_id). Returns the persisted block_id on
    /// success.
    virtual Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& block) = 0;

    /// Update an existing block's content/metadata_json (used to stamp invalidation
    /// state on the old block, D6, and to mutate mention_count on preference upgrade).
    virtual Status UpdateMemoryBlock(const MemoryBlockRecord& block) = 0;

    /// Fetch one block by id (used by the revoke path to restore status=active).
    /// NotFound when absent.
    virtual Result<MemoryBlockRecord> GetMemoryBlock(const std::string& block_id) = 0;
};

/// Contradiction-retrieval seam over the Query unified read pipeline. Extraction
/// does NOT depend on the concrete QueryPipeline in standalone; it depends on this
/// seam, whose real adapter calls QueryPipeline::Execute(const QueryRequest&) (the
/// real single-arg signature) and is wired in D3.5. The mock returns candidate old
/// memories for a new fact so the judge prompt can run.
struct CandidateBlock {
    std::string block_id;
    std::string content;
    MemoryType type = MemoryType::kEvent;
    double score = 0.0;  ///< RRF fusion score from the read pipeline
};

class IContradictionQuery {
public:
    virtual ~IContradictionQuery() = default;

    /// Retrieve up to `top_k` existing memory blocks in `ns` semantically closest to
    /// `content` (vector + BM25 → RRF, per D5). The extractor then runs the
    /// contradiction-judgment prompt over each candidate.
    virtual std::vector<CandidateBlock> FindCandidates(const std::string& ns,
                                                       const std::string& content,
                                                       int top_k) = 0;

    /// D8 preference-upgrade lookup: find the existing active preference block in `ns`
    /// that represents the same preference as `content` (so its mention_count can be
    /// incremented), or std::nullopt if this is a first mention. Standalone returns a
    /// mock match; the real semantic/dedup match behind this seam is D3.5. Default
    /// no-match keeps existing mocks that don't override it compiling.
    virtual std::optional<CandidateBlock> FindMatchingPreference(const std::string& ns,
                                                                 const std::string& content) {
        (void)ns;
        (void)content;
        return std::nullopt;
    }
};

/// Memory-extraction configuration (cortrix.yaml `mem02.*`). Defaults match the spec.
struct MemoryExtractorConfig {
    bool enabled = true;                       ///< false = NullEnricher mode (LLM disabled)
    std::string llm_model = "gpt-4o-mini";     ///< default extraction model
    int llm_timeout_ms = 60000;                ///< per-call timeout (structured extraction +
                                               ///< contradiction judge are slower than a chat
                                               ///< turn; override via enricher_llm.timeout_ms)
    int window_size_default = 3;               ///< D3 multi-turn window (1-10)
    int window_size_max = 10;
    int preference_immunization_threshold = 2; ///< D8 N=2
    int preference_immunization_window_days = 30;
    double auto_revoke_confidence_threshold = 0.7;  ///< D6 low-confidence auto-revoke mark
    int contradiction_top_k = 5;               ///< D5 candidate count from read pipeline
    double tokens_cost_per_1k_usd = 0.0;       ///< optional cost model for extract_meta
};

/// MemoryExtractor — the extraction main class (directly holds the shared
/// ILlmClient; prompt templates live in this module's namespace). Extends the
/// existing cortrix::memory module; adds no dependency the frozen tree lacks.
///
/// Standalone-D3: every collaborator is an injected seam, so extract() runs fully
/// in unit tests against mocks (§12.3 eight LLM-mock scenarios). DEFERRED → D3.5:
/// the real MemoryStore adapter, the real QueryPipeline contradiction path, the
/// server memory_handler endpoints, the Python middleware async worker, and the
/// opt-out live wiring.
class MemoryExtractor {
public:
    /// @param llm        shared LLM client (ILlmClient; OpenAiLlmClient is the V1 impl)
    /// @param block_store memory-block persistence seam (D3.5 real adapter)
    /// @param contradiction_query contradiction-retrieval seam over the read pipeline (D5)
    /// @param op_logger  operation logger (CE; real cortrix::observability::IOperationLogger)
    /// @param config     mem02.* config
    MemoryExtractor(std::shared_ptr<llm::ILlmClient> llm,
                    std::shared_ptr<IMemoryBlockStore> block_store,
                    std::shared_ptr<IContradictionQuery> contradiction_query,
                    std::shared_ptr<observability::IOperationLogger> op_logger,
                    MemoryExtractorConfig config);

    // --- D12 entry points (Agent self-service extract) ---

    /// Extract memories from a single interaction (its window of preceding turns is
    /// supplied by the caller; standalone uses an explicit window — see
    /// ExtractFromWindow). `ctx` propagates the W3C trace (nullptr default).
    MemoryExtractionResult Extract(const InteractionLog& current_turn,
                                   const observability::TraceContext* ctx = nullptr);

    /// Extract over an explicit multi-turn window (D3). The last element is the
    /// current turn; earlier elements are context. This is the standalone-testable
    /// core (real get_window from the store is a D3.5 wiring concern).
    MemoryExtractionResult ExtractFromWindow(const std::vector<InteractionLog>& window,
                                             const observability::TraceContext* ctx = nullptr);

    /// Batch extract — one result per turn, in order.
    MemoryExtractionBatchResult ExtractBatch(const std::vector<InteractionLog>& turns,
                                             const observability::TraceContext* ctx = nullptr);

    // --- D9 revoke / manual invalidate (SDK admin) ---

    /// Revoke a prior invalidation (D9): restore the old block's status to active and
    /// emit a memory_revoke operation_log entry. `triggered_by` is one of
    /// "user_manual" / "agent_self" / "system_auto". Returns the restored block.
    Result<MemoryBlockRecord> RevokeInvalidation(const std::string& invalidated_block_id,
                                                 const std::string& reason,
                                                 const std::string& revoked_by,
                                                 const observability::TraceContext* ctx = nullptr);

    /// Manually invalidate a memory (D9 `memory.invalidate`): stamp status=invalidated
    /// (triggered_by=manual) and emit a memory_blocks_update operation_log entry.
    Status InvalidateMemory(const std::string& block_id,
                            const std::string& reason,
                            const observability::TraceContext* ctx = nullptr);

    // --- Internals exposed for unit testing (pure, deterministic) ---

    /// Build the LLM prompt input from a window: concatenates `role: content` lines
    /// (oldest→newest), truncated to the config window size.
    std::string BuildWindowText(const std::vector<InteractionLog>& window) const;

    /// Parse the LLM extraction JSON array → ExtractedMemory list. Applies the D4
    /// fallback (unknown type → event). Returns InvalidArgument (carrying
    /// CX_ERR_MEMEXTRACT_INVALID_OUTPUT identity via Mem02Status) on non-JSON /
    /// wrong-shape output.
    Result<std::vector<ExtractedMemory>> ParseExtractionJson(const std::string& llm_output) const;

    /// Parse the contradiction-judgment JSON object → (is_contradiction, reason,
    /// confidence). Invalid JSON → InvalidArgument.
    struct Judgment {
        bool is_contradiction = false;
        std::string reason;
        double confidence = 0.0;
    };
    Result<Judgment> ParseJudgmentJson(const std::string& llm_output) const;

    /// Run the contradiction pipeline for the extracted memories (D5): for each new
    /// fact, retrieve candidates (seam) + judge each (LLM). Returns the confirmed pairs.
    std::vector<ContradictionPair> FindContradictions(
        const std::string& ns,
        const std::vector<ExtractedMemory>& new_memories,
        const observability::TraceContext* ctx);

    /// Persist new memories + stamp invalidation state on contradicted old blocks +
    /// emit operation_log (D6 / §4.2.3). Fills each new memory's block_id and
    /// invalidates[]. `ns`/`user_id` come from the source interaction.
    Status WriteWithOperationLog(const std::string& ns,
                                 const std::string& user_id,
                                 std::vector<ExtractedMemory>& new_memories,
                                 const std::vector<ContradictionPair>& contradictions,
                                 const observability::TraceContext* ctx);

    /// D8 preference immunity: given the existing mention_count + first_mentioned_at
    /// of a matching preference and the config threshold/window, decide whether the
    /// preference is now immune (count+1 ≥ N within the window). Pure helper.
    static bool IsPreferenceImmune(int existing_mention_count,
                                   const std::string& first_mentioned_at_iso,
                                   const std::string& now_iso,
                                   int threshold,
                                   int window_days);

    const MemoryExtractorConfig& config() const { return config_; }

    /// D8 preference upgrade: if `mem` is a preference that matches an existing active
    /// preference (via the IContradictionQuery seam), increment that block's
    /// mention_count, stamp immunity when the threshold+window is met, and emit a
    /// memory_blocks_update operation_log entry. Returns true iff an existing
    /// preference was upgraded (so the caller skips inserting a duplicate block).
    bool MaybeUpgradePreference(const std::string& ns,
                                const std::string& now_iso,
                                const ExtractedMemory& mem,
                                const observability::TraceContext* ctx);

private:
    /// Construct the Agent-friendly LLM_DISABLED result (NullEnricher mode, D11).
    MemoryExtractionResult DisabledResult(const std::string& interaction_id) const;

    std::shared_ptr<llm::ILlmClient> llm_;
    std::shared_ptr<IMemoryBlockStore> block_store_;
    std::shared_ptr<IContradictionQuery> contradiction_query_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
    MemoryExtractorConfig config_;
};

}  // namespace cortrix::memory
