#pragma once
#include <string>

#include "cortrix/llm/i_llm_client.h"

namespace cortrix::llm {

/// Shared factory for canned ChatCompletionResponse objects (enricher Issue 5.2, 8
/// methods). Used with MockLlmClient (tests/mocks/mock_llm_client.h) to drive the
/// 7 LLM-consumer features' tests without a live endpoint. Success responses
/// carry the enricher's index-keyed JSON shape ({"0":{entities,summary,score},
/// ...}) so they exercise the real Issue 3.3 L1/L2/L3 parse path.
///
/// Lives in tests/mocks/ alongside MockLlmClient (the MVP scaffolding home for
/// shared mocks) rather than a separate tests/llm/ dir — one place for shared
/// test doubles. (design §5.4 envisioned tests/llm/; consolidated here.)
class MockResponseBuilder {
public:
    /// A valid response for `batch_size` chunks (Issue 5.2 #1). Each chunk index
    /// gets one entity + summary + score. usage tokens are set so budget/cost
    /// tests have non-zero input.
    static ChatCompletionResponse Normal(int batch_size,
                                         const std::string& model = "gpt-4o-mini");

    /// An empty-but-OK response (#2): empty content → parser sees L3 (no object).
    static ChatCompletionResponse Empty(const std::string& model = "gpt-4o-mini");

    /// 2xx with malformed (non-JSON) content (#3) → L3 whole-batch parse failure.
    static ChatCompletionResponse MalformedJson(const std::string& model = "gpt-4o-mini");

    /// 2xx but only `returned` of `expected` chunk indices present (#4) → the
    /// missing indices hit L2 in ParseEnrichBatchResponse.
    static ChatCompletionResponse PartialBatch(int returned, int expected,
                                               const std::string& model = "gpt-4o-mini");

    /// Timeout signal (#5): status carries the neutral transport token (the enricher
    /// pool models the *hard* timeout itself; this is the in-band variant the
    /// MockLlmClient returns when a test wants Chat() to report a timeout-like
    /// transport failure).
    static ChatCompletionResponse Timeout(const std::string& model = "gpt-4o-mini");

    /// HTTP error (#6): status carries CX_LLM_HTTP + the code (5xx retryable /
    /// 4xx permanent — the enricher classifies on the token).
    static ChatCompletionResponse HttpError(int status_code,
                                            const std::string& model = "gpt-4o-mini");

    /// 429 rate limit (#7): CX_LLM_RATE_LIMIT, Retry-After surfaced on finish_reason.
    static ChatCompletionResponse RateLimit(int retry_after_sec,
                                            const std::string& model = "gpt-4o-mini");

    /// Budget-exceeded signal (#8): a high-usage OK response so accumulating it
    /// trips the BudgetTracker cap (the enricher's own gate raises CX_ERR_ENRICHER_
    /// BUDGET; this just supplies the large token usage that crosses the cap).
    static ChatCompletionResponse BudgetExceeded(const std::string& model = "gpt-4o-mini");
};

}  // namespace cortrix::llm
