#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/i_global_config.h"   // IGlobalConfig (NS-config three-layer override)
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/llm/i_llm_client.h"          // llm::ILlmClient (injected seam)
#include "cortrix/spc/contextual_config.h"      // kContextual* keys/defaults
#include "cortrix/spc/onnx_embedder.h"          // cortrix::OnnxEmbedder (ContextualOnnxEmbedder adapter)
#include "cortrix/spc_enricher.h"               // ISpcEnricher / EnrichResult / ChunkContext / DocumentMetadata

namespace cortrix::spc {

/// Embedder seam for the contextualized re-embedding (`embedder_`).
///
/// 🔌 Reconcile (standalone): the design's §5.1 wrote a concrete
/// `std::shared_ptr<BgeM3Embedder>` member, but the frozen embedder
/// (cortrix::OnnxEmbedder) is a concrete, non-virtual class with no shared base
/// interface — so it cannot be a mockable injection point directly. We introduce
/// this F35-local virtual seam (same role ILlmClient plays for OpenAiLlmClient):
/// production injects an adapter wrapping OnnxEmbedder (ContextualOnnxEmbedder,
/// below); tests inject a fake. Wiring the real OnnxEmbedder through the SPC
/// pipeline is cross-Feature integration → D3.5. A null embedder means "no
/// re-embedding available" — Enrich() then produces contextualized_text only and
/// reports CX_ERR_CONTEXTUAL_EMBEDDING_FAILED for the embedding half.
class IContextualEmbedder {
public:
    virtual ~IContextualEmbedder() = default;

    /// Embed `text` into a dense vector (BGE-M3 1024-dim). Never throws; a failure
    /// is reported as a non-OK Status (the enricher maps it to
    /// CX_ERR_CONTEXTUAL_EMBEDDING_FAILED + a transparent degrade, §7.3).
    virtual Result<std::vector<float>> Embed(const std::string& text) = 0;
};

/// Resolved contextual-retrieval config. The three-layer override (built-in
/// default → IGlobalConfig global → per-NS metadata) is applied by
/// ResolveContextualConfig(); this struct is the post-resolve result the enricher
/// consumes. Mirrors the HyPEConfig shape.
struct ContextualRetrievalConfig {
    bool enabled = true;
    std::string prompt_template;   ///< empty == use the built-in Anthropic default (§6.1)
    int max_output_tokens = kContextualMaxOutputTokensDefault;  ///< 80 default (40-200)
    std::string llm_model = kContextualDefaultLlmModel;          ///< "gpt-4o-mini"
    int timeout_ms = kContextualTimeoutMs;  ///< per-call LLM deadline (§6.1 10s default)
    /// §8 injection-guard bytes-per-token multiplier (default 2, range 1-20).
    /// guard_limit = max_output_tokens x this; see kContextualGuardCharsPerTokenKey.
    int guard_chars_per_token = kContextualGuardCharsPerTokenDefault;
};

/// Resolve a ContextualRetrievalConfig from `global` (three-layer
/// override, global layer). Reads the kContextual* keys via the generic
/// IGlobalConfig accessors; any key absent / unparseable keeps the built-in
/// default (fail-soft). `global == nullptr` returns all built-in defaults.
/// max_output_tokens is clamped to [40,200] (§6.2 / prompt-injection guard).
/// The per-NS metadata layer is applied on top by the caller (NS metadata is a
/// JSON blob resolved at the pipeline boundary → D3.5); exposed as a separate
/// MergeNsOverride() so each layer is unit-testable.
ContextualRetrievalConfig ResolveContextualConfig(const IGlobalConfig* global);

/// Apply a per-NS override JSON object onto a base config (NS layer).
/// Recognized keys: "prompt_template" (string), "max_output_tokens" (int,
/// clamped), "llm_model" (string), "enabled" (bool). Unknown keys ignored;
/// malformed values keep the base value (fail-soft). Static + pure so it is
/// unit-testable without an IGlobalConfig.
ContextualRetrievalConfig MergeNsOverride(const ContextualRetrievalConfig& base,
                                          const std::string& ns_metadata_json);

/// Contextual Retrieval enricher — an ISpcEnricher chain subclass
/// (chunk-level, per-child), peer of LlmEnricher / HyPEEnricher. Chain
/// order enrich → contextualize → HyPE. Implements the Anthropic 2024 Contextual
/// Retrieval scheme: at index time an LLM writes a short context prefix for each
/// chunk, the prefix+chunk is re-embedded, and that contextualized embedding joins
/// P-HNSW alongside the original child embedding (double-vector coexistence,
/// The 5-path chunk-level RRF fusion that consumes both vectors is owned
/// by the sparse retrieval path (retrieval/sparse_rrf.h FuseFivePathRrf, kContextualized) and wired
/// at D3.5 — this round produces contextualized_text + contextualized_embedding +
/// contextualized_status onto the (frozen) EnrichResult, which the pipeline writes
/// into the child-chunk contextualized_* columns.
///
/// 🔌 Network/embedding seams: the LLM is an injected llm::ILlmClient (production:
/// OpenAiLlmClient; tests: MockLlmClient). The re-embedding is an injected
/// IContextualEmbedder (production: ContextualOnnxEmbedder; tests: fake). Metrics
/// use the self-contained ContextualRetrievalMetrics recorder (S7), not an
/// IMetricsRegistry (which does not exist in the frozen tree — same reconcile
/// made). Three-layer fallback (§7): L1 NullEnricher zero-config (the chain simply
/// omits f35), L2 startup LLM-unavailable skip (IsAvailable()==false →
/// status=skipped_no_llm), L3 transient retry+degrade (LLM/embedding failure →
/// status=failed, fall back to the original child embedding).
class ContextualRetrievalEnricher : public ISpcEnricher {
public:
    /// @param config    resolved ContextualRetrievalConfig (post three-layer override).
    /// @param llm_client injected LLM client (production: OpenAiLlmClient; tests:
    ///                   MockLlmClient). A null client => IsAvailable()==false (L2 skip).
    /// @param embedder  injected re-embedding seam (production: ContextualOnnxEmbedder;
    ///                   tests: fake). May be null — the embedding half then degrades
    ///                   to CX_ERR_CONTEXTUAL_EMBEDDING_FAILED while contextualized_text is
    ///                   still produced (partial result, §7.3).
    ContextualRetrievalEnricher(const ContextualRetrievalConfig& config,
                                std::shared_ptr<llm::ILlmClient> llm_client,
                                std::shared_ptr<IContextualEmbedder> embedder);
    ~ContextualRetrievalEnricher() override;

    // --- ISpcEnricher (🔒 base SoT-locked signatures, shared with the other enrichers) ---

    /// Contextual enrichment for one chunk. Populates the contextual EnrichResult fields
    /// (contextualized_text / contextualized_embedding / contextualized_status) and
    /// leaves entities/summary empty (this stage does no NER). status=0 even on a degrade
    /// (the chunk Block is still written upstream with the original embedding); the
    /// contextualized_status field carries the outcome (1/2/3) and error_meta
    /// the Agent-friendly detail. enricher_name="f35_contextual_retrieval".
    EnrichResult Enrich(const std::string& chunk_text,
                        const DocumentMetadata& doc_meta,
                        const ChunkContext& ctx) override;

    /// Batch form (4th pure-virtual — required). Runs Enrich() per context.
    std::vector<EnrichResult> EnrichBatch(
        const std::vector<ChunkContext>& contexts) override;

    /// Available iff a non-null LLM client was injected AND config.enabled
    /// (a missing client / disabled config degrades the whole stage — the L2 path).
    bool IsAvailable() const override;

    /// Enricher name (EnrichResult.enricher_name, §5.1).
    std::string Name() const override { return "f35_contextual_retrieval"; }

    // --- F35-specific API ---

    /// Generate the contextualized prefix for `chunk_text`. On success
    /// returns the LLM prefix text. On LLM transport/timeout failure returns a
    /// non-OK Status carrying CX_ERR_CONTEXTUAL_LLM_FAILED; on an over-long output
    /// (length > 2 x max_output_tokens, §8 defense) returns
    /// CX_ERR_CONTEXTUAL_PROMPT_INJECTION. Null client => CX_ERR_CONTEXTUAL_STARTUP_NO_LLM.
    Result<std::string> GenerateContextualizedText(const std::string& chunk_text,
                                                   const DocumentMetadata& doc_meta,
                                                   const ChunkContext& ctx);

    /// Build the §6.1 Anthropic v1 English prompt. Uses config.prompt_template when
    /// non-empty (NS override, §6.2) substituting {{doc_title}} / {{section_heading}}
    /// / {{prev_chunk_text}} / {{chunk_text}} / {{next_chunk_text}}; otherwise emits
    /// the built-in default. Exposed for tests asserting the rendered prompt.
    std::string BuildPrompt(const std::string& chunk_text,
                            const DocumentMetadata& doc_meta,
                            const ChunkContext& ctx) const;

    const ContextualRetrievalConfig& config() const { return config_; }

private:
    ContextualRetrievalConfig config_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    std::shared_ptr<IContextualEmbedder> embedder_;
};

/// Production adapter: wraps the frozen concrete cortrix::OnnxEmbedder behind the
/// IContextualEmbedder seam (reuses the shared dense inference path). Kept
/// header-declared so the production factory (D3.5) can construct it; tests use
/// their own fake instead of touching ONNX. Defined in contextual_enricher.cpp.
class ContextualOnnxEmbedder : public IContextualEmbedder {
public:
    explicit ContextualOnnxEmbedder(std::shared_ptr<cortrix::OnnxEmbedder> embedder);
    Result<std::vector<float>> Embed(const std::string& text) override;

private:
    std::shared_ptr<cortrix::OnnxEmbedder> embedder_;
};

}  // namespace cortrix::spc
