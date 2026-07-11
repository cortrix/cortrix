#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <optional>
#include <memory>
#include <string>
#include <thread>

#include "cortrix/logging/logging.h"
#include "cortrix/llm/llm_error_tokens.h"
#include "cortrix/reranker/circuit_breaker.h"        // F02 frozen — reused (B-R1 §2)
#include "cortrix/reranker/reranker_thread_pool.h"   // F02 frozen — reused (B-R1 §2)
#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/budget_tracker.h"     // W4 budget cap (§3.5)
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc_enricher/enricher_metrics.h"
#include "cortrix/spc_enricher/enricher_response_parser.h"
#include "cortrix/spc_enricher/prompt_template.h"

namespace cortrix::spc {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Map the feature-neutral OpenAiLlmClient status token (llm_error_tokens.h) to
// the F03 enricher error code (§4.2). RATE_LIMIT is distinct; transport / HTTP
// 5xx / bad-body all surface as LLM_API (transient, retryable per §5.1).
EnricherErrorCode ClassifyLlmFailure(const std::string& status_msg) {
    if (status_msg.rfind(llm::llm_tokens::kRateLimit, 0) == 0) {
        return EnricherErrorCode::kRateLimit;
    }
    return EnricherErrorCode::kLlmApi;
}

bool ModelSupportsThinkingControl(std::string model) {
    std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return model.find("deepseek") != std::string::npos;
}

std::optional<std::string> ParseFailureLayer(const EnrichResult& r) {
    if (r.status != static_cast<int>(EnricherErrorCode::kParse)) {
        return std::nullopt;
    }
    if (r.error_msg.find("(L2)") != std::string::npos) return std::string("L2");
    if (r.error_msg.find("(L3)") != std::string::npos) return std::string("L3");
    return std::string("unknown");
}

// Whole-batch error results (score=0 + registry-filled EnricherErrorMeta) when a
// batch ultimately fails (after retries / on timeout / breaker open).
std::vector<EnrichResult> BatchError(int n, EnricherErrorCode code,
                                     const std::string& model,
                                     const std::string& detail, int retry_count) {
    std::vector<EnrichResult> out(static_cast<size_t>(n < 0 ? 0 : n));
    nlohmann::json sd = {{"model", model}};
    switch (code) {
        case EnricherErrorCode::kLlmApi:
            sd["http_status"] = 0; sd["retry_count"] = retry_count; break;
        case EnricherErrorCode::kRateLimit:
            sd["http_status"] = 429; sd["retry_after_sec"] = 60; break;
        case EnricherErrorCode::kLlmTimeout:
            sd["duration_ms"] = 0; sd["task_timeout_ms"] = 0; break;
        default: break;
    }
    for (auto& r : out) {
        r.status = static_cast<int>(code);
        r.enriched_score = 0.0f;
        r.model_used = model;
        r.error_meta = EnricherErrorMeta::FromCode(code, sd.dump());
        r.error_msg = std::string(EnricherErrorCodeString(code)) + ": " + detail;
    }
    return out;
}

void StampResultDefaults(EnrichResult& r,
                         int duration_ms,
                         int token_count,
                         int64_t enriched_at,
                         const std::string& model,
                         const std::string& prompt_version) {
    r.duration_ms = duration_ms;
    r.token_count = token_count;
    r.enriched_at = enriched_at;
    if (r.model_used.empty()) r.model_used = model;
    if (r.enricher_name.empty()) r.enricher_name = "LlmEnricher";
    if (r.prompt_version.empty()) r.prompt_version = prompt_version;
}

void LogParseFailureSample(const EnrichResult& r,
                           int batch_size,
                           size_t result_index,
                           int chunk_index,
                           const std::string& model,
                           const llm::ChatCompletionResponse& chat,
                           const std::string& prompt_version,
                           const char* event) {
    const auto layer = ParseFailureLayer(r).value_or("unknown");
    CORTRIX_LOG_WARN(
        "spc",
        "F03 LlmEnricher parse failure {}: layer={} batch_size={} "
        "result_index={} chunk_index={} model={} content_source={} "
        "finish_reason={} content_length={} reasoning_content_length={} "
        "prompt_version={} error={}",
        event, layer, batch_size, result_index, chunk_index, model,
        chat.content_source, chat.finish_reason, chat.content_length,
        chat.reasoning_content_length, prompt_version, r.error_msg);
}

}  // namespace

LlmEnricher::LlmEnricher(const EnricherConfig& config,
                         std::shared_ptr<llm::ILlmClient> client)
    : config_(config),
      llm_client_(std::move(client)),
      prompt_template_(std::make_unique<PromptTemplate>(config.prompt_template_id)) {
    enabled_ = config_.enabled && llm_client_ != nullptr;

    // S3.1: bounded-concurrency pool (4 workers / queue 100 / task_timeout 30s,
    // topic 1.3) — F02 RerankerThreadPool reused (B-R1 §2). One pool task = one
    // Chat call (the §5.1 retry schedule runs on the caller thread), so the pool
    // timeout keeps its single-call meaning.
    thread_pool_ = std::make_unique<reranker::RerankerThreadPool<float>>(
        config_.workers, config_.queue_size, config_.task_timeout_ms);

    // S3.2: independent breaker (threshold 10 / cooldown 60s, topic 3.4) — F02
    // CircuitBreaker reused. Drives the state gauge + trips counter (S3.4).
    if (config_.circuit_breaker_enabled) {
        circuit_breaker_ = std::make_unique<reranker::CircuitBreaker>(
            config_.circuit_breaker_threshold, config_.circuit_breaker_cooldown_sec);
        circuit_breaker_->OnStateChange([](reranker::CircuitBreakerState s) {
            auto& m = EnricherMetrics::Instance();
            m.SetCircuitBreakerState(static_cast<int>(s));
            if (s == reranker::CircuitBreakerState::kOpen) m.RecordCircuitBreakerTrip();
        });
    }

    // S4.1: global budget cap (topic 3.5). 0 == disabled (AllowRequest always true).
    budget_tracker_ = std::make_unique<BudgetTracker>(config_.budget_cap_usd);
}

LlmEnricher::~LlmEnricher() = default;

bool LlmEnricher::IsAvailable() const { return enabled_; }

EnrichResult LlmEnricher::Enrich(const std::string& chunk_text,
                                 const DocumentMetadata& doc_meta,
                                 const ChunkContext& ctx) {
    // topic 1.2: single chunk goes through the batch path with batch_size=1.
    ChunkContext one = ctx;
    if (one.chunk_text.empty()) one.chunk_text = chunk_text;
    if (one.doc_metadata == nullptr) one.doc_metadata = &doc_meta;
    auto results = EnrichBatch({one});
    return results.empty() ? EnrichResult{} : results.front();
}

std::vector<EnrichResult> LlmEnricher::EnrichBatch(
    const std::vector<ChunkContext>& contexts) {
    std::vector<EnrichResult> out;
    out.reserve(contexts.size());
    if (contexts.empty()) return out;

    // §4.2: circuit breaker check. Open → all chunks score=0 (degrade to the
    // NullEnricher behavior for the cooldown window) without hitting the LLM.
    if (circuit_breaker_ && !circuit_breaker_->AllowRequest()) {
        EnricherMetrics::Instance().RecordFallbackToNull(
            EnricherMetrics::FallbackReason::kCircuitOpen);
        return BatchError(static_cast<int>(contexts.size()),
                          EnricherErrorCode::kLlmApi, config_.model,
                          "circuit breaker open", /*retry_count=*/0);
    }

    // §4.2: budget check (after the breaker). Over cap → all chunks score=0 +
    // CX_ERR_ENRICHER_BUDGET + degrade (operator must raise budget_cap_usd / wait
    // for the next cycle). 0 cap = disabled.
    if (budget_tracker_ && !budget_tracker_->AllowRequest()) {
        auto& m = EnricherMetrics::Instance();
        m.RecordFallbackToNull(EnricherMetrics::FallbackReason::kBudgetExceeded);
        m.RecordFailedTask(EnricherErrorCode::kBudget);
        nlohmann::json sd = {
            {"budget_cap_usd", budget_tracker_->budget_cap_usd()},
            {"current_cost_usd",
             static_cast<double>(budget_tracker_->TotalCostMicroUsd()) / 1e6},
            {"model", config_.model}};
        std::vector<EnrichResult> out(contexts.size());
        for (auto& r : out) {
            r.status = static_cast<int>(EnricherErrorCode::kBudget);
            r.enriched_score = 0.0f;
            r.model_used = config_.model;
            r.error_meta = EnricherErrorMeta::FromCode(EnricherErrorCode::kBudget, sd.dump());
            r.error_msg = std::string(EnricherErrorCodeString(EnricherErrorCode::kBudget)) +
                          ": budget cap reached";
        }
        return out;
    }

    const int batch_size = config_.batch_size > 0 ? config_.batch_size : 8;
    EnricherMetrics::Instance().SetQueueDepth(thread_pool_ ? thread_pool_->QueueDepth() : 0);

    for (size_t start = 0; start < contexts.size();
         start += static_cast<size_t>(batch_size)) {
        size_t end = std::min(contexts.size(), start + static_cast<size_t>(batch_size));
        std::vector<ChunkContext> group(contexts.begin() + static_cast<long>(start),
                                        contexts.begin() + static_cast<long>(end));
        auto group_results = RunOneBatch(group);
        for (auto& r : group_results) out.push_back(std::move(r));
    }
    return out;
}

std::vector<EnrichResult> LlmEnricher::RunOneBatch(
    const std::vector<ChunkContext>& group) {
    const int n = static_cast<int>(group.size());
    // §3.6 cortrix_enricher_batch_size_actual — the actual per-LLM-call batch size
    // (topic 1.2 batching of EnrichBatch); recorded for every dispatched batch.
    EnricherMetrics::Instance().ObserveBatchSizeActual(n);
    const std::string model = config_.model;
    const std::string prompt = prompt_template_->Render(group);  // S2.2 / S2.3

    // Per-task state lives in a shared_ptr so the task body owns everything it
    // touches: on a pool-level timeout the worker keeps running the task AFTER
    // RunOneBatch returns, so the task must NOT reference any RunOneBatch local.
    struct Slot {
        // captured-by-value inputs (own copies — no dangling refs on timeout):
        std::shared_ptr<llm::ILlmClient> client;
        std::string prompt;
        std::string model;
        int task_timeout_ms = 30000;
        int max_tokens = 4096;
        // outputs:
        llm::ChatCompletionResponse chat;
        int attempts = 0;
        int64_t duration_ms = 0;
    };

    // ONE pool task == ONE Chat call, bounded by task_timeout_ms at both the
    // transport (call.timeout_ms) and the pool level. The §5.1 retry schedule
    // (transport retries + backoff sleeps + the timeout retry) is driven from the
    // caller thread below — the seconds-level backoff (addendum §3.7 A-part) must
    // not run inside the pool task or it would eat the per-call timeout budget.
    auto submit_one_call = [this, &prompt, &model]() -> std::shared_ptr<Slot> {
        auto slot = std::make_shared<Slot>();
        slot->client = llm_client_;
        slot->prompt = prompt;
        slot->model = model;
        slot->task_timeout_ms = config_.task_timeout_ms;
        slot->max_tokens = config_.max_tokens > 0 ? config_.max_tokens : 4096;
        const int64_t t0 = NowMs();
        // Task captures ONLY the slot (shared_ptr) + t0 by value.
        auto task = [slot, t0]() -> float {
            llm::LlmCallConfig call;
            call.model = slot->model;
            call.timeout_ms = slot->task_timeout_ms;
            call.max_tokens = slot->max_tokens;
            call.response_format = "json_object";
            call.allow_reasoning_content_fallback = false;
            if (ModelSupportsThinkingControl(call.model)) {
                // F03 requires the batch JSON in message.content. DeepSeek
                // thinking mode can spend the response budget in reasoning_content
                // and leave incomplete or non-final structured output.
                call.thinking_type = "disabled";
            }
            slot->chat = slot->client->Chat(slot->prompt, call);
            slot->duration_ms = NowMs() - t0;
            return 1.0f;  // pool is float-typed; payload travels via the slot
        };
        auto [_, completed] = thread_pool_->SubmitWaitFor(task);
        return completed ? slot : nullptr;  // nullptr == pool-level timeout
    };

    // §5.1 retry schedule, caller-thread driven: transport/HTTP-5xx failures are
    // retried up to kEnricherMaxHttpAttempts calls with backoff base × attempt
    // between them; a pool-level timeout is retried exactly once (no backoff);
    // rate-limit + bad-body return immediately for higher-level handling.
    std::shared_ptr<Slot> slot;
    int transport_attempts = 0;
    bool timeout_retried = false;
    while (true) {
        slot = submit_one_call();
        if (slot == nullptr) {
            if (!timeout_retried) {
                timeout_retried = true;  // §5.1 timeout → retry 1 time
                continue;
            }
            break;  // hard timeout after the retry
        }
        ++transport_attempts;
        if (slot->chat.ok()) break;
        const std::string& msg = slot->chat.status.message();
        const bool retryable = msg.rfind(llm::llm_tokens::kHttp, 0) == 0 ||
                               msg.rfind(llm::llm_tokens::kTransport, 0) == 0;
        if (!retryable || transport_attempts >= kEnricherMaxHttpAttempts) break;
        // Seconds-level backoff (addendum §3.7 A-part): base × attempt.
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int64_t>(config_.http_retry_backoff_ms) *
            transport_attempts));
    }
    if (slot) slot->attempts = transport_attempts;

    auto& metrics = EnricherMetrics::Instance();

    // Hard timeout after the retry → CX_ERR_ENRICHER_LLM_TIMEOUT + breaker failure.
    if (slot == nullptr) {
        if (circuit_breaker_) circuit_breaker_->RecordFailure();
        metrics.RecordFailedTask(EnricherErrorCode::kLlmTimeout);
        return BatchError(n, EnricherErrorCode::kLlmTimeout, model,
                          "task exceeded task_timeout_ms", /*retry_count=*/1);
    }

    const llm::ChatCompletionResponse& chat = slot->chat;
    const int duration_ms = static_cast<int>(slot->duration_ms);
    // §3.6 cortrix_enricher_score_duration_seconds — the completed LLM-call latency,
    // recorded for both the failure-after-retries and success paths (the hard-timeout
    // path above has no measured call latency, so it is excluded). slot->duration_ms is
    // wall-clock ms → seconds.
    metrics.ObserveScoreDuration(static_cast<double>(slot->duration_ms) / 1000.0);

    // LLM call failed (after HTTP retries) → batch error + breaker failure.
    if (!chat.ok()) {
        EnricherErrorCode code = ClassifyLlmFailure(chat.status.message());
        if (circuit_breaker_) circuit_breaker_->RecordFailure();
        metrics.RecordFailedTask(code);
        auto results = BatchError(n, code, model, chat.status.message(),
                                  /*retry_count=*/slot->attempts);
        for (auto& r : results) r.duration_ms = duration_ms;
        return results;
    }

    // Success at the transport level → reset breaker; record tokens + cost.
    if (circuit_breaker_) circuit_breaker_->RecordSuccess();
    const int token_count = chat.prompt_tokens + chat.completion_tokens;
    if (token_count > 0) metrics.RecordTokens(model, token_count);
    // S4.1: accrue spend against the global cap (topic 3.5) + cost metric.
    if (budget_tracker_) {
        budget_tracker_->RecordUsage(model, chat.prompt_tokens, chat.completion_tokens);
    }
    {
        ModelPricing pricing;
        metrics.RecordCostMicroUsd(
            model, pricing.CostMicroUsd(model, chat.prompt_tokens, chat.completion_tokens));
    }

    // topic 3.3 L1/L2/L3 parse (S2.4). NOTE (§4.2): an L3 whole-batch parse failure
    // is NOT counted toward the breaker (the transport succeeded) — we record the
    // PARSE failure metric only.
    auto results = ParseEnrichBatchResponse(chat.content, n, model,
                                            prompt_template_->id());
    std::vector<size_t> parse_fail_indices;
    const int64_t now = NowMs();
    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        StampResultDefaults(r, duration_ms, token_count, now, model,
                            prompt_template_->id());
        if (r.status == static_cast<int>(EnricherErrorCode::kParse)) {
            parse_fail_indices.push_back(i);
            const int chunk_index =
                i < group.size() ? group[i].chunk_index : static_cast<int>(i);
            LogParseFailureSample(r, n, i, chunk_index, model, chat,
                                  prompt_template_->id(), "before_single_retry");
        }
    }

    std::vector<bool> parse_failure_counted_by_retry(results.size(), false);
    if (!parse_fail_indices.empty() && n > 1) {
        CORTRIX_LOG_WARN(
            "spc",
            "F03 LlmEnricher retrying parse failures individually: "
            "parse_failures={} batch_size={} model={} content_source={} "
            "finish_reason={} content_length={} reasoning_content_length={} "
            "prompt_version={}",
            parse_fail_indices.size(), n, model, chat.content_source,
            chat.finish_reason, chat.content_length, chat.reasoning_content_length,
            prompt_template_->id());
        for (const size_t failed_index : parse_fail_indices) {
            if (failed_index >= group.size()) continue;
            auto retry_results = RunOneBatch({group[failed_index]});
            if (retry_results.size() == 1) {
                parse_failure_counted_by_retry[failed_index] =
                    retry_results[0].status ==
                    static_cast<int>(EnricherErrorCode::kParse);
                results[failed_index] = std::move(retry_results[0]);
            }
        }
    }

    int final_parse_fail_count = 0;
    int uncounted_final_parse_fail_count = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        if (r.status == static_cast<int>(EnricherErrorCode::kParse)) {
            ++final_parse_fail_count;
            if (!parse_failure_counted_by_retry[i]) {
                ++uncounted_final_parse_fail_count;
                const int chunk_index =
                    i < group.size() ? group[i].chunk_index : static_cast<int>(i);
                LogParseFailureSample(r, n, i, chunk_index, model, chat,
                                      prompt_template_->id(), "final");
            }
        }
    }
    if (uncounted_final_parse_fail_count > 0) {
        metrics.RecordFailedTask(EnricherErrorCode::kParse);
    }
    if (final_parse_fail_count > 0) {
        CORTRIX_LOG_WARN(
            "spc",
            "F03 LlmEnricher parse failure batch: parse_failures={} batch_size={} "
            "uncounted_parse_failures={} model={} content_source={} "
            "finish_reason={} content_length={} reasoning_content_length={} "
            "prompt_version={}",
            final_parse_fail_count, n, uncounted_final_parse_fail_count, model,
            chat.content_source, chat.finish_reason, chat.content_length, chat.reasoning_content_length,
            prompt_template_->id());
    }
    return results;
}

}  // namespace cortrix::spc
