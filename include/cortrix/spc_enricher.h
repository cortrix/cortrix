#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/llm/i_llm_client.h"    // ILlmClient (injectable LLM seam)
#include "cortrix/spc/parser.h"          // DocumentMetadata SoT
#include "cortrix/spc_enricher/enricher_error.h"

namespace cortrix::reranker {            // frozen reranker classes the enricher reuses
class CircuitBreaker;
template <typename R> class RerankerThreadPool;  // R=float here (task payload via Slot)
}  // namespace cortrix::reranker

namespace cortrix::llm { class IHttpTransport; }  // startup-probe seam (S4.2)

namespace cortrix::spc {

class PromptTemplate;   // spc_enricher/prompt_template.h
class BudgetTracker;    // spc_enricher/budget_tracker.h (Wave 4)

// =============================================================================
// NER + Summary Enricher — ISpcEnricher framework
//
// V1: ISpcEnricher pluggable framework (NullEnricher default + LlmEnricher V1
// sole implementation). See design/features/enricher-ner-summary.md.
//
// 🔒 ISpcEnricher base SoT lock (v1.0.5/v1.0.6, design): Enrich = 3-param
// (chunk_text:const std::string& / doc_meta:const DocumentMetadata& / ctx:const
// ChunkContext&); IsAvailable() (NOT IsEnabled); Name() returns std::string.
// the contextual and HyPE subclasses already sit on this SoT — must not drift.
// =============================================================================

/// One NER-extracted entity (design). The design wrote char* for
/// text/type; reconciled to std::string here for the same reason the base SoT
/// locked chunk_text/Name() to std::string — avoid raw owning-pointer lifetime
/// management. Entity is a struct-internal value with no cross-Feature signature
/// lock, so this is a safe reconcile.
struct Entity {
    std::string text;
    std::string type;       ///< PERSON / ORG / DATE / MONEY / LOC / EVENT / PRODUCT (topic 3.2)
    int start_offset = 0;
    int end_offset = 0;
};

/// Agent-friendly error response metadata (design, Major-1 / GEN-Agent).
/// Populated on EnrichResult when status != 0. `category` reuses the FROZEN
/// agent_friendly::ErrorCategory (the design's parallel "EnricherCategory" enum
/// is reconciled away — same discipline as spc::parser.h). The canonical
/// per-code values come from enricher_error.h's registry; FromCode() builds this
/// from an EnricherErrorCode so call sites never restate retryable/category.
struct EnricherErrorMeta {
    bool retryable = false;                            ///< Agent decision: retry?
    agent_friendly::ErrorCategory category =
        agent_friendly::ErrorCategory::kPermanent;     ///< error class enum (frozen)
    int retry_after_ms = -1;                           ///< retry interval (0 / -1 = N/A)
    std::string structured_data;                       ///< JSON string (model / endpoint / ...)

    /// Build from a canonical EnricherErrorCode (registry-driven). `data` is the
    /// serialized structured_data JSON for this occurrence.
    static EnricherErrorMeta FromCode(EnricherErrorCode code, std::string data = "");
};

/// LLM enrichment output for one chunk (15 fields (+3 contextual,
///) = original 5 from ARCH + 6 added by topic 1.4 + topic 1.6
/// EnricherErrorMeta + 3 added by ContextualRetrieval). status == 0 = success;
/// otherwise one of the 6 EnricherErrorCode (cast to int) and error_meta filled.
struct EnrichResult {
    // The original 5 fields from ARCH
    int status = 0;                      ///< 0 = success / else (int)EnricherErrorCode
    std::vector<Entity> entities;        ///< NER-extracted entities
    std::string summary;                 ///< One-sentence summary
    float enriched_score = 0.0f;         ///< Post-enrichment semantic quality score [0.0, 1.0]
    std::string error_msg;               ///< Free-text error message (design char[256] → std::string)

    // 6 fields added by topic 1.4
    std::string prompt_version;          ///< A/B testing / traceability
    std::string model_used;              ///< Multi-model support
    int duration_ms = 0;                 ///< Performance diagnostics
    int token_count = 0;                 ///< Input for topic 3.5 cost calculation
    std::string enricher_name;           ///< Multi-Enricher chain provenance
    int64_t enriched_at = 0;             ///< hotness coordination (Unix epoch ms)

    // v1.0.2 Major-1: Agent-friendly error response (populated when status != 0, design)
    EnricherErrorMeta error_meta;        ///< topic 1.6 mapping of 6 error codes → 4 fields

    // ✨ ContextualRetrieval (pure ADD). Populated only by
    // ContextualRetrievalEnricher; the Null/Llm and HyPE enrichers leave these defaulted
    // (forward compatible). contextualized_status: 0=pending 1=generated 2=failed
    // 3=skipped_no_llm. The optionals are engaged only when actually produced; the
    // SPC pipeline writes them into the child contextualized_* columns.
    std::optional<std::string> contextualized_text;
    std::optional<std::vector<float>> contextualized_embedding;
    int contextualized_status = 0;

    bool ok() const { return status == 0; }
};

/// Per-chunk enrichment input (design, topic 1.5 ruling A + v1.0.4 G1.1).
///
/// 🔧 Reconcile (B-R1 standalone): the design wrote `const char*` text fields
/// + a `const DocumentMetadata&` reference member. A reference member makes
/// ChunkContext non-assignable, which is incompatible with the LOCKED
/// `EnrichBatch(const std::vector<ChunkContext>&)` signature (vector requires
/// assignable/copyable elements). Reconciled to value std::string text fields +
/// a non-owning `const DocumentMetadata*` so contexts are storable in a vector.
/// Semantics unchanged: doc_metadata must outlive the ChunkContext.
struct ChunkContext {
    std::string chunk_text;                       ///< Required: the current chunk's text
    int chunk_index = 0;                          ///< Required: ordinal within the doc (0-based)
    const DocumentMetadata* doc_metadata = nullptr;  ///< Required: DocumentMetadata SoT, non-owning
    // enricher_config: the per-call merged config travels separately to
    // EnrichBatch via the LlmEnricher's resolved EnricherConfig (topic 2.4 three-layer
    // override is applied by the enricher, not carried per-context) — avoids a
    // dangling reference member here. The design's "enricher_config required" intent
    // is satisfied by the enricher resolving it; per-request `enrich:bool` is the
    // only request-level knob (topic 2.5).

    // ✨ ContextualRetrievalEnricher consumes these; the Null/Llm enrichers
    // default ignore — forward compatible). Empty string == document boundary.
    std::string prev_chunk_text;                  ///< ("" == doc start)
    std::string next_chunk_text;                  ///< ("" == doc end)
};

// -----------------------------------------------------------------------------
// EnricherConfig + types (design)
// -----------------------------------------------------------------------------

/// Enricher backend kind (design EnricherType). local_ner = Phase 2.
enum class EnricherType {
    kNull,       ///< NullEnricher (default)
    kLlm,        ///< LlmEnricher (V1 sole implementation)
    kLocalNer,   ///< Phase 2
};

/// Parse a GUC string ("null"/"llm"/"local_ner") to EnricherType (default kNull
/// for unknown — fail-soft, the design's default path).
EnricherType ParseEnricherType(const std::string& s);
const char* EnricherTypeString(EnricherType t);

/// Upper sanity clamp for the batch-call max_tokens knob (QA 2026-07-12
/// F-7): the yaml side has only a ">0 use it" gate, so a nonsensical value
/// would otherwise ride straight into every provider request and 400 each
/// batch. Generous versus any real provider output budget.
inline constexpr int kEnricherMaxTokensCap = 32768;

/// Enricher configuration (design). A-class fields are global-only (GUC);
/// B-class fields (enabled / model / score_threshold / prompt_template_id) are
/// NS-overridable (topic 2.2). Defaults match the GUC table.
struct EnricherConfig {
    // A-class global (topic 2.1 + 3.5/3.6 = 11 items)
    EnricherType type = EnricherType::kNull;
    std::string endpoint = "https://api.openai.com/v1";
    std::string api_key;                          ///< V1.0 global; per-NS Phase 2
    int workers = 4;                              ///< topic 1.3
    int queue_size = 100;
    int task_timeout_ms = 30000;
    int batch_size = 8;                           ///< topic 1.2 (1-32)
    int max_tokens = 4096;                        ///< batch-call response budget; size it
                                                  ///< with batch_size (128 tokens/chunk at
                                                  ///< batch 32 truncates the batch JSON)
    bool circuit_breaker_enabled = true;          ///< topic 3.4
    int circuit_breaker_threshold = 10;
    int circuit_breaker_cooldown_sec = 60;        ///< 60s (differs from the reranker's 30s)
    int budget_cap_usd = 0;                       ///< topic 3.5 (0 = disabled)
    int startup_probe_timeout_ms = 5000;          ///< topic 3.6
    int http_retry_backoff_ms = 5000;             ///< transport/5xx retry backoff base
                                                  ///< (sleep = base × attempt; addendum
                                                  ///< A-part raises the old 50ms to seconds
                                                  ///< so inline retries survive short bursts)

    // B-class NS-overridable (topic 2.2 V1.0, 4 items)
    bool enabled = true;
    std::string model = "gpt-4o-mini";
    float score_threshold = 0.0f;
    std::string prompt_template_id = "default-zh";
};

/// inline HTTP attempts per LLM call (transport/5xx only; 429 returns
/// immediately for higher-level handling).
inline constexpr int kEnricherMaxHttpAttempts = 3;

// -----------------------------------------------------------------------------
// ISpcEnricher abstract interface (design, 🔒 base SoT lock)
// -----------------------------------------------------------------------------

/// Pluggable SPC enrichment stage. 🔒 SoT-locked signatures (v1.0.5/v1.0.6) —
/// The contextual and HyPE subclasses share these exact signatures; do not drift.
class ISpcEnricher {
public:
    virtual ~ISpcEnricher() = default;

    /// Single-chunk enrichment (topic 1.2 internal batch_size=1 path).
    /// 🔒 3-param signature (incl. ChunkContext&) — shared with the subclasses.
    virtual EnrichResult Enrich(const std::string& chunk_text,
                                const DocumentMetadata& doc_meta,
                                const ChunkContext& ctx) = 0;

    /// Batch enrichment (topic 1.2 main path, batch_size=8 default).
    virtual std::vector<EnrichResult> EnrichBatch(
        const std::vector<ChunkContext>& contexts) = 0;

    /// Availability (topic 1.1 upper-layer short-circuit + startup LLM probe).
    /// 🔒 Method name IsAvailable (NOT IsEnabled) — the subclasses already use it.
    virtual bool IsAvailable() const = 0;

    /// Enricher name (log / debug / EnrichResult.enricher_name).
    /// 🔒 v1.0.6 returns std::string (NOT const char*) — C++ override consistency
    /// with the subclasses.
    virtual std::string Name() const = 0;
};

/// Factory (design). V1 returns NullEnricher / LlmEnricher (mutually
/// exclusive per topic 4.5). type == kLlm but unavailable (api_key missing /
/// endpoint probe failed) auto-degrades to NullEnricher (topic 1.1 addendum),
/// recording the fallback metric. The production overload probes the
/// endpoint via the default transport (real network GET → integration).
std::unique_ptr<ISpcEnricher> CreateEnricher(const EnricherConfig& config);

/// Seam overload (S4.2): inject the startup-probe transport (tests / integration). Runs
/// the same validation against `probe_transport` instead of a live network.
std::unique_ptr<ISpcEnricher> CreateEnricher(const EnricherConfig& config,
                                             llm::IHttpTransport& probe_transport);

// -----------------------------------------------------------------------------
// NullEnricher (V1 default — design)
// -----------------------------------------------------------------------------

/// No-op enricher: the default + the fail-soft degradation target. Returns empty
/// EnrichResult (status=0, no entities, empty summary, score=0). IsAvailable() ==
/// false so the upper-layer short-circuit (topic 1.1) skips the whole stage.
class NullEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string& chunk_text,
                        const DocumentMetadata& doc_meta,
                        const ChunkContext& ctx) override;
    std::vector<EnrichResult> EnrichBatch(
        const std::vector<ChunkContext>& contexts) override;
    bool IsAvailable() const override { return false; }
    std::string Name() const override { return "NullEnricher"; }
};

// -----------------------------------------------------------------------------
// LlmEnricher (V1 sole real implementation — design)
// -----------------------------------------------------------------------------

/// LLM-backed enricher: combined NER + Summary over OpenAI-compatible chat
/// (topic 3). Wave 2 delivers the core path (PromptTemplate → ILlmClient.Chat →
/// topic 3.3 L1/L2/L3 parse → EnrichResult 12-field fill). Wave 3 adds the
/// ThreadPool (RerankerThreadPool) + CircuitBreaker + retry;
/// adds the BudgetTracker + startup probe / fallback.
///
/// 🔌 Network seam: the LLM is reached through an injected ILlmClient — the
/// production factory injects an OpenAiLlmClient (topic 4.3 shared module); tests
/// inject MockLlmClient. Real endpoint wiring → integration.
class LlmEnricher : public ISpcEnricher {
public:
    /// @param config  resolved EnricherConfig (A-class global + B-class, post three-layer
    ///                override). `enabled` + a non-null client => IsAvailable().
    /// @param client  injected LLM client (production: OpenAiLlmClient; tests:
    ///                MockLlmClient). Must be non-null for the enricher to be
    ///                available; a null client => IsAvailable()==false (degrade).
    LlmEnricher(const EnricherConfig& config, std::shared_ptr<llm::ILlmClient> client);
    ~LlmEnricher() override;

    EnrichResult Enrich(const std::string& chunk_text,
                        const DocumentMetadata& doc_meta,
                        const ChunkContext& ctx) override;
    std::vector<EnrichResult> EnrichBatch(
        const std::vector<ChunkContext>& contexts) override;
    bool IsAvailable() const override;
    std::string Name() const override { return "LlmEnricher"; }

    const EnricherConfig& config() const { return config_; }

private:
    /// Run one ≤batch_size group through the LLM + parse (Wave 2 synchronous
    /// core; Wave 3 wraps this in the pool + breaker + retry).
    std::vector<EnrichResult> RunOneBatch(const std::vector<ChunkContext>& group);

    EnricherConfig config_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    std::unique_ptr<PromptTemplate> prompt_template_;
    bool enabled_ = false;

    // Forward-declared (frozen reranker classes reused):
    std::unique_ptr<reranker::RerankerThreadPool<float>> thread_pool_;
    std::unique_ptr<reranker::CircuitBreaker> circuit_breaker_;
    // Wave 4: global budget cap (topic 3.5). null == cap disabled.
    std::unique_ptr<BudgetTracker> budget_tracker_;
};

}  // namespace cortrix::spc
