#pragma once
#include <string>
#include <vector>

#include "cortrix/spc_enricher.h"  // EnrichResult, Entity

namespace cortrix::spc {

/// Parse one batch LLM response body into per-chunk EnrichResult[] with the
/// topic 3.3 three-layer fault tolerance (S2.4). `batch_size` = the number of
/// chunks sent (the result vector is always exactly this size, index-aligned).
///
/// Expected body shape (see prompt_template.cpp): a single JSON object keyed by
/// chunk index string → {entities:[{text,type,start_offset,end_offset}],
/// summary, score}.
///
/// Layers:
///   - L3 (whole-batch): body is not a parseable JSON object → EVERY result gets
///     status=kParse, layer="L3", score=0. (Caller does NOT count L3 toward the
///     circuit breaker — §4.2.)
///   - L2 (chunk missing): the object parses but a chunk index key is absent →
///     that result gets status=kParse, layer="L2"; others proceed.
///   - L1 (field missing/malformed inside a chunk): default empty value for the
///     bad field; the chunk is still status=0 (success) with whatever parsed
///     (topic 1.6: "that field defaults to empty / the rest is normal (acceptable)").
///
/// `model` / `prompt_version` are stamped onto every successful result (S2.5
/// fills duration_ms / token_count / enriched_at / enricher_name at the call
/// site). On L2/L3 the EnricherErrorMeta + error_msg are filled via the
/// enricher_error registry.
std::vector<EnrichResult> ParseEnrichBatchResponse(const std::string& body,
                                                   int batch_size,
                                                   const std::string& model,
                                                   const std::string& prompt_version);

}  // namespace cortrix::spc
