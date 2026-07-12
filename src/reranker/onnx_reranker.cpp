#include <cstdint>
#include "cortrix/reranker/onnx_reranker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <utility>

#include <spdlog/spdlog.h>

#include "cortrix/ml/ort_env_singleton.h"
#include "cortrix/ml/execution_provider.h"
#include "cortrix/ml/tokenizer_registry.h"
#include "cortrix/reranker/input_preprocessor.h"
#include "cortrix/reranker/reranker_error.h"
#include "cortrix/reranker/reranker_metrics.h"
#include "cortrix/reranker/reranker_status.h"
#include "cortrix/reranker/score_fusion.h"

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef CORTRIX_HAS_COREML
#include <coreml_provider_factory.h>
#endif
#endif

namespace fs = std::filesystem;

namespace cortrix::reranker {

namespace {
constexpr const char* kTokenizerKey = "bge-m3";  // shared with F40/F03 (§2.4-bis)

#ifdef CORTRIX_HAS_ONNXRUNTIME
Status AppendExecutionProvider(Ort::SessionOptions* options,
                               ml::ExecutionProvider provider) {
    OrtStatus* status = nullptr;
    switch (provider) {
        case ml::ExecutionProvider::kCpu:
            return Status::Ok();
        case ml::ExecutionProvider::kCoreMl:
#ifdef CORTRIX_HAS_COREML
            status = OrtSessionOptionsAppendExecutionProvider_CoreML(
                *options, COREML_FLAG_ENABLE_ON_SUBGRAPH);
            break;
#else
            return Status::Unavailable("CoreML execution provider is not compiled in");
#endif
        case ml::ExecutionProvider::kCuda:
#ifdef CORTRIX_HAS_CUDA
            {
                OrtCUDAProviderOptions cuda_options{};
                cuda_options.device_id = 0;
                status = Ort::GetApi().SessionOptionsAppendExecutionProvider_CUDA(
                    *options, &cuda_options);
            }
            break;
#else
            return Status::Unavailable("CUDA execution provider is not compiled in");
#endif
        case ml::ExecutionProvider::kAuto:
            return Status::InvalidArgument("auto must be resolved before session creation");
    }

    if (status == nullptr) return Status::Ok();
    const char* message = Ort::GetApi().GetErrorMessage(status);
    const std::string detail = message == nullptr ? "unknown ONNX Runtime error" : message;
    Ort::GetApi().ReleaseStatus(status);
    return Status::Unavailable(std::string(ml::ExecutionProviderName(provider)) +
                               " execution provider initialization failed: " + detail);
}

/// Cross-encoder pair encoding (F02 §2.2, bge-reranker-v2-m3 / XLM-R):
///   <s> query </s> </s> passage </s>
/// Built by splicing two single-text Encode() results (each comes back
/// <s>...</s> + padding): strip padding via the attention mask, join with the
/// double-</s> separator, drop the passage's leading <s>. No padding in the
/// output — batch-of-1 with a dynamic seq axis.
HfTokenizer::Encoded EncodePair(const HfTokenizer& tok, const std::string& query,
                                const std::string& passage, int max_len) {
    HfTokenizer::Encoded eq, ep;
    if (!tok.Encode(query, max_len, &eq).ok() || !tok.Encode(passage, max_len, &ep).ok()) {
        return {};
    }
    auto strip_padding = [](HfTokenizer::Encoded& e) {
        size_t n = 0;
        while (n < e.attention_mask.size() && e.attention_mask[n] == 1) n++;
        e.input_ids.resize(n);
        e.attention_mask.resize(n);
    };
    strip_padding(eq);
    strip_padding(ep);
    if (eq.input_ids.empty() || ep.input_ids.empty()) return {};

    // Encode() ends every sequence with </s>; reuse it as the separator id
    // instead of hard-coding the vocab constant.
    const int64_t sep = eq.input_ids.back();

    HfTokenizer::Encoded out;
    out.input_ids = eq.input_ids;                       // <s> query </s>
    out.input_ids.push_back(sep);                       // </s>
    out.input_ids.insert(out.input_ids.end(),           // passage </s> (skip <s>)
                         ep.input_ids.begin() + 1, ep.input_ids.end());
    if (static_cast<int>(out.input_ids.size()) > max_len) {
        out.input_ids.resize(max_len);
        out.input_ids.back() = sep;                     // keep trailing </s>
    }
    out.attention_mask.assign(out.input_ids.size(), 1);
    return out;
}

/// Single-pair cross-encoder inference: logits[0] → sigmoid → [0,1] relevance
/// (score_fusion.h contract: rerank_score ∈ [0,1]). MAY throw Ort::Exception —
/// ScoreBatch's per-task guard owns the catch (header ScoreForTask contract).
float RunCrossEncoder(Ort::Session* session, const HfTokenizer::Encoded& enc) {
    Ort::MemoryInfo mem_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t seq_len = static_cast<int64_t>(enc.input_ids.size());
    const int64_t shape[2] = {1, seq_len};

    auto input_ids_tensor = Ort::Value::CreateTensor<int64_t>(
        mem_info, const_cast<int64_t*>(enc.input_ids.data()), enc.input_ids.size(),
        shape, 2);
    auto attention_mask_tensor = Ort::Value::CreateTensor<int64_t>(
        mem_info, const_cast<int64_t*>(enc.attention_mask.data()),
        enc.attention_mask.size(), shape, 2);

    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name_0 = session->GetInputNameAllocated(0, allocator);
    auto input_name_1 = session->GetInputNameAllocated(1, allocator);
    auto output_name_0 = session->GetOutputNameAllocated(0, allocator);
    const char* input_names[] = {input_name_0.get(), input_name_1.get()};
    const char* output_names[] = {output_name_0.get()};

    std::vector<Ort::Value> input_tensors;
    input_tensors.push_back(std::move(input_ids_tensor));
    input_tensors.push_back(std::move(attention_mask_tensor));

    auto output_tensors =
        session->Run(Ort::RunOptions{nullptr}, input_names, input_tensors.data(),
                     input_tensors.size(), output_names, 1);
    const float logit = output_tensors[0].GetTensorData<float>()[0];
    return 1.0f / (1.0f + std::exp(-logit));
}
#endif  // CORTRIX_HAS_ONNXRUNTIME
}  // namespace

OnnxReranker::OnnxReranker(const RerankerConfig& config, store::ChunkStore* chunk_store)
    : config_(config), chunk_store_(chunk_store) {}

OnnxReranker::~OnnxReranker() {
    // Join pool workers FIRST: a timed-out ScoreBatch task is still running on
    // its worker (§3.3) and may be inside ScoreForTask using session_. The dtor
    // body runs before member destruction, so without this the session below is
    // deleted out from under an in-flight inference (TSAN-caught UAF).
    Shutdown();
#ifdef CORTRIX_HAS_ONNXRUNTIME
    // session_ is owned; env_ is borrowed from OrtEnvSingleton (never deleted).
    delete static_cast<Ort::Session*>(session_);
    session_ = nullptr;
#endif
}

void OnnxReranker::Shutdown() {
    thread_pool_.reset();  // RerankerThreadPool dtor joins all workers
}

Status OnnxReranker::Init() {
    ml::ExecutionProvider configured_provider;
    Status provider_status =
        ml::ParseExecutionProvider(config_.execution_provider, &configured_provider);
    if (!provider_status.ok()) {
        return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                              provider_status.message());
    }

    if (config_.use_coreml != RerankerConfig::UseCoreML::kAuto) {
        const ml::ExecutionProvider legacy_provider =
            config_.use_coreml == RerankerConfig::UseCoreML::kForceTrue
                ? ml::ExecutionProvider::kCoreMl
                : ml::ExecutionProvider::kCpu;
        if (configured_provider != ml::ExecutionProvider::kAuto &&
            configured_provider != legacy_provider) {
            return RerankerStatus(
                RerankerErrorCode::kRerankerInitFailed,
                "execution_provider conflicts with deprecated use_coreml");
        }
        if (configured_provider == ml::ExecutionProvider::kAuto) {
            configured_provider = legacy_provider;
        }
    }

    provider_status = ml::ValidateExecutionProviderForBuild(configured_provider);
    if (!provider_status.ok()) {
        return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                              provider_status.message());
    }
    config_.execution_provider = ml::ExecutionProviderName(configured_provider);
    active_ep_ = "stub";
    ready_ = false;
    RerankerMetrics::Instance().SetConfiguredEp(config_.execution_provider);
    RerankerMetrics::Instance().SetActiveEp(active_ep_);

    // Build the resident thread pool (S2.1) + circuit breaker (S2.4) regardless of
    // model vs stub mode — ScoreBatch routes work through them in both. The
    // breaker drives the status/metric/log via the S2.5 reporter on each
    // transition. (Config ranges were validated by RerankerGuc upstream.)
    thread_pool_ = std::make_unique<RerankerThreadPool<RerankTaskResult>>(
        config_.workers, config_.queue_size, config_.task_timeout_ms);
    if (config_.circuit_breaker_enabled) {
        circuit_breaker_ = std::make_unique<CircuitBreaker>(
            config_.circuit_breaker_threshold, config_.circuit_breaker_cooldown_sec);
        const int cooldown = config_.circuit_breaker_cooldown_sec;
        circuit_breaker_->OnStateChange([cooldown](CircuitBreakerState s) {
            RerankerStatusReporter::OnCircuitStateChange(s, cooldown);
        });
    }

    // Fail-fast contract (F02 §3.1 / §4.1, S1.3): if a model is *configured*
    // (model_path non-empty) it MUST load, else startup fails with
    // CX_ERR_RERANKER_INIT_FAILED. An EMPTY model_path = no reranker model
    // configured → stub mode (offline/dev; mirrors the in-tree OnnxEmbedder so a
    // box without model files still starts). This is the standalone-testable
    // equivalent of the production "model is mandatory" rule.
    if (config_.model_path.empty()) {
        stub_mode_ = true;
        ready_ = true;
        RerankerMetrics::Instance().SetActiveEp(active_ep_);
        spdlog::info("Reranker: no model configured, initialized in stub mode");
        return Status::Ok();
    }

#ifdef CORTRIX_HAS_ONNXRUNTIME
    // Model configured: it must exist on disk.
    if (!fs::exists(config_.model_path)) {
        spdlog::error("Reranker: model path does not exist: {}", config_.model_path);
        return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                              "model path not found: " + config_.model_path);
    }

    // --- Shared tokenizer (TokenizerRegistry, §2.4-bis) ---
    std::string tok_path = config_.tokenizer_path;
    if (tok_path.empty()) {
        tok_path = (fs::path(config_.model_path).parent_path() / "tokenizer.json").string();
    }
    Status tok_status = ml::TokenizerRegistry::LoadAndRegister(kTokenizerKey, tok_path);
    if (!tok_status.ok()) {
        spdlog::error("Reranker: tokenizer load failed ({}): {}", tok_path, tok_status.message());
        return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                              "tokenizer load failed: " + tok_status.message());
    }
    tokenizer_ = ml::TokenizerRegistry::Get(kTokenizerKey);

    // --- Shared Ort::Env (OrtEnvSingleton, §2.4-bis) ---
    ml::OrtEnvSingleton::Init();
    env_ = ml::OrtEnvSingleton::EnvHandle();
    if (env_ == nullptr) {
        spdlog::error("Reranker: ONNX environment unavailable");
        return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                              "ONNX environment unavailable");
    }
    auto* env = static_cast<Ort::Env*>(env_);

    try {
        auto create_session = [&](ml::ExecutionProvider provider,
                                  std::unique_ptr<Ort::Session>* session,
                                  RerankerMetrics::ProviderFallbackReason* reason) {
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (config_.intra_threads > 0) {
                options.SetIntraOpNumThreads(config_.intra_threads);
            }

            Status append_status = AppendExecutionProvider(&options, provider);
            if (!append_status.ok()) {
                *reason = RerankerMetrics::ProviderFallbackReason::kEpInitFailed;
                return append_status;
            }

            try {
                *session = std::make_unique<Ort::Session>(
                    *env, config_.model_path.c_str(), options);
                return Status::Ok();
            } catch (const Ort::Exception& e) {
                *reason = RerankerMetrics::ProviderFallbackReason::kSessionInitFailed;
                return Status::Unavailable(std::string("ONNX session creation failed: ") +
                                           e.what());
            }
        };

        const bool allow_cpu_fallback =
            configured_provider == ml::ExecutionProvider::kAuto;
        const ml::ExecutionProvider preferred_provider = allow_cpu_fallback
            ? ml::PreferredAutoExecutionProvider()
            : configured_provider;
        ml::ExecutionProvider active_provider = preferred_provider;
        std::unique_ptr<Ort::Session> session;
        RerankerMetrics::ProviderFallbackReason fallback_reason =
            RerankerMetrics::ProviderFallbackReason::kSessionInitFailed;
        Status session_status =
            create_session(preferred_provider, &session, &fallback_reason);

        if (!session_status.ok()) {
            RerankerMetrics::Instance().RecordProviderInitFailure(
                ml::ExecutionProviderName(preferred_provider), fallback_reason);
        }

        if (!session_status.ok() && allow_cpu_fallback &&
            preferred_provider != ml::ExecutionProvider::kCpu) {
            const std::string from = ml::ExecutionProviderName(preferred_provider);
            RerankerMetrics::Instance().RecordProviderFallback(from, fallback_reason);
            if (preferred_provider == ml::ExecutionProvider::kCoreMl &&
                fallback_reason == RerankerMetrics::ProviderFallbackReason::kEpInitFailed) {
                RerankerMetrics::Instance().RecordCoremlFallback(
                    RerankerMetrics::CoremlFallbackReason::kEpInitFailed);
            }
            spdlog::warn("Reranker: {} EP unavailable ({}); auto falling back to CPU",
                         from, session_status.message());
            active_provider = ml::ExecutionProvider::kCpu;
            session_status = create_session(active_provider, &session, &fallback_reason);
            if (!session_status.ok()) {
                RerankerMetrics::Instance().RecordProviderInitFailure(
                    ml::ExecutionProviderName(active_provider), fallback_reason);
            }
        }
        if (!session_status.ok()) {
            return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                                  session_status.message());
        }

        session_ = session.release();
        stub_mode_ = false;
        active_ep_ = ml::ExecutionProviderName(active_provider);
        ready_ = true;
        RerankerMetrics::Instance().SetActiveEp(active_ep_);
        spdlog::info("Reranker: ONNX session created (model={}, ep={})",
                     config_.model_path, active_ep_);
        return Status::Ok();
    } catch (const Ort::Exception& e) {
        spdlog::error("Reranker: ONNX session creation failed: {}", e.what());
        return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                              std::string("ONNX session creation failed: ") + e.what());
    }
#else
    // Built without ONNX Runtime but a model was configured → cannot honor it.
    spdlog::error("Reranker: model configured ({}) but binary built without ONNX Runtime",
                  config_.model_path);
    return RerankerStatus(RerankerErrorCode::kRerankerInitFailed,
                          "binary built without ONNX Runtime");
#endif
}

float OnnxReranker::StubScore(const char* query, const char* passage) const {
    // Deterministic, offline relevance proxy: cosine-ish overlap of byte hashes.
    // Keeps Score/ScoreBatch/Rerank testable without a model; replaced by real
    // ONNX inference in later Waves.
    auto h = [](const char* s) -> uint32_t {
        uint32_t hash = 2166136261u;
        for (const char* p = s; p && *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
        return hash;
    };
    uint32_t hq = h(query ? query : "");
    uint32_t hp = h(passage ? passage : "");
    uint32_t mix = hq ^ (hp * 2654435761u);
    return static_cast<float>(mix & 0xFFFF) / 65535.0f;  // [0,1]
}

float OnnxReranker::Score(const char* query, const char* passage) {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    // Real cross-encoder path (F02 §2.2): pair-encode → session Run → sigmoid.
    // MAY throw on ONNX/OOM failure — ScoreBatch's per-task guard owns the
    // catch (ScoreForTask contract); direct callers get fail-loud semantics.
    if (!stub_mode_ && session_ != nullptr && tokenizer_ && tokenizer_->loaded()) {
        HfTokenizer::Encoded enc =
            EncodePair(*tokenizer_, query ? query : "", passage ? passage : "",
                       config_.max_seq_length);
        if (enc.input_ids.empty()) {
            spdlog::warn("Reranker: pair encoding failed, scoring 0");
            return 0.0f;
        }
        return RunCrossEncoder(static_cast<Ort::Session*>(session_), enc);
    }
#endif
    return StubScore(query, passage);
}

std::vector<float> OnnxReranker::ScoreBatch(const char* query,
                                            const std::vector<const char*>& passages) {
    const std::string q = query ? query : "";
    const size_t n = passages.size();

    // D35-MET-04: time the whole ScoreBatch (F02 §3.4 score_duration_seconds) and
    // sample the live pool queue depth (queue_depth_current gauge). The duration is
    // observed at every return path via this scope guard.
    const auto score_start = std::chrono::steady_clock::now();
    struct ScoreDurationGuard {
        std::chrono::steady_clock::time_point start;
        ~ScoreDurationGuard() {
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            RerankerMetrics::Instance().ObserveScoreDuration(secs);
        }
    } score_guard{score_start};
    if (thread_pool_) {
        RerankerMetrics::Instance().SetQueueDepth(thread_pool_->QueueDepth());
    }

    // §4.2 step 1 — circuit-breaker gate. OPEN → skip the reranker entirely and
    // return an all-zero score vector so the upper layer falls back to the RRF
    // ordering (rerank_score=NULL semantics). half_open/closed → proceed (the
    // single half-open probe is this batch's first scored task).
    if (circuit_breaker_ && !circuit_breaker_->AllowRequest()) {
        return std::vector<float>(n, 0.0f);
    }

    std::vector<float> scores(n, 0.0f);

    // §4.2 inner loop: per passage apply the §3.3 three-segment policy, then run
    // the guarded inference task on the pool. A NORMAL/TRUNCATED passage is a real
    // inference (counts toward the breaker); an EXTREMELY_LONG passage is forced
    // to 0 as a data-quality fallback (does NOT count as a task failure, §5.1).
    for (size_t i = 0; i < n; ++i) {
        const std::string passage = passages[i] ? passages[i] : "";
        PreprocessedPassage pre =
            InputPreprocessor::Prepare(q, passage, config_.max_seq_length);
        if (pre.force_zero) {
            scores[i] = 0.0f;  // EXTREMELY_LONG; metric already recorded by Prepare
            continue;
        }

        // Guarded task: never throws out of the worker. On an ONNX exception /
        // OOM it returns failed=true + reason + score=0 (S3.4). The outcome is
        // returned BY VALUE (RerankTaskResult) — never written back through a
        // captured reference. This is the R02-C2 fix: a task that times out keeps
        // running on its worker, so writing into the per-iteration stack locals
        // (the old `&task_failed` / `&reason` capture) was a use-after-free once
        // the iteration's stack slot was destroyed / reused, plus a data race
        // across the 4 workers. Returning by value confines the outcome to this
        // task's own future, which the caller discards on timeout.
        // Capture BOTH query and passage BY VALUE. A timed-out task keeps running
        // on its worker after ScoreBatch returns and `q` (a ScoreBatch stack local)
        // is destroyed, so a `&q` capture would be a use-after-free on the abandoned
        // task — the read-side companion to the R02-C2 by-value fix above.
        const std::string passage_text = pre.text;
        auto task = [this, q, passage_text]() -> RerankTaskResult {
            try {
                return {this->ScoreForTask(q.c_str(), passage_text.c_str()),
                        false, {}};
            } catch (const std::bad_alloc&) {
                return {0.0f, true, RerankerMetrics::FailedTaskReason::kOom};
            } catch (...) {
                // ONNX exception / any internal error → score=0 (candidate ranked
                // last). The rich identity is observed via the metric, not thrown
                // to the query user (§5.1).
                return {0.0f, true, RerankerMetrics::FailedTaskReason::kOnnxException};
            }
        };

        bool failed = false;
        if (thread_pool_) {
            // Refresh the gauge with the live depth around each submit (D35-MET-04).
            RerankerMetrics::Instance().SetQueueDepth(thread_pool_->QueueDepth());
            std::pair<RerankTaskResult, bool> r = thread_pool_->SubmitWaitFor(task);
            if (!r.second) {
                // §1.2 / §5.1: per-task timeout → score=0 + timeout metric +
                // counts toward the breaker. The abandoned task's RerankTaskResult
                // (whatever it eventually returns) stays inside the dropped future.
                scores[i] = 0.0f;
                RerankerMetrics::Instance().RecordFailedTask(
                    RerankerMetrics::FailedTaskReason::kTimeout);
                failed = true;
            } else {
                scores[i] = r.first.score;
                if (r.first.failed) {
                    RerankerMetrics::Instance().RecordFailedTask(r.first.reason);
                    failed = true;
                }
            }
        } else {
            // No pool (defensive): run inline with the same guard.
            RerankTaskResult r = task();
            scores[i] = r.score;
            if (r.failed) {
                RerankerMetrics::Instance().RecordFailedTask(r.reason);
                failed = true;
            }
        }

        // §1.3 / §3.4 breaker accounting, in passage order (models "consecutive
        // task failures"): a failure advances the trip counter, a success resets
        // it / closes a half-open probe.
        if (circuit_breaker_) {
            if (failed) {
                circuit_breaker_->RecordFailure();
            } else {
                circuit_breaker_->RecordSuccess();
            }
        }
    }
    return scores;
}

std::vector<retrieval::RankedChunk> OnnxReranker::Rerank(
    const std::vector<retrieval::ScoredResult>& candidates,
    const std::string& query) {
    std::vector<retrieval::RankedChunk> result;
    if (candidates.empty()) return result;

    // 1. Reverse-look-up chunk text (ChunkStore.GetBatch). Standalone uses a mock;
    //    real F08+F09 assembly = D3.5.
    std::vector<std::string> child_ids;
    child_ids.reserve(candidates.size());
    for (const auto& c : candidates) child_ids.push_back(c.child_id);

    std::vector<store::ChunkRecord> records;
    if (chunk_store_) {
        std::vector<std::string> missing;
        records = chunk_store_->GetBatch(child_ids, &missing);
        if (!missing.empty()) {
            spdlog::warn("Reranker: {} chunk(s) missing during Rerank", missing.size());
        }
    }

    // Index records by child_id for assembly (GetBatch skips missing ids).
    auto find_record = [&records](const std::string& cid) -> const store::ChunkRecord* {
        for (const auto& r : records) {
            if (r.child_id == cid) return &r;
        }
        return nullptr;
    };

    // 2. Assemble RankedChunk + collect passages for scoring.
    std::vector<std::string> passage_storage;
    passage_storage.reserve(candidates.size());
    for (const auto& c : candidates) {
        retrieval::RankedChunk rc;
        rc.child_id = c.child_id;
        rc.score = c.score;  // pre-rerank (RRF) score
        if (const store::ChunkRecord* rec = find_record(c.child_id)) {
            rc.chunk_text = rec->content;
            rc.score_signals = rec->score_signals;
        }
        passage_storage.push_back(rc.chunk_text);
        result.push_back(std::move(rc));
    }

    // 3. ScoreBatch → rerank_score.
    std::vector<const char*> passages;
    passages.reserve(passage_storage.size());
    for (const auto& s : passage_storage) passages.push_back(s.c_str());
    std::vector<float> rerank_scores = ScoreBatch(query.c_str(), passages);
    for (size_t i = 0; i < result.size() && i < rerank_scores.size(); ++i) {
        result[i].rerank_score = rerank_scores[i];
    }

    // 4. F02-owned RRF fusion (§4.2-ter): base ordering score =
    //    rerank_score*0.7 + rrf_score*0.3 (rrf_score = the candidate's RRF score,
    //    carried as RankedChunk.score), then optional F03/F07 semantic multiplier.
    //    Write the final score back so downstream query surfaces use one score
    //    contract.
    const RerankerScoreFusion fusion;
    std::vector<std::pair<float, size_t>> order;  // (fused_score, original index)
    order.reserve(result.size());
    for (size_t i = 0; i < result.size(); ++i) {
        const float fused = fusion.ComputeRerankRrfScore(
            result[i].rerank_score, result[i].score, result[i], query);
        result[i].score = fused;
        order.emplace_back(fused, i);
    }

    // 6. Sort by fused ordering score desc. Stable on ties (preserve input order)
    //    via the original index as the tiebreaker.
    std::stable_sort(order.begin(), order.end(),
                     [](const std::pair<float, size_t>& a, const std::pair<float, size_t>& b) {
                         return a.first > b.first;
                     });

    std::vector<retrieval::RankedChunk> sorted;
    sorted.reserve(result.size());
    for (const auto& [fused, idx] : order) {
        (void)fused;
        sorted.push_back(std::move(result[idx]));
    }
    return sorted;
}

}  // namespace cortrix::reranker
