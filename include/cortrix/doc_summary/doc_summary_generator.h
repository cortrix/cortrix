#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/i_global_config.h"     // cortrix::IGlobalConfig (config resolver)
#include "cortrix/common/result.h"
#include "cortrix/doc_summary/doc_summary_config.h"
#include "cortrix/doc_summary/doc_summary_types.h"
#include "cortrix/llm/i_llm_client.h"          // llm::ILlmClient (injected seam)
#include "cortrix/store/chunk_store.h"          // store::ChunkStore (GetChunksByDocId)

namespace cortrix::doc_summary {

/// Resolved F41 config (F41 §4.4). Post-resolve result the generator consumes.
struct DocSummaryConfig {
    int max_chars = kMaxCharsDefault;                 ///< default 500 (200-500)
    int chunk_threshold = kChunkThresholdDefault;     ///< > this → map-reduce (default 50)
    std::string prompt_version = kPromptVersionDefault;  ///< "v1"
    std::string llm_model = kDefaultLlmModel;          ///< "gpt-4o-mini"
    bool fts5_fallback_enabled = kFts5FallbackEnabledDefault;
};

/// Resolve a DocSummaryConfig from `global` (F41 §4.4). Reads the doc_summary.*
/// keys via the generic IGlobalConfig accessors; any key absent / unparseable
/// keeps the built-in default (fail-soft). `global == nullptr` returns defaults.
DocSummaryConfig ResolveDocSummaryConfig(const cortrix::IGlobalConfig* global);

/// Document Summary generator (F41 §5.1) — an INDEPENDENT pipeline step (NOT an
/// ISpcEnricher subclass, unlike F38/F35). For each document it asks an LLM for a
/// structured JSON summary (summary_text + keywords + topics + one_liner), with a
/// map-reduce path for long documents (> chunk_threshold, F41-2/§9.2). The result
/// is written to a doc_summary Block (block_type=17) + indexed in P-HNSW; that
/// write + the F42 async scheduling + the embedding are cross-Feature wiring →
/// D3.5 (this round produces the parsed structured summary, standalone-testable).
///
/// 🔌 Seams (standalone reconcile): the design §5.1 wrote a concrete
/// OpenAiLlmClient + ParentChunkStore + F42TaskScheduler + IMetricsRegistry. Here
/// the LLM is the frozen llm::ILlmClient seam (production: OpenAiLlmClient; tests:
/// MockLlmClient); chunks come from the frozen store::ChunkStore seam
/// (GetChunksByDocId, tests: MockChunkStore); metrics use the self-contained
/// DocSummaryMetrics recorder (not IMetricsRegistry — same reconcile F38 made);
/// and the F42 scheduler is mocked (TaskType SoT = F42 §3.2). Real OnnxEmbedder /
/// F25 PWL write / F42 enqueue wiring = D3.5.
class DocSummaryGenerator {
public:
    /// @param config     resolved DocSummaryConfig.
    /// @param llm_client injected LLM client (production: OpenAiLlmClient; tests:
    ///                   MockLlmClient). Must be non-null to Generate.
    /// @param chunk_store injected chunk reverse-lookup (GetChunksByDocId for the
    ///                    map-reduce input; tests: MockChunkStore). Must be non-null.
    DocSummaryGenerator(const DocSummaryConfig& config,
                        std::shared_ptr<llm::ILlmClient> llm_client,
                        std::shared_ptr<store::ChunkStore> chunk_store);
    ~DocSummaryGenerator();

    /// Main entry (F42 async-task callback target). Loads the doc's chunks via
    /// ChunkStore, runs the single-call or map-reduce summary, parses the
    /// structured output, and returns it. On any failure GenerationResult.success
    /// is false and .error carries the CX_ERR_F41_* identity (the F42 worker maps
    /// it to retry / DLQ + doc_summary_status="failed"). `embedding` is left empty
    /// (the OnnxEmbedder re-embed is D3.5 pipeline wiring).
    GenerationResult Generate(const std::string& doc_id, const std::string& ns_id);

    /// §9.2 map-reduce: short docs (chunks <= chunk_threshold) → one structured
    /// call; long docs → Map each kMapGroupSize-chunk group to a partial summary,
    /// then Reduce the partials into the final structured summary. Exposed for
    /// tests; `doc_title` threads into the prompts.
    Result<DocSummaryStructured> GenerateSummary(
        const std::vector<store::ChunkRecord>& chunks, const std::string& doc_title,
        bool* used_map_reduce);

    /// Parse the LLM structured-JSON output (F41 §9.1) into the 4 fields. A
    /// non-object / missing-summary_text / unparseable body →
    /// CX_ERR_F41_LLM_INVALID_OUTPUT. summary_text is truncated to config.max_chars.
    /// Static so it is unit-testable in isolation.
    Result<DocSummaryStructured> ParseStructuredOutput(const std::string& llm_output,
                                                       int max_chars);

    /// Build the §9.1 v1 English structured-output prompt for the full document
    /// (short-doc path / Reduce input is BuildReducePrompt). `chunks_concatenated`
    /// is the joined chunk text. Exposed for tests asserting the rendered prompt.
    std::string BuildPrompt(const std::string& doc_title,
                            const std::string& chunks_concatenated) const;

    /// Build the §9.2 Map-stage prompt (a plain-text partial summary for one chunk
    /// group — not structured JSON).
    std::string BuildGroupSummaryPrompt(const std::string& doc_title,
                                        const std::string& group_text) const;

    /// Build the §9.2 Reduce-stage prompt (combine the Map partials into the final
    /// structured JSON summary).
    std::string BuildReducePrompt(const std::string& doc_title,
                                  const std::vector<std::string>& partials) const;

    const DocSummaryConfig& config() const { return config_; }

private:
    /// One structured-JSON LLM call (short-doc path + Reduce stage). Returns the
    /// parsed 4 fields or a CX_ERR_F41_* Status.
    Result<DocSummaryStructured> CallLlmStructured(const std::string& prompt);

    DocSummaryConfig config_;
    std::shared_ptr<llm::ILlmClient> llm_client_;
    std::shared_ptr<store::ChunkStore> chunk_store_;
};

}  // namespace cortrix::doc_summary
