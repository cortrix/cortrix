#pragma once
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/memory/memory_extractor.h"
#include "cortrix/observability/trace_context.h"

// MEM02 contradiction-judgment helper (D5). Wraps the contradiction-detect prompt +
// the LLM judge call + the single-object JSON parse into one focused unit so the
// extractor's WriteWithOperationLog path stays readable and the judge logic is
// independently testable.
//
// Standalone-D3: depends only on the injected ILlmClient (mockable). The candidate
// retrieval that feeds it comes from IContradictionQuery (the read-pipeline seam in
// memory_extractor.h); the real QueryPipeline adapter behind that seam is D3.5.
namespace cortrix::memory {

class ContradictionDetector {
public:
    /// @param llm   shared LLM client (judge calls; OpenAiLlmClient is the V1 impl)
    /// @param model model name for the judge call (defaults to the extractor's model)
    /// @param timeout_ms per-call timeout for the judge
    ContradictionDetector(std::shared_ptr<llm::ILlmClient> llm,
                          std::string model,
                          int timeout_ms);

    /// Judge whether `new_content` contradicts `old_content` (D5 prompt). Returns the
    /// parsed judgment, or an error Status (CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT on bad
    /// JSON, CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT / _CONTRADICTION_AMBIGUOUS on the LLM
    /// fault paths) carried via Mem02Status.
    Result<MemoryExtractor::Judgment> Judge(const std::string& new_content,
                                            const std::string& old_content,
                                            const observability::TraceContext* ctx = nullptr);

    /// Parse the judge's single-object JSON output → Judgment. Public for direct unit
    /// testing of the parse contract.
    static Result<MemoryExtractor::Judgment> ParseJudgmentJson(const std::string& llm_output);

private:
    std::shared_ptr<llm::ILlmClient> llm_;
    std::string model_;
    int timeout_ms_;
};

}  // namespace cortrix::memory
