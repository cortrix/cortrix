#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/llm/i_llm_client.h"        // llm::ILlmClient (injected seam)
#include "cortrix/spc/hype_config.h"          // kHype* config keys/defaults
#include "cortrix/spc_enricher.h"             // ISpcEnricher / EnrichResult / ChunkContext / DocumentMetadata
#include "cortrix/store/parent_chunk_store.h" // store::ParentChunkStore (parent_text reverse-lookup)

namespace cortrix::spc {

/// HyPE generation config. K + prompt version + LLM model. NS-config
/// (questions_per_chunk 1-10, S6) overrides `questions_per_chunk` at resolve time.
struct HyPEConfig {
    int questions_per_chunk = kHypeQuestionsDefault;   ///< K, default 3 (NS 1-10)
    std::string prompt_version = kHypePromptVersionDefault;  ///< "v1"
    std::string llm_model = kHypeDefaultLlmModel;      ///< "gpt-4o-mini"
    /// Per-call LLM deadline in ms; 0 = the shared client's default. The other enrichers
    /// honor enricher_llm.timeout_ms while HyPE historically ignored it — at
    /// provider peak (GLM evening, observed 2026-07-08) the 30s client default
    /// is the only protection hype generation gets. Wired from the same yaml key.
    int timeout_ms = 0;
};

/// One generated hypothetical question for a child chunk. The embedding
/// is filled by the SPC pipeline via OnnxEmbedder before the hype_question Block
/// is written (S3); HyPEEnricher itself produces question_text + provenance.
struct HypeQuestion {
    std::string question_text;
    int question_index = 0;                 ///< 0..K-1
    std::string source_child_id;            ///< Provenance primary key (recall expansion)
    std::string source_parent_id;           ///< Phase-1 written, Phase-2 dual-path
    std::vector<float> embedding;           ///< 1024-dim BGE-M3 (filled at S3 / pipeline)
};

/// HyPE enricher — an ISpcEnricher chain subclass (chunk-level, per-child),
/// peer of LlmEnricher / ContextualRetrieval.
///
/// ⚠️ Two contract reconciles (Lead-ruled 2026-06-01, the HyPE design
/// §7.1 / §6.3 reverse-revised to D3.5):
///   1. The frozen ISpcEnricher::EnrichResult (shared enricher type) has NO
///      hype_questions field and no generic extension slot, so Enrich() returns a
///      STANDARD EnrichResult (status only — HyPE does no NER, so entities/summary
///      stay empty). The hype questions are produced by the dedicated
///      GenerateHypeQuestions() below; the SPC pipeline consuming them into
///      hype_question Blocks (design §5.2 / §9.1 `result.hype_questions`) is
///      cross-Feature wiring → D3.5.
///   2. The frozen 3-param Enrich(chunk_text, doc_meta, ctx) carries NO parent_id
///      (neither DocumentMetadata nor ChunkContext has one), so parent_text is an
///      OPTIONAL context: GenerateHypeQuestions takes an explicit parent_text (the
///      caller resolves it via ParentChunkStore when it has a parent_id). The real
///      chunk→parent binding through the pipeline = D3.5. Standalone covers both
///      "with parent_text" and "no parent_text" paths.
///
/// 🔌 Network seam: the LLM is reached through an injected llm::ILlmClient (design
/// §5.1 wrote the concrete OpenAiLlmClient + an IMetricsRegistry — reconciled here
/// to the frozen ILlmClient seam; metrics use the self-contained HypeMetrics
/// recorder (S7), the IMetricsRegistry type does not exist in the frozen tree).
/// Tests inject MockLlmClient + MockParentChunkStore.
class HyPEEnricher : public ISpcEnricher {
public:
    /// @param config       resolved HyPEConfig (K / prompt_version / model).
    /// @param llm_client   injected LLM client (production: OpenAiLlmClient; tests:
    ///                      MockLlmClient). A null client => IsAvailable()==false.
    /// @param parent_store injected parent reverse-lookup (tests: MockParentChunkStore).
    ///                     May be null — parent_text simply stays empty (optional context).
    HyPEEnricher(const HyPEConfig& config,
                 std::shared_ptr<llm::ILlmClient> llm_client,
                 std::shared_ptr<store::ParentChunkStore> parent_store);
    ~HyPEEnricher() override;

    // --- ISpcEnricher (🔒 base SoT-locked signatures, shared with the other enrichers) ---

    /// HyPE enrichment for one chunk. Returns a STANDARD EnrichResult (reconcile 1):
    /// status=SUCCESS(0) on success / FAILED with error_meta on LLM failure
    /// (transparent degrade). entities/summary stay empty (HyPE does no NER).
    /// The generated questions are NOT carried here (the frozen EnrichResult has no
    /// slot) — call GenerateHypeQuestions() for them. enricher_name="hype".
    EnrichResult Enrich(const std::string& chunk_text,
                        const DocumentMetadata& doc_meta,
                        const ChunkContext& ctx) override;

    /// Batch form (4th pure-virtual — required). Runs Enrich() per context.
    std::vector<EnrichResult> EnrichBatch(
        const std::vector<ChunkContext>& contexts) override;

    /// Available iff a non-null LLM client was injected (a missing client
    /// degrades the whole HyPE stage, like the enricher's null-client path).
    bool IsAvailable() const override;

    /// Enricher name (EnrichResult.enricher_name / enricher_name="hype").
    std::string Name() const override { return "hype"; }

    // --- HyPE-specific API (reconcile 1: the real HyPE product) ---

    /// Generate K hypothetical questions for `chunk_text`. `parent_text`
    /// is OPTIONAL context (reconcile 2): empty == no parent context available.
    /// `source_child_id` / `source_parent_id` are the provenance written onto
    /// each question. On success returns exactly `config.questions_per_chunk`
    /// questions; on LLM failure / parse failure returns a non-OK Status carrying a
    /// CX_ERR_HYPE_* token (the caller / Enrich() maps it to the degrade path).
    /// `embedding` is left empty here (filled by the pipeline via OnnxEmbedder, S3).
    Result<std::vector<HypeQuestion>> GenerateHypeQuestions(
        const std::string& chunk_text,
        const std::string& parent_text,
        const DocumentMetadata& doc_meta,
        const std::string& source_child_id,
        const std::string& source_parent_id);

    /// Resolve parent_text via the injected ParentChunkStore. Returns
    /// empty string when parent_store is null OR `parent_id` is empty (optional
    /// context, reconcile 2). A store NOT_FOUND / DB error surfaces as a non-OK
    /// Status carrying CX_ERR_HYPE_PARENT_NOT_FOUND (caller decides whether to treat
    /// a missing parent as fatal or degrade to empty context).
    Result<std::string> ResolveParentText(const std::string& parent_id);

    /// Parse LLM output into exactly `expected_k` questions. Splits by
    /// newline, trims, drops empty lines. A count != expected_k is a parse failure
    /// → non-OK Status carrying CX_ERR_HYPE_QUESTION_PARSE_FAILED (transient,
    /// retryable) with the actual/expected counts; an entirely empty output →
    /// CX_ERR_HYPE_LLM_INVALID_OUTPUT. Static so it is unit-testable in isolation.
    static Result<std::vector<std::string>> ParseQuestions(
        const std::string& llm_output, int expected_k);

    /// Build the §6.1 v1 English prompt. `parent_text` is optional (reconcile 2):
    /// when empty the "Parent context" section is OMITTED entirely (not emitted
    /// with an empty body). Exposed for tests asserting the rendered prompt.
    std::string BuildPrompt(const std::string& chunk_text,
                            const std::string& parent_text,
                            const DocumentMetadata& doc_meta, int k) const;

    const HyPEConfig& config() const { return config_; }

private:

    HyPEConfig config_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    std::shared_ptr<store::ParentChunkStore> parent_store_;
};

}  // namespace cortrix::spc
