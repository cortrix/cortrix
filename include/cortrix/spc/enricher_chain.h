#pragma once
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "cortrix/common/i_global_config.h"  // IGlobalConfig (enricher.chain GUC + NS override)
#include "cortrix/spc/hype_enricher.h"        // HyPEEnricher / HypeQuestion
#include "cortrix/spc_enricher.h"             // ISpcEnricher / EnrichResult / ChunkContext

namespace cortrix::spc {

// =============================================================================
// ISpcEnricher chain framework (enrich → contextualize → HyPE, fail-soft serial).
//
// The SPC pipeline runs an ordered chain of ISpcEnrichers per chunk. Each enricher contributes
// independently (entities/summary; contextualized_*; hype questions
// via its own channel) and runs fail-soft — a failing enricher is skipped, its
// per-enricher status/error is recorded, and the chain continues. The chain order
// is fixed enrich → contextualize → HyPE (the later stages consume the chunk text, not the enricher's
// output, so the only ordering invariant is that the enricher leads).
//
// The chain is resolved from the `enricher.chain` GUC ("enrich" default; e.g.
// "enrich,contextual,hype") with an optional per-NS metadata override. Membership of an
// enricher is gated by BOTH the chain list AND the enricher's own IsAvailable()
// (an LLM-less contextualizer or HyPE stage degrades to a no-op).
// =============================================================================

/// Parse an `enricher.chain` spec ("enrich,contextual,hype") into a normalized, de-duplicated
/// token list. Tokens are lowercased + trimmed; unknown tokens are dropped
/// (fail-soft). An empty / absent spec yields {"enrich"} (the §7.1 default chain).
/// enrich is always implied first when any token is present (the enricher leads), so
/// "contextual" alone resolves to {"enrich","contextual"}. Pure + static for unit testing.
std::vector<std::string> ParseEnricherChainSpec(const std::string& spec);

/// Resolve the chain token list from IGlobalConfig (`enricher.chain` key) with an
/// optional per-NS metadata override JSON (the NS layer wins when it carries an
/// "enricher_chain" string). `global == nullptr` + empty ns_metadata → {"enrich"}.
std::vector<std::string> ResolveEnricherChain(const IGlobalConfig* global,
                                              const std::string& ns_metadata_json);

/// One enricher's outcome for a single chunk (per-enricher status/error_code, the
/// fail-soft bookkeeping the design requires). `name` = ISpcEnricher::Name();
/// `status` mirrors EnrichResult.status (0 == ok); `error_code` is the CX_ERR_*
/// token when the enricher degraded (empty on success / skip). Exception: the
/// The contextualizer fail-soft shape keeps status==0 (the chunk retains its original
/// embedding) while contextualized_status==2 — there error_code carries the
/// member's cause so debt rows record why (D12, 2026-07-11).
struct EnricherStepOutcome {
    std::string name;
    int status = 0;
    std::string error_code;
    bool skipped = false;   ///< enricher not available (IsAvailable()==false) — soft skip
};

/// Per-chunk merged enrichment result across the whole chain. `merged` is the
/// single EnrichResult the write phase consumes (entities/summary +
/// contextualized_* folded into one struct — the frozen EnrichResult already
/// carries both field families). `hype_questions` is the HyPE side channel
/// (reconcile 1: the frozen EnrichResult has no slot for them). `steps` records
/// each enricher's fail-soft outcome for observability / tests.
struct ChunkChainResult {
    EnrichResult merged;
    std::vector<HypeQuestion> hype_questions;
    std::vector<EnricherStepOutcome> steps;
};

/// Ordered, fail-soft ISpcEnricher chain. Owns its enrichers (enricher head + optional
/// contextualizer and HyPE stages). The SPC pipeline holds one of these (installed via a seam, like the
/// doc-summary enqueue) and calls EnrichChunks() once per document.
///
/// Construction: the production bootstrap builds the chain from
/// the resolved token list + the shared LLM client / embedder / parent store; an
/// empty chain (or all enrichers unavailable) makes EnrichChunks() a transparent
/// pass-through (the §7.1 L1 default).
class EnricherChain {
public:
    EnricherChain() = default;

    /// Append an enricher to the chain tail (call order = run order). Ownership is
    /// transferred. A null enricher is ignored.
    void Append(std::shared_ptr<ISpcEnricher> enricher);

    /// True when at least one chain member reports IsAvailable() — lets the pipeline
    /// short-circuit the whole stage (matches the single-enricher IsAvailable() gate
    /// it replaces).
    bool AnyAvailable() const;

    /// The ordered enricher names (for logging / tests).
    std::vector<std::string> Names() const;

    /// Run the chain over a batch of chunk contexts (one ChunkChainResult per
    /// context, index-aligned). For each chunk every available enricher runs in
    /// order; a throwing / failing enricher is caught, recorded in `steps`, and the
    /// chain continues (fail-soft). HyPE questions are collected via its
    /// GenerateHypeQuestions() side channel when a HyPE enricher is in the chain;
    /// `parent_texts` supplies the optional parent context per chunk (index-aligned;
    /// "" == no parent context). `source_child_ids` /
    /// `source_parent_ids` are the provenance stamped on each generated
    /// question (index-aligned; empty vectors → provenance left blank).
    ///
    /// `member_filter` (addendum §3.7 backfill): when non-null, only members whose
    /// ChainMemberToken is in the set run; the rest record a skipped step (same
    /// bookkeeping as an unavailable member). nullptr == run all (ingest path).
    std::vector<ChunkChainResult> EnrichChunks(
        const std::vector<ChunkContext>& contexts,
        const std::vector<std::string>& parent_texts,
        const std::vector<std::string>& source_child_ids,
        const std::vector<std::string>& source_parent_ids,
        const std::unordered_set<std::string>* member_filter = nullptr);

private:
    std::vector<std::shared_ptr<ISpcEnricher>> enrichers_;
};

/// Map an enricher's Name() to its chain-spec token ("enrich" / "contextual" / "hype").
/// The head slot (LlmEnricher / LocalNer / test fakes) is "enrich"; the token is the
/// vocabulary of enricher.chain specs AND of enrich_state.failed_members.
std::string ChainMemberToken(const std::string& enricher_name);

}  // namespace cortrix::spc
