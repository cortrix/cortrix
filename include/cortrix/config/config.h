#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "cortrix/auth/api_key_auth.h"

namespace cortrix {

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8420;
    int thread_count = 8;
    int64_t max_payload_bytes = 100 * 1024 * 1024;  // 100MB
};

struct AuthConfig {
    bool enabled = true;
    std::vector<ApiKeyConfig> api_keys;
};

struct LogConfig {
    std::string level = "info";
    std::string format = "json";
    std::string output = "stdout";
};

struct NamespaceConfig {
    std::string data_dir = "./data";
    int max_active = 20;
    int idle_timeout_s = 300;
    // F05 per-NS startup-load timeout guard (§6.3 C1). A large-but-healthy namespace
    // (100MB+ store, big HNSW/sparse indexes) legitimately needs many seconds to load;
    // too small a value mis-classifies it as hung and leaves it un-admitted (invisible)
    // after restart (a 5183-doc NS measured ~42s to load). <=0 means "no timeout, wait
    // fully". See resource::F05Config.
    int64_t load_timeout_ms_per_ns = 60000;
};

struct EmbeddingConfig {
    std::string model_path;           // Path to ONNX model file (e.g., /models/bge-m3/model.onnx)
    std::string tokenizer_path;       // Path to tokenizer.json (derived from model_path dir if empty)
    int dimension = 1024;
    int max_seq_length = 512;         // Maximum input token length
    // Deprecated C++ compatibility field. Config loading normalizes it into
    // execution_provider; new callers should use the canonical field.
    std::string gpu_provider = "auto";
    std::string execution_provider = "auto";  // auto, cpu, coreml, cuda
    std::string execution_provider_error;     // canonical/legacy conflict, if any
};

struct LlmConfig {
    std::string provider;
    std::string api_key;
    std::string model;
    std::string base_url;
    int timeout_ms = 0;          ///< per-call LLM deadline; 0 = use the consumer's default
    int batch_size = 0;          ///< enricher role only: chunks per LLM call (0 = consumer
                                 ///< default). Large batches push the non-streaming
                                 ///< generation past provider gateway idle windows.
    int max_tokens = 0;          ///< enricher role only: response token budget for the
                                 ///< F03 batch call (0 = built-in default 4096; oversize
                                 ///< values are clamped downstream — bootstrap mapping
                                 ///< and enricher slot — to kEnricherMaxTokensCap, QA
                                 ///< F-7; this raw field itself stays as parsed).
                                 ///< Size it with batch_size — 4096 across a 32-chunk
                                 ///< batch is ~128 tokens/chunk and truncates the batch
                                 ///< JSON (D5b evidence); the budget/batch policy itself
                                 ///< stays a deployment decision until the D5b round.
    int hype_questions_per_chunk = 0;  ///< enricher role only: F38 §4.3 hypothetical
                                       ///< questions per chunk (0 = built-in default 3;
                                       ///< clamped to the design range [1, 10]). The
                                       ///< first-order candidate-pool-coverage lever.
    int ctx_max_output_tokens = 0;     ///< enricher role only: F35 §6.2 context-prefix
                                       ///< token budget (0 = built-in default 80;
                                       ///< clamped to [40, 200]).
    int ctx_guard_chars_per_token = 0; ///< enricher role only: F35 §8 injection-guard
                                       ///< bytes-per-token multiplier (0 = built-in
                                       ///< default 6; clamped to [1, 20]). The old
                                       ///< default 2 rejected legitimate >160-byte
                                       ///< outputs as injection (D12).

    bool IsConfigured() const {
        return !provider.empty() && !api_key.empty() && !model.empty();
    }
};

struct SPCConfig {
    // 0 = auto: resolved after embedder.Init(); current default is 2 for every EP.
    int worker_count = 0;
    // Parser-subprocess concurrency cap (memory protection; the F42 pool-size
    // startup gate compares against it). Default 4 unchanged — raise EXPLICITLY
    // only for workloads that spawn no/few parsers (e.g. enrich backfill).
    int parser_max_concurrent = 4;
    int max_queue_size = 10000;
    int task_timeout_s = 300;
    std::string python_bin = "python3";
    std::string parse_pdf_script;
    std::string parse_word_script;
    int parser_timeout_s = 120;
    int chunk_size = 512;
    int chunk_overlap = 50;
    int embedding_batch_size = 4;  // 4 = good default for both GPU and CPU batching
    // 0 = auto: 2 for an accelerator EP, min(4, cores) for CPU.
    int onnx_intra_threads = 0;
    int onnx_inter_threads = 1;
    std::string ocr_script;
    int ocr_timeout_s = 60;
    float ocr_confidence_threshold = 0.3f;
    bool ocr_enable_cls = true;
    bool enable_crash_recovery = true;
    std::string vision_llm_script;       // Path to run_vision_llm.py (empty = disabled)
    int vision_llm_timeout_s = 120;      // Per-call timeout for vision LLM
    // Refuse to start when no document parser script can be located (default
    // false = legacy WARN-and-boot). Without it a mis-provisioned host (missing
    // CORTRIX_SCRIPTS_DIR) answers health 200 while 100% of ingests fail
    // CX_ERR_PARSE_FAILED — observed as a full 5000-doc wipeout 2026-07-09.
    bool require_parsers = false;
    // F42 enrich-retry sweeper pacing. 0 = built-in defaults (60s tick, 32
    // docs/NS/tick). At 5000-doc scale the defaults mean a multi-hour drain.
    int enrich_sweep_interval_sec = 0;
    int enrich_sweep_batch = 0;
};

struct UploadConfig {
    int64_t max_file_size = 100 * 1024 * 1024;
    int64_t large_file_threshold = 100 * 1024 * 1024;
    std::vector<std::string> allowed_mime_types;
    std::string temp_dir = "/tmp/cortrix_upload";
    int max_concurrent_uploads = 0;
};

struct WatchDirConfig {
    std::string data_dir;
    std::string namespace_name = "default";
    bool watch_enabled = true;
    double fsevents_latency_s = 0.5;
    int scan_batch_size = 100;
    int max_concurrent_imports = 50;
    int error_threshold = 0;  // 0 = no limit; >0 = abort scan after N consecutive errors
    std::vector<std::string> ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};
    std::vector<std::string> allowed_extensions;
};

struct MemoryConfig {
    int default_ttl_seconds = 0;
    int text_to_sql_ttl_seconds = 86400;
    int inject_recent_turns = 5;
    int inject_max_tokens = 2000;
    int max_sessions_per_namespace = 10000;
    int max_interactions_per_session = 1000;
    std::string chunk_strategy = "per_turn";
    // [MEM01] Classified-decay scoring for memory search (design § 2.5).
    // D5 lock: V1.0 is global-GUC only (no per-namespace override). These feed
    // MemoryDecayConfig at the MemorySearcher wiring site (memory_routes.cpp).
    double decay_lambda = 0.01;     // memory.decay.lambda  — decay coefficient (half-life ~70d)
    double decay_min_score = 0.0;   // memory.decay.min_score — decay floor (0 = old events sink)
};

struct RerankerTopConfig {
    std::string model_dir;  ///< dir with model.onnx + tokenizer.json; empty = stub mode
    std::string execution_provider = "auto";  ///< auto, cpu, coreml, cuda
    std::string execution_provider_error;     ///< canonical/legacy conflict, if any
};

struct QueryComplexityTopConfig {
    std::string model_dir = "models/query-complexity";  ///< F39 DistilBERT ONNX dir
};

// === Retrieval candidate-pool sizing (F02 §top_N over-fetch) ===
// candidate_k = min(top_k * candidate_multiplier * oversample, max_candidates).
// Defaults preserve the historical hardcoded 3 / 50. Raising max_candidates
// matters on large corpora where relevant docs spread past rank 50 and the pool
// cap otherwise ceilings recall. Env: CORTRIX_RETRIEVAL_MAX_CANDIDATES /
// CORTRIX_RETRIEVAL_CANDIDATE_MULTIPLIER (env wins over yaml).
struct RetrievalConfig {
    int candidate_multiplier = 3;   ///< top_k multiplier before the max cap (>=1)
    int max_candidates = 50;        ///< hard ceiling on the candidate pool (>=1)
};

// === [OPEN-2] three-stage GC (ARCH §5.x, A6 §10.8) ===
// Config for the built-in background GC thread + manual ops endpoints. Defaults
// mirror the ARCH `gc:` section. CE runs with immediate_purge_enabled=false
// (the GDPR immediate-purge API is a Cloud V1 feature, not built in CE).
struct GcConfig {
    bool enabled = true;                     ///< background GC thread on/off (default on, can disable)
    int soft_delete_retention_days = 30;     ///< Stage 1 → 2 window (soft-delete retention)
    int blob_gc_retention_days = 90;         ///< Stage 2 → 3 window (blob second-confirm wait)
    int scan_interval_hours = 24;            ///< background scan cadence
    int max_purge_per_run = 10000;           ///< cap on blobs unlinked per run (runaway guard)
    int max_run_duration_minutes = 5;        ///< cap on a single run's wall time (IO guard)
    bool dry_run = false;                    ///< scan but do not delete (test/inspection)
    // Compliance path (Cloud V1+; CE default false — not built in CE).
    bool immediate_purge_enabled = false;    ///< GDPR immediate-purge API switch
};

// === [F20] security hardening ===
// Admin-endpoint access policy (design topic 3, plan A). The server binds a
// single socket (server.host:server.port); admin paths are gated at the
// application layer by AdminGuard, not by a separate bind. `admin_bind` is the
// admin client-IP whitelist (default loopback-only), NOT a socket bind address.
// Phase 1 supports the two-value switch only: "127.0.0.1" (loopback) or
// "0.0.0.0" (all client IPs, which additionally requires allow_public_admin).
struct SecurityConfig {
    std::string admin_bind = "127.0.0.1";  ///< CORTRIX_ADMIN_BIND — admin IP whitelist
    bool allow_public_admin = false;       ///< CORTRIX_ALLOW_PUBLIC_ADMIN — V3-E-06 ack
};

struct CortrixConfig {
    ServerConfig server;
    AuthConfig auth;
    LogConfig log;
    NamespaceConfig ns;
    EmbeddingConfig embedding;
    // Three independent LLM roles, each can use a different model/provider:
    LlmConfig semantic_llm;  // Intent classification, reranking (future), query expansion (future)
                              // Should be fast & cheap — called on every query
    LlmConfig vision_llm;    // OCR image enhancement — must be a multimodal model
                              // Inherits semantic_llm's provider/api_key/base_url if not set
    LlmConfig agent_llm;     // Conversational RAG & chat generation
                              // Can be configured here or in cortrix-agent/.env (env wins)
    LlmConfig doc_summary_llm;  // [F41] Document-level LLM summary (ingest-side, async via F42).
                                // Independent role (Derek 2026-06-08); the doc_summary feature is
                                // OFF unless IsConfigured() (api_key present) — main then builds an
                                // OpenAiLlmClient for the F41AsyncWorker.
    LlmConfig enricher_llm;     // [F03 · gap①] SPC ingest enricher (NER + summary). Independent
                                // role (Derek 2026-06-10, plan B — symmetric with doc_summary_llm;
                                // F03 §2.7.bis). OFF (NullEnricher) unless IsConfigured(); main
                                // maps it into EnricherConfig{type=kLlm, endpoint, api_key, model},
                                // remaining tuning fields keep the F03 §2.7 defaults.
    SPCConfig spc;
    UploadConfig upload;
    WatchDirConfig watch_dir;
    MemoryConfig memory;
    RerankerTopConfig reranker;              // F02 reranker model dir
    QueryComplexityTopConfig query_complexity;  // F39 complexity classifier model dir
    RetrievalConfig retrieval;               // candidate-pool sizing (over-fetch cap)
    SecurityConfig security;  // [F20] admin access policy
    GcConfig gc;              // [OPEN-2] three-stage GC + ops endpoints
};

/// Load configuration.
/// Priority: environment variables > YAML file > hardcoded defaults
/// @param config_path: YAML file path, empty string uses defaults+env only
CortrixConfig LoadConfig(const std::string& config_path);

/// Validate configuration, returning a list of error messages.
/// Empty vector means all checks passed.
/// Checks:
///   - server.port in [1, 65535]
///   - server.thread_count >= 1
///   - server.max_payload_bytes > 0
///   - ns.data_dir non-empty
///   - ns.max_active >= 1
///   - ns.idle_timeout_s >= 0
///   - embedding.dimension >= 1
///   - spc.worker_count >= 1
///   - spc.chunk_size >= 1
///   - spc.chunk_overlap >= 0 and < chunk_size
///   - spc.task_timeout_s >= 0
///   - spc.parser_timeout_s >= 0
///   - spc.embedding_batch_size >= 1
///   - log.level in {"trace","debug","info","warn","error","critical","off"}
///   - log.format in {"json","text"}
///   - log.output non-empty
///   - upload.max_file_size > 0
///   - watch_dir.scan_batch_size >= 0
///   - watch_dir.max_concurrent_imports >= 0
///   - watch_dir.error_threshold >= 0
///   - watch_dir.fsevents_latency_s >= 0.0
///   - memory.default_ttl_seconds >= 0
///   - memory.inject_recent_turns >= 0
///   - memory.inject_max_tokens >= 0
///   - memory.max_sessions_per_namespace >= 1
///   - memory.max_interactions_per_session >= 1
std::vector<std::string> ValidateConfig(const CortrixConfig& config);

}  // namespace cortrix
