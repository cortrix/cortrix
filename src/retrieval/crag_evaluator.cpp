#include "cortrix/retrieval/crag_evaluator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

#include "cortrix/query/query_context.h"     // full QueryContext (EvaluateAndUpdateContext)
#include "cortrix/retrieval/crag_error.h"    // CragInferenceError
#include "cortrix/retrieval/crag_metrics.h"  // CragMetrics

namespace cortrix::retrieval {

namespace {

// --- §6.1 score-distribution helpers (no existing util in the frozen tree) ---

float ComputeMedian(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

float ComputeMean(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    float sum = 0.0f;
    for (float x : v) sum += x;
    return sum / static_cast<float>(v.size());
}

float ComputeStd(const std::vector<float>& v) {
    if (v.size() < 2) return 0.0f;  // population std is 0 for a single point
    const float mean = ComputeMean(v);
    float acc = 0.0f;
    for (float x : v) {
        const float d = x - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<float>(v.size()));
}

// L3 retry back-off (§7.3): 50 / 100 / 200 ms exponential. Index clamps to the cap.
int BackoffMs(int retry_index) {
    static const int kSchedule[] = {50, 100, 200};
    const int n = static_cast<int>(std::size(kSchedule));
    if (retry_index < 0) retry_index = 0;
    if (retry_index >= n) retry_index = n - 1;
    return kSchedule[retry_index];
}

}  // namespace

CragEvaluator::CragEvaluator(std::shared_ptr<ICragClassifierBackend> backend,
                             const CragConfig& config)
    : backend_(std::move(backend)),
      config_(config),
      // Reuse the reranker's circuit breaker: trip after 3 consecutive backend
      // failures, 30s cooldown. Mirrors the reranker default profile.
      breaker_(/*threshold=*/3, /*cooldown_sec=*/30) {}

bool CragEvaluator::IsAvailable() const {
    return backend_ != nullptr && backend_->IsAvailable();
}

std::map<std::string, float> CragEvaluator::ComputeMultiSignals(
    const std::vector<RankedChunk>& chunks) const {
    std::map<std::string, float> signals;

    if (chunks.empty()) {
        signals["top1"] = 0.0f;
        signals["median"] = 0.0f;
        signals["std"] = 0.0f;
        signals["high_score_ratio"] = 0.0f;
        signals["chunk_count"] = 0.0f;
        return signals;
    }

    // The score distribution drives the multi-signal features. CRAG evaluates
    // on the reranker output; per RETRIEVAL_TYPES_SPEC the pre-rerank `score`
    // field is the RRF score and `rerank_score` the cross-encoder score. §6.1's
    // reference code reads chunk.score, so we mirror that exactly.
    std::vector<float> scores;
    scores.reserve(chunks.size());
    for (const auto& chunk : chunks) scores.push_back(chunk.score);

    signals["top1"] = scores[0];
    signals["median"] = ComputeMedian(scores);
    signals["std"] = ComputeStd(scores);

    const int high_score_count = static_cast<int>(
        std::count_if(scores.begin(), scores.end(), [](float s) { return s >= 0.5f; }));
    signals["high_score_ratio"] =
        static_cast<float>(high_score_count) / static_cast<float>(scores.size());

    // top1-top5 gap (§6.1 — CRAG paper style; only when >=5 candidates).
    if (scores.size() >= 5) {
        signals["top1_top5_gap"] = scores[0] - scores[4];
    }

    signals["chunk_count"] = static_cast<float>(scores.size());
    return signals;
}

std::string CragEvaluator::LabelFromScore(float score) const {
    // §6.2 three-tier mapping using the resolved thresholds.
    if (score >= config_.threshold_correct) return "correct";
    if (score < config_.threshold_incorrect) return "incorrect";
    return "ambiguous";
}

ClassificationResult CragEvaluator::HeuristicGuard(
    const std::map<std::string, float>& signals) const {
    // §6.2 step 2 / §7.3: a no-ONNX verdict from the score distribution. The score
    // blends top1 (dominant) with the high-score ratio so a single strong hit and
    // broad agreement both push toward "correct", while weak/sparse hits fall to
    // "incorrect". This is the standalone classifier and the safety net when the
    // backend is unavailable or low-confidence.
    auto get = [&signals](const char* k) -> float {
        auto it = signals.find(k);
        return it == signals.end() ? 0.0f : it->second;
    };
    const float top1 = get("top1");
    const float high_ratio = get("high_score_ratio");
    const float score = std::clamp(0.7f * top1 + 0.3f * high_ratio, 0.0f, 1.0f);

    ClassificationResult r;
    r.label = LabelFromScore(score);
    r.score = score;
    // Heuristic confidence is intentionally moderate (it is a guard, not the model).
    r.confidence = 0.5f;
    r.raw_signals = signals;
    return r;
}

ClassificationResult CragEvaluator::RunClassifier(
    const std::string& query,
    const std::string& chunk_text,
    const std::map<std::string, float>& signals) {
    // §7.3 L3: retry the backend on transient failure, then transparently degrade.
    // The circuit breaker is a SYSTEMIC gate (shared reranker component): it is checked
    // ONCE at entry so a query does not even start the retry loop against a backend
    // that recent queries proved dead. Inside one query, the full
    // max_inference_retries budget is honored (the breaker's per-query failures
    // are recorded but do not abort this query's own retries — that would conflate
    // the N=3 per-query retry policy with the cross-query breaker threshold).
    if (!breaker_.AllowRequest()) {
        return DegradedResult(signals);  // breaker open → skip backend, degrade
    }

    const int max_retries = std::max(0, config_.max_inference_retries);
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        try {
            CragBackendResult br = backend_->Infer(query, chunk_text, signals);
            breaker_.RecordSuccess();

            ClassificationResult r;
            // Backend may emit a label directly; otherwise derive from score via
            // the configured thresholds. We always re-derive from score so the NS
            // threshold override governs the final verdict.
            r.score = std::clamp(br.score, 0.0f, 1.0f);
            r.label = br.label.empty() ? LabelFromScore(r.score) : br.label;
            r.confidence = br.confidence;
            r.raw_signals = signals;
            return r;
        } catch (const CragInferenceError&) {
            breaker_.RecordFailure();
            if (attempt < max_retries) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(BackoffMs(attempt)));
                continue;
            }
            // last attempt failed → degrade below
        }
    }
    return DegradedResult(signals);
}

ClassificationResult CragEvaluator::DegradedResult(
    const std::map<std::string, float>& signals) const {
    // §7.3: transparent degrade. Encode the fallback into the verdict enum (SPEC:
    // "correct_fallback_classifier_failed") and tag the inference error code;
    // signals are preserved for ?explain=true.
    ClassificationResult degraded;
    degraded.label = "correct_fallback_classifier_failed";
    degraded.score = 0.5f;
    degraded.confidence = 0.5f;
    degraded.raw_signals = signals;
    degraded.error_code = CragErrorCodeString(CragErrorCode::kInferenceFailed);
    return degraded;
}

ClassificationResult CragEvaluator::Classify(const ClassifierInput& input) {
    // 1. multi-signal features (§6.1)
    auto signals = ComputeMultiSignals(input.chunks);

    // 2. heuristic guard for degenerate input (§6.2 step 2) or no backend (L1/L2).
    if (input.query.length() < 3 || input.chunks.empty() || !IsAvailable()) {
        return HeuristicGuard(signals);
    }

    // 3. main path: backend inference with L3 retry/degrade (§6.2 step 3 / §7.3).
    ClassificationResult result =
        RunClassifier(input.query, input.chunks[0].chunk_text, signals);

    // A degraded verdict already encodes its own fallback label — return as-is.
    if (result.error_code.has_value()) return result;

    // 4. low-confidence guard → heuristic (§6.2).
    if (result.confidence < config_.classifier_min_confidence) {
        return HeuristicGuard(signals);
    }
    return result;
}

namespace {

// Map a verdict label to the §10 metric decision bucket. The transparent-degrade
// verdict ("correct_fallback_classifier_failed") and any unknown label fold into
// `fallback`.
CragMetrics::Decision DecisionForVerdict(const std::string& verdict) {
    if (verdict == "correct") return CragMetrics::Decision::kCorrect;
    if (verdict == "ambiguous") return CragMetrics::Decision::kAmbiguous;
    if (verdict == "incorrect") return CragMetrics::Decision::kIncorrect;
    return CragMetrics::Decision::kFallback;  // correct_fallback_classifier_failed / unknown
}

}  // namespace

void CragEvaluator::EvaluateAndUpdateContext(
    query::QueryContext& ctx, const std::vector<RankedChunk>& chunks) {
    // Classify and write the CRAG verdict fields onto QueryContext. This does
    // NOT decide skip — the QueryPipeline gates on ShouldSkipF37 *before*
    // calling this. Skipped queries simply never reach here.
    ClassifierInput input{ctx.query, chunks,
                          ctx.ns_id.empty() ? std::optional<std::string>{}
                                            : std::optional<std::string>{ctx.ns_id}};
    ClassificationResult result = Classify(input);

    // Write the CRAG fields (QUERY_CONTEXT_SPEC). The classification helper
    // already wrote QueryContext-independent state; here we only touch CRAG's own
    // fields, never the router's (write-failure isolation).
    ctx.crag_verdict = result.label;
    ctx.crag_score = result.score;
    ctx.f37_signals = result.raw_signals;

    // §10 metric: one evaluation_total increment per verdict (enum label only).
    CragMetrics::Instance().RecordEvaluation(DecisionForVerdict(result.label));

    // --- §6.3 path handling ---
    if (result.label == "correct" ||
        result.label == "correct_fallback_classifier_failed") {
        // Directly return reranker top-K (no extra processing). The
        // fallback verdict also takes the correct path (transparent degrade, §7.3).
        return;
    }

    if (result.label == "ambiguous") {
        // Fine-grained filtering (top-N/2) is done downstream by the
        // Query Engine; CRAG only records the action taken (a later change wires the actual
        // filter into QueryPipeline).
        ctx.ambiguous_action_taken = "filtered_top_n_by_2";
        return;
    }

    if (result.label == "incorrect") {
        // Phase 1 only records (OBS metric above + structured log) and
        // returns the degraded reranker top-K. Phase 2 triggers IRetrievalFallback
        // (WebSearchFallback). web_fallback_triggered stays false in Phase 1.
        ctx.web_fallback_triggered = false;
        return;
    }
}

}  // namespace cortrix::retrieval
