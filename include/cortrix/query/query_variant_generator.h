#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/observability/trace_context.h"
#include "cortrix/query/rag_fusion_types.h"

namespace cortrix::query {

/// QueryVariantGenerator — LLM-backed query-variant generation.
///
/// 🔑 Standalone seam (B_R1_BRIEFING §7): the D1 design names the dependency
/// `f03::OpenAiLlmClient`, but the FROZEN scaffolding seam (D2-pre-3,
/// `cortrix/llm/i_llm_client.h`) is `cortrix::llm::ILlmClient` — the interface the
/// real `OpenAiLlmClient` implements, and which lists RAG-Fusion as one of its 7
/// consumers. We depend on the interface (the correct abstraction), and unit-test
/// against the frozen `cortrix::llm::MockLlmClient`. The REAL `OpenAiLlmClient.Chat()`
/// HTTP transport is delivered with the enricher → wiring a live client is a
/// **deferred** step (the cross-NS layer mocked IReranker the same way).
///
/// Prompt-injection hardening:
///   1. user input wrapped in `<USER_QUERY_{suffix}>...</USER_QUERY_{suffix}>`
///   2. {suffix} = a fresh random 8-char hex per call (defeats a user closing the tag)
///   3. LLM output strictly JSON-schema-validated (no free text accepted)
///   4. system-prompt keywords ("ignore previous"/"system:"/"forget all") detected
///      → warning logged (the delimiter + schema are the actual defense)
class QueryVariantGenerator {
public:
    /// @param llm  the shared OpenAI-compatible client (real or mock), or null when
    ///             no LLM is configured (CE OSS default). A null client is allowed:
    ///             Generate() then degrades to a single-query fallback (it never
    ///             dereferences a null llm), and callers can pre-check via has_llm().
    explicit QueryVariantGenerator(std::shared_ptr<llm::ILlmClient> llm);

    /// True iff an LLM client is configured (non-null). The rag-fusion gate
    /// (query_wiring) checks this to skip variant expansion entirely when no LLM is
    /// available, degrading the query to plain scatter (dense + BM25) — [R7].
    bool has_llm() const { return llm_ != nullptr; }

    /// Generate up to `config.variant_count` variants (NOT including the original
    /// query). On success returns the parsed QueryVariants; on failure returns a
    /// Status carrying the CX_ERR_RAG_FUSION_* identity (timeout / quota /
    /// circuit-open / invalid-response) — re-inflate to the Agent-friendly body via
    /// MakeRagFusionError() at the boundary.
    ///
    /// @param original_query the user/Agent query (untrusted — injection-guarded)
    /// @param config         the resolved NS config (variant_count / strategies / timeout)
    /// @param locale         prompt locale ("zh" default, "en" for English corpora such as BEIR)
    /// @param ctx            optional TraceContext (V1.0 OSS always nullptr; §9)
    Result<QueryVariants> Generate(
        const std::string& original_query,
        const RagFusionConfig& config,
        const std::string& locale = "zh",
        const observability::TraceContext* ctx = nullptr);

    /// Backward-compatible overload for existing callers that only pass a
    /// TraceContext; keeps the historical zh prompt default.
    Result<QueryVariants> Generate(
        const std::string& original_query,
        const RagFusionConfig& config,
        const observability::TraceContext* ctx) {
        return Generate(original_query, config, "zh", ctx);
    }

    /// The prompt-template version string surfaced in QueryVariants (B-class,
    /// ?explain=true only) and the IGlobalConfig default (§4.7). Stable token.
    static constexpr const char* kPromptVersion = "default-v1";

    // --- exposed for unit testing the injection-hardening + parsing in isolation ---

    /// Build the full LLM prompt for `original_query` using the `locale` template
    /// ("zh" default, else "en"), wrapping the query in
    /// `<USER_QUERY_{suffix}>...</USER_QUERY_{suffix}>`. `suffix` is the
    /// caller-supplied random hex (so tests are deterministic); production passes
    /// the per-call RandomSuffix().
    static std::string BuildPrompt(const std::string& original_query,
                                   int variant_count,
                                   const std::string& suffix,
                                   const std::string& locale);

    /// A fresh random 8-char lowercase-hex suffix.
    static std::string RandomSuffix();

    /// True iff `query` contains a known prompt-injection keyword.
    /// Case-insensitive substring match over a fixed keyword set.
    static bool ContainsInjectionKeyword(const std::string& query);

    /// v1.0.8 semantic-drift guard: true iff `variant_query` keeps a minimum
    /// overlap with `original_query`'s content tokens. Short keyword queries have
    /// too little lexical signal and are allowed through; longer factual queries
    /// must preserve at least one/two anchor terms so broad generic rewrites do not
    /// dominate BEIR-style retrieval.
    static bool ShouldKeepVariant(const std::string& original_query,
                                  const std::string& variant_query);

    /// Parse the LLM's JSON response into variant strings (strict
    /// schema). Expects `{"variants":[{"strategy":"..","query":".."}, ...]}`.
    /// Returns false (and fills `schema_error`) on any schema violation — a missing
    /// `variants` array, a non-object element, a missing/empty `query`, etc. On
    /// success `out` holds the variant query strings (de-duplicated, original
    /// excluded by the caller), capped at `max_variants`.
    static bool ParseVariantsJson(const std::string& llm_content,
                                  const std::string& original_query,
                                  int max_variants,
                                  std::vector<std::string>* out,
                                  std::string* schema_error);

private:
    std::shared_ptr<llm::ILlmClient> llm_;
};

}  // namespace cortrix::query
