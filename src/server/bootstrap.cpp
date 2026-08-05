#include "cortrix/server/bootstrap.h"

#include "cortrix/config/config.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <sqlite3.h>

#include "cortrix/logging/logging.h"
#include "cortrix/common/version.h"  // [P5] single SoT for the version string
#include "cortrix/auth/api_key_auth.h"
// [D3.5 r2 · Wave P · P1] P08-CE auth: platform.db + bootstrap + API Keys REST.
#include "cortrix/auth/platform_db.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/bootstrap_handler.h"
#include "cortrix/auth/admin_users_service.h"      // [FA1 R11] §2.13-bis admin/users
#include "cortrix/auth/jwt_secret_service.h"        // [FA1 R11] §2.11 JWT rotation
#include "cortrix/auth/pbkdf2_password_hasher.h"    // [FA1 R11] invite-mode create hasher
#include "cortrix/server/routes/auth_routes.h"
#include "cortrix/namespace/namespace_manager.h"
// [D3.5 wire ①②] F12 catalog + F05 NamespacePool runtime resource stack
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/default_unit_router.h"
// [OPEN-2] three-stage GC manager + background thread + ops routes
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/catalog/gc/gc_thread.h"
#include "cortrix/server/gc_cli.h"
#include "cortrix/server/routes/gc_routes.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/parser_factory.h"          // F06 structured parser factory
#include "cortrix/spc/docling_parser.h"          // F06 primary parser
#include "cortrix/spc/paddleocr_parser.h"        // F06 OCR fallback parser
#include "cortrix/chunker/parent_child_chunker.h" // F34 parent-child chunker
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc_enricher.h"                  // F03 CreateEnricher / EnricherConfig
#include "cortrix/spc/enricher_chain.h"            // I1 EnricherChain (F03→F35→F38)
#include "cortrix/spc_enricher/enrich_backfill_worker.h"  // §3.7 kTaskEnrichBackfill handler
#include "cortrix/spc_enricher/enrich_retry_sweeper.h"    // §3.7 enrich_state due-row sweeper
#include "cortrix/server/routes/enrich_routes.h"          // §3.7 backfill ops surface
#include "cortrix/spc/contextual_enricher.h"       // I2 ContextualRetrievalEnricher / ResolveContextualConfig
#include "cortrix/spc/hype_enricher.h"             // I3 HyPEEnricher / HyPEConfig
#include "cortrix/spc/hype_ns_config.h"            // ClampHypeK (F38 §4.3 yaml wiring)
#include "cortrix/llm/openai_client.h"             // OpenAiLlmClient (shared enricher LLM)
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/cleaning_config_resolver.h"  // per-NS CleaningConfig resolve
// [F42 main wiring] async subsystem + F41 doc-summary worker + LLM client
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/document_processor.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/async/task_type.h"
#include "cortrix/doc_summary/f41_async_worker.h"
#include "cortrix/doc_summary/doc_summary_generator.h"
#include "cortrix/llm/openai_client.h"
#include "cortrix/upload/upload_handler.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/query_wiring.h"   // [D3.5-R2 Q1/Q2] F04 cross-NS query live mount
#include "cortrix/retrieval/sparse_index_registry.h"  // [Q4 F40] shared per-NS SPLADE index
#include "cortrix/memory/memory_routes.h"
#include "cortrix/memory/memory_extraction_service.h"  // M1 MEM02 extraction runtime
#include "cortrix/server/routes/observability_routes.h" // [F13 TC4] RegisterTracesRoutesGlobal + RegisterInteractionsRoutesPerNs
// [V1.0 acceptance · Wave2 integration] A3 startup validation / A4+TC4 F13 agent_trace
// global wiring / A6 doc-summary discover route / F query-trace EngineInstrumentation.
#include "cortrix/onnx/startup_validator.h"               // A3 F22 fail-fast ONNX validation
#include "cortrix/auth/auth_middleware.h"                 // A6 WithAuth for the discover route
#include "cortrix/doc_summary/discover_handler.h"         // A6 GET /documents/discover
#include "cortrix/agent_trace/agent_trace_schema.h"       // TC4 agent_trace in global catalog.db
#include "cortrix/agent_trace/agent_trace_writer_impl.h"  // TC4/A4/F shared agent_trace writer
#include "cortrix/agent_trace/engine_instrumentation.h"   // F query agent_trace write side
#include "cortrix/agent_trace/f13_cleanup_registrar.h"    // A4 agent_trace 90d cleanup
#include "cortrix/agent_trace/interaction_log_sweeper.h"  // A4 interaction_log 180d per-NS sweep
#include "cortrix/server/routes/agent_proxy_routes.h"
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/server/routes/flat_document_routes.h"
#include "cortrix/server/routes/connector_routes.h"
#include "cortrix/async/document_task_handler.h"
// [D3.5 r2 · Wave P · P3] TD-F42-BULK batch submit route + its production submitter.
#include "cortrix/server/routes/batch_routes.h"
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/async/managed_input.h"
#include "cortrix/server/batch_temp_store.h"
#include "cortrix/server/f42_task_submitter_adapter.h"
// [D3.5 r2 · Wave P · P4] F48 §6.3 agent_llm_config admin API.
#include "cortrix/server/routes/system_config_routes.h"
// [D3.5 r2 · Wave P · P2] F16a DB-import: ImportManager DI + 6 endpoints.
#include "cortrix/server/routes/import_routes.h"
#include "cortrix/server/import_handler.h"
#include "cortrix/import/import_manager.h"
#include "cortrix/import/connection_manager.h"
#include "cortrix/import/query_executor.h"
#include "cortrix/import/text_serializer.h"
#include "cortrix/import/spc_feed.h"
#include "cortrix/import/spc_manager_feeder.h"   // [P2b] real SPCManager feed + cleaner
#include "cortrix/import/import_task_queue.h"
// [D3.5 batch1] P09 tenant/permission/quota REST surface
#include "cortrix/server/routes/tenant_routes.h"
#include "cortrix/tenant/tenant_service.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/i_plan_provider.h"
// [D3.5 batch1] F18a operation-log query route + DI module + schema provider
#include "cortrix/server/routes/operations_routes.h"
#include "cortrix/observability/observability_module.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/connector/directory_importer.h"
#include "cortrix/connector/dir_watcher_registry.h"       // ConnectorState holds unique_ptr<DirWatcherRegistry> (complete type for dtor)
#include "cortrix/server/http_server.h"
// security hardening
#include "cortrix/middleware/admin_guard.h"
#include "cortrix/middleware/rate_limiter.h"               // pre-routing rate-limit gate
#include "cortrix/logging/sanitizer.h"
// deployment: dual health endpoint + OpenMetrics :9091 + disk monitor + graceful shutdown
#include "cortrix/deploy/graceful_shutdown.h"
#include "cortrix/resource/namespace_facade.h"   // [gap⑤] resume re-hydrates tasks from the doc row
#include "cortrix/deploy/health_routes.h"
#include "cortrix/health/readiness.h"  // F20 readiness contract — components registered at wiring
#include "cortrix/ml/execution_provider.h"
#include "cortrix/security/env_secret_provider.h"      // F20-6 CreateSecretProvider() for /ready
#include "cortrix/security/secret_provider_readiness.h" // F20-6 secret_provider readiness adapter
#include "cortrix/deploy/metrics_server.h"
#include "cortrix/deploy/deploy_metrics.h"
#include "cortrix/deploy/disk_monitor.h"
// [D3.5 wire · gap④] §5.3 subsystem metric recorders (process-wide singletons,
// MET round standalone-implemented) aggregated into the :9091 /metrics endpoint.
#include "cortrix/async/f42_metrics.h"
#include "cortrix/query/scatter_metrics.h"
#include "cortrix/query/rag_fusion_metrics.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/scoring/scoring_metrics.h"
#include "cortrix/resource/ns_pool_metrics.h"
#include "cortrix/reranker/reranker_metrics.h"
#include "cortrix/onnx/onnx_metrics.h"
#include "cortrix/spc_enricher/enricher_metrics.h"
#include "cortrix/spc/contextual_metrics.h"
#include "cortrix/spc/hype_metrics.h"
#include "cortrix/retrieval/crag_metrics.h"
#include "cortrix/retrieval/sparse_metrics.h"
#include "cortrix/metadata/metadata_metrics.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "cortrix/import/import_metrics.h"
#include "cortrix/memory/mem02_metrics.h"
#include "cortrix/memory/mem03_metrics.h"
#include "cortrix/memory/mem04_metrics.h"
#include "cortrix/memory/mem05_metrics.h"
#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/observability/oplog_metrics.h"

#include <atomic>
#include <csignal>
#include <memory>
#include <string>

static cortrix::CortrixHttpServer* g_server = nullptr;
// [D3.5 wire] Set when shutdown begins so the /ready endpoint reports not-ready
// (503) before the listener actually closes — the orchestrator stops routing to a
// draining process. Read from request threads, so atomic.
static std::atomic<bool> g_shutting_down{false};

void SignalHandler(int sig) {
    (void)sig;
    // Mark draining first so an in-flight /ready probe sees the shutdown, then stop
    // the server — this unblocks the main thread which handles connector + SPC
    // cleanup in the graceful shutdown section.
    g_shutting_down.store(true);
    if (g_server) {
        g_server->Stop();
    }
}

namespace cortrix::server {

int RunServer(int argc, char* argv[], const ServerExtensions& extensions) {
    // Initialize SQLite's global state while still single-threaded. sqlite3_open
    // would lazy-init on first use, but with per-facade private connections
    // (F05 D-I1.bis) the first opens can race from worker threads — TSAN flags
    // the unsynchronized bootstrap inside sqlite3MutexInit. One explicit call
    // here pins the documented "init before threads" invariant.
    sqlite3_initialize();

    // [OPEN-2] GC admin subcommands (gc-status / gc-run / restore / purge) run
    // against the catalog and exit without starting the server. Returns a value
    // only when argv[1] is a gc subcommand.
    if (auto rc = MaybeRunGcCli(argc, argv); rc.has_value()) {
        return *rc;
    }

    // Parse --config <path> argument
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    // ---------------------------------------------------------------
    // Initialization order (must be respected for correct operation):
    //   1. Config
    //   2. Logging
    //   3. NamespaceManager (metadata SQLite)
    //   4. Auth
    //   5. F05 NamespacePool (per-NS resource pool; replaces the MVP manager)
    //   6. OnnxEmbedder (ONNX Runtime for embeddings)
    //   7. SPC pipeline components (parser, chunker, OCR, assembler)
    //   8. SPCManager (background document processing workers)
    //   9. UploadHandler
    //  10. Query components (IntentClassifier, RRFFusion)
    //  11. HTTP Server + route registration
    //  12. Signal handlers
    //  13. Start SPC workers + HTTP listen
    // ---------------------------------------------------------------

    // 1. Load config
    auto config = cortrix::LoadConfig(config_path);
    const auto config_errors = cortrix::ValidateConfig(config);
    if (!config_errors.empty()) {
        std::cerr << "[config] ERROR: Invalid configuration:\n";
        for (const auto& error : config_errors) {
            std::cerr << "  - " << error << '\n';
        }
        return 1;
    }

    // 2. Initialize logging
    cortrix::InitLogging(config.log);
    CORTRIX_LOG_INFO("main", "Cortrix server starting...");

    // Load log sanitization rules + validate admin access config.
    // ValidateConfigOrAbort() is the V3-E-06 hard-fail: CORTRIX_ADMIN_BIND=0.0.0.0
    // without CORTRIX_ALLOW_PUBLIC_ADMIN=true aborts startup here.
    cortrix::logging::LogSanitizer::Initialize("/app/config/sensitive_fields.yaml");
    cortrix::middleware::AdminGuard::ValidateConfigOrAbort();

    std::error_code data_dir_ec;
    std::filesystem::create_directories(config.ns.data_dir, data_dir_ec);
    if (data_dir_ec) {
        CORTRIX_LOG_ERROR("main", "Failed to create namespace data_dir ({}): {}",
                          config.ns.data_dir, data_dir_ec.message());
        return 1;
    }

    // 3. Initialize namespace metadata manager
    cortrix::NamespaceManager ns_mgr(config.ns.data_dir);
    auto s = ns_mgr.Init();
    if (!s.ok()) {
        CORTRIX_LOG_ERROR("main", "Failed to initialize namespace manager: {}", s.message());
        return 1;
    }

    // 4. Initialize auth
    cortrix::ApiKeyAuth auth;
    auth.set_enabled(config.auth.enabled);
    auth.LoadKeys(config.auth.api_keys);

    // 4b. [D3.5 r2 · Wave P · P1] P08-CE auth layer: open platform.db (the Auth SoT,
    // P08 §3.1 — a SEPARATE file from catalog.db), build the API Key service +
    // bootstrap handler, and on a first start (empty `users` table) print the 60s
    // single-use admin-bootstrap banner. The ApiKeyService is bound into ApiKeyAuth
    // so runtime-minted keys authenticate (bootstrap admin key + /auth/api-keys).
    cortrix::auth::PlatformDb platform_db;
    {
        const std::string platform_path = config.ns.data_dir + "/platform.db";
        if (auto ps = platform_db.Open(platform_path); !ps.ok()) {
            CORTRIX_LOG_ERROR("main", "Failed to open platform.db ({}): {}",
                              platform_path, ps.message());
            return 1;
        }
    }
    cortrix::auth::ApiKeyService api_key_service(platform_db.db());
    auth.SetApiKeyService(&api_key_service);
    cortrix::auth::BootstrapHandler bootstrap_handler(platform_db.db(), &api_key_service);

    // [FA1 R11] P08 admin/users (§2.13-bis) + JWT rotation (§2.11) backends.
    // The PBKDF2 hasher is the invite-mode create path's password hasher (same
    // TECH_DEBT-P08-PBKDF2-PLACEHOLDER seam as the login flow). JwtSecretService
    // LoadOrInit reads/auto-generates the HS256 secret in platform.db auth_secrets
    // (§2.11) so the rotation route has a live current secret to rotate.
    cortrix::auth::Pbkdf2PasswordHasher admin_users_hasher;
    cortrix::auth::AdminUsersService admin_users_service(platform_db.db(), &admin_users_hasher);
    cortrix::auth::JwtSecretService jwt_secret_service(platform_db.db());
    if (auto js = jwt_secret_service.LoadOrInit(); !js.ok()) {
        CORTRIX_LOG_ERROR("main", "JWT secret LoadOrInit failed (rotation route disabled): {}",
                          js.message());
    }
    if (auto token = bootstrap_handler.GenerateToken(); token.ok() && !token.value().empty()) {
        // First start: surface the one-time setup URL on stdout. The
        // token lives in memory only; visiting either bootstrap endpoint mints the
        // admin API Key and invalidates it. AdminGuard restricts the path to
        // loopback by default, so this is local-operator-only.
        // Print the actually-configured bind host/port, not a hardcoded :8420.
        // A wildcard/empty bind host is shown as localhost since the setup URL is
        // opened from the local operator's browser (AdminGuard is loopback-only).
        std::string setup_host = config.server.host;
        if (setup_host.empty() || setup_host == "0.0.0.0" || setup_host == "::") {
            setup_host = "localhost";
        }
        std::printf(
            "\n>>> First-time admin setup URL (60s, single-use):\n"
            ">>>   http://%s:%d/api/v1/admin/bootstrap?token=%s\n"
            ">>>\n"
            ">>> Visit this URL in your browser within 60 seconds to obtain the "
            "admin API Key.\n\n",
            setup_host.c_str(), static_cast<int>(config.server.port), token.value().c_str());
        std::fflush(stdout);
    } else if (!token.ok()) {
        CORTRIX_LOG_WARN("main", "bootstrap token generation skipped: {}",
                         token.status().message());
    }

    // 5. [D3.5 wire ①②] F12 catalog + F05 NamespacePool — the Phase-1 runtime
    // resource stack (replaced the MVP namespace manager, removed in wire ⑥). On a
    // fresh, empty catalog StartupLoadAll is a no-op (0 namespaces), so this stack
    // stays inert until namespaces are created through the F13 path.
    cortrix::catalog::CatalogDb catalog_db;
    {
        const std::string catalog_path = config.ns.data_dir + "/catalog.db";
        // [D3.5 batch1/F18a] migrate the operation_log schema into the
        // catalog/global DB at startup so ObservabilityModule attaches to an
        // already-migrated handle (its ctor doc requires the schema to exist).
        cortrix::observability::OperationLogSchemaProvider oplog_schema;
        // [F13 TC4] agent_trace lives in the global catalog.db now (moved out of the
        // per-NS memory.db, where memory_store keeps only interaction_sources). Migrate
        // its schema here so the writer (A4/F) + reader (RegisterTracesRoutesGlobal) both
        // bind an already-built table.
        cortrix::agent_trace::AgentTraceSchemaProvider agent_trace_schema;
        auto cs = catalog_db.Open(catalog_path, {&oplog_schema, &agent_trace_schema});
        if (!cs.ok()) {
            CORTRIX_LOG_ERROR("main", "Failed to open catalog.db ({}): {}",
                              catalog_path, cs.message());
            return 1;
        }
    }
    cortrix::catalog::DefaultINSRouter ns_router(catalog_db.db(), /*f05_pool=*/nullptr);
    cortrix::catalog::DefaultUnitRouter unit_router(catalog_db.db());

    // [OPEN-2] GC layer 1 — catalog.db / blob_gc_queue. The Stage 3 sink resolves
    // the content-addressed blob_uri ({hash[0:2]}/{hash}, F25 layer) under the blob
    // root; a missing blob is an idempotent no-op. Layer 2 (DocumentGcSweeper) is
    // built below once the F05 NamespacePool exists. The background thread runs both
    // layers each cycle (gc.enabled-gated); Start()ed after the workers come up,
    // Stop()ed in graceful shutdown.
    cortrix::catalog::gc::LocalBlobGcSink gc_blob_sink(config.ns.data_dir + "/blobs");
    cortrix::catalog::gc::GcManager gc_manager(catalog_db.db(), &gc_blob_sink, config.gc);
    cortrix::store::PhnswIndexFactory index_factory;
    cortrix::resource::WriteCoordinatorFactory wc_factory =
        [](const std::string& data_dir,
           const cortrix::store::WriteCoordinatorConfig& cfg,
           cortrix::store::IVectorStore* vs, cortrix::store::IMetadataStore* ms,
           cortrix::store::IBlobStore* bs)
            -> cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>> {
        auto coord = std::make_unique<cortrix::store::WriteCoordinator>(
            data_dir, cfg, vs, ms, bs);
        auto init = coord->Init();
        if (!init.ok()) return init;
        return cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>>(
            std::move(coord));
    };
    cortrix::resource::F05Config f05_config;
    f05_config.data_root = config.ns.data_dir + "/units";  // per-Unit layout root
    f05_config.load_timeout_ms_per_ns = config.ns.load_timeout_ms_per_ns;  // configurable startup-load guard
    cortrix::resource::DefaultNamespacePool ns_pool(
        &index_factory, std::move(wc_factory), &ns_router, &unit_router, f05_config);
    {
        auto startup = ns_pool.StartupLoadAll();
        if (startup.ok()) {
            const auto& rep = startup.value();
            CORTRIX_LOG_INFO("main",
                             "F05 pool StartupLoadAll: {} loaded / {} failed / {} total",
                             rep.loaded_successfully, rep.failed, rep.total_namespaces);
        } else {
            CORTRIX_LOG_WARN("main", "F05 pool StartupLoadAll failed: {}",
                             startup.status().message());
        }
    }
    // [D3.5 wire⑤c / F13] Late-bind the pool into the router now that both exist
    // (breaks the pool↔router ctor cycle). From here, INSRouter.CreateNamespace
    // admits the new NS into ns_pool before the catalog INSERT.
    ns_router.SetPool(&ns_pool);

    // [OPEN-2] GC layer 2 — per-Unit document GC over the F05 pool (iterates NSs
    // via the router, acquires each facade, hard-deletes expired soft-deleted docs:
    // MarkDelete vectors + blob().del + block/doc rows). The GcThread drives both
    // layers each cycle.
    cortrix::catalog::gc::DocumentGcSweeper gc_doc_sweeper(&ns_router, &ns_pool, config.gc);
    cortrix::catalog::gc::GcThread gc_thread(&gc_manager, &gc_doc_sweeper);

    // 6. OnnxEmbedder
    cortrix::OnnxEmbedder embedder(
        config.embedding.model_path,
        config.embedding.dimension,
        config.spc.onnx_intra_threads,
        config.spc.onnx_inter_threads);
    if (!config.embedding.tokenizer_path.empty()) {
        embedder.set_tokenizer_path(config.embedding.tokenizer_path);
    }
    embedder.set_max_seq_length(config.embedding.max_seq_length);
    embedder.set_execution_provider(config.embedding.execution_provider);
    // [F22 D3.5 · A3] Fail-fast ONNX startup validation BEFORE the embedder initializes:
    // an ABI-mismatched runtime, or an out-of-range opset on a PRESENT model, aborts the
    // process here (in the operator's startup window) rather than at the first inference.
    // Validation skips missing paths so Init() can emit the component-specific error.
    // An intentionally empty model path is the only supported stub-mode contract;
    // a configured non-empty path that is absent or invalid fails startup below.
    {
        auto onnx_val = cortrix::onnx::StartupValidator::CollectRegisteredOnnxModels(config);
        onnx_val.skip_missing_models = true;
        if (auto vs = cortrix::onnx::StartupValidator::Validate(onnx_val); !vs.ok()) {
            CORTRIX_LOG_ERROR("main", "ONNX startup validation failed: {}", vs.message());
            return 1;
        }
        CORTRIX_LOG_INFO("main", "ONNX startup validation passed ({} model(s) registered)",
                         onnx_val.registered_model_paths.size());
    }
    s = embedder.Init();
    if (!s.ok()) {
        CORTRIX_LOG_ERROR("main", "OnnxEmbedder init failed: {}", s.message());
        return 1;
    }
    CORTRIX_LOG_INFO("main",
                     "OnnxEmbedder initialized (real_model={}, configured_ep={}, active_ep={})",
                     embedder.is_real_model(), embedder.configured_ep(), embedder.active_ep());

    // Auto-resolve worker_count=0: 2 workers in both GPU and CPU-only environments.
    // HfTokenizer::Encode() is const (read-only, thread-safe); Ort::Session::Run()
    // is thread-safe per ONNX Runtime docs. CoreML serializes concurrent Run() calls
    // internally. For file pipelines, parse/OCR (not embedding) is usually the bottleneck,
    // so 2 workers provide real parallelism on IO/parse/chunk stages.
    if (config.spc.worker_count == 0) {
        config.spc.worker_count = 2;
        CORTRIX_LOG_INFO("main", "worker_count auto={} (embedding_active_ep={})",
                         config.spc.worker_count, embedder.active_ep());
    }

    // 6b. [F24 · moved up for gap②] Disk monitor: periodic statvfs over the data
    // dir, feeds cortrix_disk_usage_ratio + the write-reject flag. Constructed
    // BEFORE SPCManager / route registration so both can hold it (probe lambda /
    // pointer) — RAII then tears them down first while the monitor is still alive.
    cortrix::deploy::DiskMonitor f24_disk_monitor(
        cortrix::deploy::DiskMonitorConfig::FromGlobalConfig(nullptr, config.ns.data_dir),
        [](const cortrix::deploy::DiskUsage& u) {
            CORTRIX_LOG_WARN("disk", "Disk stage {} at {:.1f}%",
                             cortrix::deploy::ToString(u.stage), u.usage_ratio * 100.0);
        });
    f24_disk_monitor.Start();

    // 7. SPC pipeline components — F06 structured parser (owns OCR/VLM internally)
    //    + F34 parent-child chunker (Inc 4-3: A unified-blocks ingest swap).
    // [D3.5 wire · #479] Real bridge-script lookup: the old "" placeholder went
    // straight into argv (python3 "" …) and every parse failed. Resolution order:
    // CORTRIX_SCRIPTS_DIR env → /app/scripts (container layout) → ./scripts
    // (repo-root dev run). Empty result = script genuinely absent; the parser
    // then surfaces SUBPROCESS_FAILED with a clear hint.
    auto locate_script = [](const char* script_name) -> std::string {
        std::vector<std::string> candidates;
        if (const char* dir = std::getenv("CORTRIX_SCRIPTS_DIR"); dir && *dir) {
            candidates.push_back(std::string(dir) + "/" + script_name);
        }
        candidates.push_back(std::string("/app/scripts/") + script_name);
        candidates.push_back(std::string("scripts/") + script_name);
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c)) return c;
        }
        return "";
    };
    const std::string docling_script = locate_script("docling_bridge.py");
    if (docling_script.empty()) {
        if (config.spc.require_parsers) {
            // spc.require_parsers / CORTRIX_REQUIRE_PARSERS: a host that cannot
            // parse documents must fail at STARTUP, not answer health 200 while
            // 100% of ingests die CX_ERR_PARSE_FAILED (observed as a full
            // 5000-doc wipeout, 2026-07-09).
            CORTRIX_LOG_ERROR(
                "main",
                "docling_bridge.py not found and spc.require_parsers=true — "
                "checked CORTRIX_SCRIPTS_DIR, /app/scripts, ./scripts (cwd={})",
                std::filesystem::current_path().string());
            cortrix::ShutdownLogging();
            return 1;
        }
        CORTRIX_LOG_WARN("main", "docling_bridge.py not found (CORTRIX_SCRIPTS_DIR? /app/scripts? ./scripts?) — document parsing will fail");
    }

    cortrix::spc::ParserFactoryConfig parser_factory_config;
    parser_factory_config.docling.python_path = config.spc.python_bin;
    parser_factory_config.docling.script_path = docling_script;
    parser_factory_config.docling.subprocess_timeout_ms = config.spc.parser_timeout_s * 1000;
    parser_factory_config.paddleocr.python_path = config.spc.python_bin;
    parser_factory_config.paddleocr.script_path = config.spc.ocr_script.empty()
        ? locate_script("paddleocr_bridge.py") : config.spc.ocr_script;
    parser_factory_config.paddleocr.subprocess_timeout_ms = config.spc.ocr_timeout_s * 1000;
    parser_factory_config.enable_paddleocr = true;
    parser_factory_config.fallback_confidence_threshold = config.spc.ocr_confidence_threshold;

    cortrix::spc::DocumentParserFactory f06_factory(parser_factory_config);
    {
        cortrix::spc::DoclingParserConfig dc;
        dc.python_path = config.spc.python_bin;
        dc.script_path = docling_script;
        dc.subprocess_timeout_ms = config.spc.parser_timeout_s * 1000;
        f06_factory.SetPrimaryParser(std::make_unique<cortrix::spc::DoclingParser>(dc));
        cortrix::spc::PaddleOCRParserConfig pc;
        pc.python_path = config.spc.python_bin;
        pc.script_path = parser_factory_config.paddleocr.script_path;
        pc.subprocess_timeout_ms = config.spc.ocr_timeout_s * 1000;
        f06_factory.SetFallbackParser(std::make_unique<cortrix::spc::PaddleOCRParser>(pc));
    }

    cortrix::chunker::ParentChildChunker pc_chunker(cortrix::chunker::ChunkerConfig{});
    cortrix::BlockAssembler assembler;
    // [D3.5 wire · gap①] F03 enricher (plan B, F03 §2.7.bis): the connection triple
    // comes from the config.yaml `enricher_llm` role (symmetric with doc_summary_llm);
    // the remaining tuning fields keep the F03 §2.7 defaults. Unconfigured → kNull
    // (NullEnricher short-circuit). CreateEnricher then runs the §4.1 startup probe
    // and auto-degrades to NullEnricher on api_key/endpoint failure (§4.4).
    cortrix::spc::EnricherConfig enricher_cfg;
    if (config.enricher_llm.IsConfigured()) {
        enricher_cfg.type = cortrix::spc::EnricherType::kLlm;
        enricher_cfg.endpoint = config.enricher_llm.base_url.empty()
                                    ? enricher_cfg.endpoint  // keep the OpenAI default
                                    : config.enricher_llm.base_url;
        enricher_cfg.api_key = config.enricher_llm.api_key;
        enricher_cfg.model = config.enricher_llm.model;
        // Wire the configured per-call timeout through (the YAML enricher_llm
        // timeout_ms was parsed but never reached EnricherConfig, leaving the
        // 30s default — too tight for batch-of-8 JSON generation on slower
        // providers, where it turns healthy calls into pool timeouts).
        if (config.enricher_llm.timeout_ms > 0) {
            enricher_cfg.task_timeout_ms = config.enricher_llm.timeout_ms;
        }
        // Same plumb for batch_size (clamped to the §2.7 documented 1-32 range):
        // batch-8 JSON generation can outlast provider gateway idle windows on
        // slower models — observed live as mid-read connection resets.
        if (config.enricher_llm.batch_size > 0) {
            enricher_cfg.batch_size = std::min(config.enricher_llm.batch_size, 32);
        }
        if (config.enricher_llm.max_tokens > 0) {
            // Clamp here too so the startup line below logs the EFFECTIVE value
            // (mirrors batch_size above); the enricher slot re-clamps as the
            // defense-in-depth backstop (QA F-7).
            enricher_cfg.max_tokens = std::min(
                config.enricher_llm.max_tokens, cortrix::spc::kEnricherMaxTokensCap);
        }
        CORTRIX_LOG_INFO("main",
                         "F03 enricher enabled (model={} timeout_ms={} batch_size={} max_tokens={})",
                         config.enricher_llm.model, enricher_cfg.task_timeout_ms,
                         enricher_cfg.batch_size, enricher_cfg.max_tokens);
    } else {
        CORTRIX_LOG_INFO("main", "F03 enricher disabled (enricher_llm not configured)");
    }
    auto enricher = cortrix::spc::CreateEnricher(enricher_cfg);

    // [I1 · GS-2] Enricher chain (F03 → F35 → F38, fail-soft serial). The chain
    // supersedes the single F03 enricher above when it has any available member.
    // It is resolved from the `enricher.chain` GUC (default "f03"); when the
    // enricher LLM is configured we seed the GUC to the full chain so F35/F38 join
    // (they share that LLM). The chain + its enrichers live at function scope so
    // they outlive spc_mgr (which holds a non-owning pointer via the pipeline).
    auto enricher_chain_config = std::make_shared<cortrix::InMemoryGlobalConfig>();
    if (config.enricher_llm.IsConfigured()) {
        enricher_chain_config->Set("enricher.chain", "f03,f35,f38");
    }
    std::shared_ptr<cortrix::llm::ILlmClient> enricher_chain_llm;
    if (config.enricher_llm.IsConfigured()) {
        cortrix::llm::LlmClientConfig chain_llm_cfg;
        chain_llm_cfg.endpoint = config.enricher_llm.base_url.empty()
                                     ? "https://api.openai.com/v1"
                                     : config.enricher_llm.base_url;
        chain_llm_cfg.api_key = config.enricher_llm.api_key;
        chain_llm_cfg.default_model = config.enricher_llm.model;
        enricher_chain_llm =
            std::make_shared<cortrix::llm::OpenAiLlmClient>(std::move(chain_llm_cfg));
    }
    // Non-owning shared_ptr aliasing the stack embedder (it outlives the chain) so
    // the F35 ContextualOnnxEmbedder adapter (which takes a shared_ptr) can reuse
    // the same OnnxEmbedder the pipeline embeds with.
    std::shared_ptr<cortrix::OnnxEmbedder> embedder_alias(
        std::shared_ptr<void>{}, &embedder);
    cortrix::spc::EnricherChain enricher_chain;
    {
        std::vector<std::string> chain_tokens =
            cortrix::spc::ResolveEnricherChain(enricher_chain_config.get(), "");
        for (const auto& tok : chain_tokens) {
            if (tok == "f03") {
                // F03 head: reuse the single enricher already created above (shared, so
                // the chain and the pipeline ctor arg point at the same object).
                enricher_chain.Append(
                    std::shared_ptr<cortrix::spc::ISpcEnricher>(enricher.get(), [](auto*) {}));
            } else if (tok == "f35" && enricher_chain_llm) {
                auto f35_cfg =
                    cortrix::spc::ResolveContextualConfig(enricher_chain_config.get());
                // Wire the configured enricher_llm model through (mirrors the F41
                // DEFECT#3 fix): the built-in "gpt-4o-mini" default is rejected
                // with HTTP 400 by non-OpenAI providers (GLM/Claude/local), which
                // silently zeroed all F35 output.
                if (!config.enricher_llm.model.empty()) {
                    f35_cfg.llm_model = config.enricher_llm.model;
                }
                // Same wiring for the per-call deadline: a configured
                // enricher_llm.timeout_ms overrides the §6.1 10s built-in,
                // which slow providers exceed on every large-prompt call.
                if (config.enricher_llm.timeout_ms > 0) {
                    f35_cfg.timeout_ms = std::max(config.enricher_llm.timeout_ms,
                                                  cortrix::spc::kContextualTimeoutMsMin);
                }
                // F35 §6.2 knobs without a yaml alias until 2026-07-10 (deep-QA):
                // context token budget + the §8 injection-guard multiplier (whose
                // fixed 2 rejected legitimate >160-byte contexts and silently
                // dropped their contextualized vectors). 0 = built-in defaults.
                if (config.enricher_llm.ctx_max_output_tokens > 0) {
                    f35_cfg.max_output_tokens =
                        std::clamp(config.enricher_llm.ctx_max_output_tokens,
                                   cortrix::spc::kContextualMaxOutputTokensMin,
                                   cortrix::spc::kContextualMaxOutputTokensMax);
                }
                if (config.enricher_llm.ctx_guard_chars_per_token > 0) {
                    f35_cfg.guard_chars_per_token =
                        std::clamp(config.enricher_llm.ctx_guard_chars_per_token,
                                   cortrix::spc::kContextualGuardCharsPerTokenMin,
                                   cortrix::spc::kContextualGuardCharsPerTokenMax);
                }
                auto f35 = std::make_shared<cortrix::spc::ContextualRetrievalEnricher>(
                    f35_cfg, enricher_chain_llm,
                    std::make_shared<cortrix::spc::ContextualOnnxEmbedder>(embedder_alias));
                enricher_chain.Append(std::move(f35));
            } else if (tok == "f38" && enricher_chain_llm) {
                cortrix::spc::HyPEConfig hype_cfg;  // K=3 default; parent_text bound in pipeline
                // Same DEFECT#3-family wiring as F35 above: send the configured
                // model, not the OpenAI-only built-in default.
                if (!config.enricher_llm.model.empty()) {
                    hype_cfg.llm_model = config.enricher_llm.model;
                }
                // F38 §4.3 global config path, as-built 2026-07-10: the design's
                // hype_questions_per_chunk knob existed end to end (KV key +
                // NsHyPEConfig + ResolveHypeK + tests) but nothing in production
                // ever fed it — K was effectively frozen at 3. yaml
                // enricher_llm.hype_questions_per_chunk now reaches the enricher
                // (0 = keep default; clamped to the design range [1, 10]). The
                // per-NS hype_config JSONB path is still unwired (registered gap).
                if (config.enricher_llm.hype_questions_per_chunk > 0) {
                    hype_cfg.questions_per_chunk =
                        cortrix::spc::ClampHypeK(config.enricher_llm.hype_questions_per_chunk);
                    CORTRIX_LOG_INFO("main", "F38 hype questions_per_chunk={} (yaml override)",
                                     hype_cfg.questions_per_chunk);
                }
                // Same enricher_llm.timeout_ms wiring F03/F35 already honor —
                // F38 historically ignored it and rode the client's default.
                if (config.enricher_llm.timeout_ms > 0) {
                    hype_cfg.timeout_ms = std::max(config.enricher_llm.timeout_ms, 1000);
                }
                auto f38 = std::make_shared<cortrix::spc::HyPEEnricher>(
                    hype_cfg, enricher_chain_llm,
                    /*parent_store=*/nullptr);  // parent_text supplied per-chunk by the pipeline
                enricher_chain.Append(std::move(f38));
            }
        }
        CORTRIX_LOG_INFO("main", "enricher chain resolved: [{}]",
                         [&] {
                             std::string s;
                             for (const auto& n : enricher_chain.Names()) {
                                 if (!s.empty()) s += ",";
                                 s += n;
                             }
                             return s;
                         }());
    }

    // 7b. [F42 main wiring] The async subsystem, wired BEFORE the SPCPipeline so the
    //     ⑤c doc-summary enqueue seam can be installed on the pipeline before it
    //     moves into SPCManager. TaskManager (tasks.db) → TaskScheduler → WorkerPool
    //     dispatch (kTaskDocParse → DocumentProcessor; kTaskDocSummary →
    //     F41AsyncWorker, only when doc_summary_llm is configured).
    auto f42_config = std::make_shared<cortrix::InMemoryGlobalConfig>();
    f42_config->Set("f42.worker_pool_size",
                    std::to_string(config.spc.worker_count > 0 ? config.spc.worker_count : 2));
    // Default 4 (parser-subprocess memory protection); explicitly raisable via
    // spc.parser_max_concurrent for workloads that spawn no parsers (the F42
    // pool-size gate compares against this value).
    f42_config->Set("f06.parser_max_concurrent",
                    std::to_string(config.spc.parser_max_concurrent > 0
                                       ? config.spc.parser_max_concurrent
                                       : 4));
    // F42 enrich-retry sweeper pacing (deep-QA 2026-07-10): the sweeper already
    // reads these keys but nothing ever seeded them, freezing the 60s x 32
    // built-ins (a multi-hour drain at 5000-doc scale). 0/absent = unchanged.
    if (config.spc.enrich_sweep_interval_sec > 0) {
        f42_config->Set("f42.enrich_sweep_interval_sec",
                        std::to_string(config.spc.enrich_sweep_interval_sec));
    }
    if (config.spc.enrich_sweep_batch > 0) {
        f42_config->Set("f42.enrich_sweep_batch",
                        std::to_string(config.spc.enrich_sweep_batch));
    }

    cortrix::async::TaskManager task_mgr;
    if (cortrix::Status ti = task_mgr.Init(config.ns.data_dir + "/tasks.db"); !ti.ok()) {
        CORTRIX_LOG_ERROR("main", "F42 TaskManager init failed: {}", ti.message());
        cortrix::ShutdownLogging();
        return 1;
    }
    cortrix::async::TaskScheduler task_scheduler(&task_mgr, f42_config.get());

    // 8. SPCPipeline + SPCManager — constructed BEFORE doc_processor (Plan B · F42 §4.1.2):
    // the F42 doc-parse handler borrows &spc_mgr to hand its parsed doc to ProcessParsedDoc.
    // The doc-summary seam is installed LATER (after the WorkerPool exists) via the spc_mgr
    // proxy, breaking the doc_processor → spc_mgr → pipeline(seam) → worker_pool cycle.
    auto pipeline = std::make_unique<cortrix::SPCPipeline>(
        f06_factory, pc_chunker, embedder, assembler, *enricher);
    cortrix::SPCManager spc_mgr(config.spc, ns_pool, std::move(pipeline));
    // [I1 · GS-2] Install the enricher chain on the managed pipeline (no-op when the
    // chain has no available member → pipeline keeps the single-enricher path).
    spc_mgr.SetEnricherChain(&enricher_chain);
    // [Q4 · F40] The single shared per-NS SPLADE inverted-index registry: the SPC
    // write path indexes into it at ingest, the cross-NS query read path searches
    // it — one instance so the read serves exactly what was written. Per-NS index
    // files live at <data_dir>/<ns>/sparse_index.db. Must outlive both spc_mgr and
    // the query wiring (declared here, at the same scope).
    cortrix::retrieval::SparseIndexRegistry sparse_index_registry(config.ns.data_dir);
    spc_mgr.SetSparseIndexRegistry(&sparse_index_registry);
    // [D3.5 wire · gap②] F24 disk gate on task admission (catch-all for watcher /
    // CDC paths that Submit() directly; the HTTP upload route rejects earlier).
    spc_mgr.SetWriteRejectProbe(
        [&f24_disk_monitor] { return f24_disk_monitor.ShouldRejectWrites(); });

    // The last argument releases each batch-materialized input file once its task
    // is terminal (SEC/lifecycle: those files exist only to feed the parser).
    cortrix::async::DocumentProcessor doc_processor(
        &task_mgr, &f06_factory, f42_config.get(), nullptr, &spc_mgr,
        cortrix::server::BatchTempDir(config.ns.data_dir));

    // F41 doc-summary worker — built only when an LLM is configured; otherwise the
    // feature is OFF (no kTaskDocSummary handler, and the ⑤c seam stays unset). It is
    // constructed BEFORE the WorkerPool so RAII tears the pool down first (Stop + join
    // the worker threads) while the handlers they dispatch to are still alive.
    std::shared_ptr<cortrix::llm::ILlmClient> doc_summary_llm;
    std::unique_ptr<cortrix::doc_summary::F41AsyncWorker> doc_summary_worker;
    if (config.doc_summary_llm.IsConfigured()) {
        cortrix::llm::LlmClientConfig llm_cfg;
        llm_cfg.endpoint = config.doc_summary_llm.base_url.empty()
                               ? "https://api.openai.com/v1"
                               : config.doc_summary_llm.base_url;
        llm_cfg.api_key = config.doc_summary_llm.api_key;
        llm_cfg.default_model = config.doc_summary_llm.model;
        doc_summary_llm = std::make_shared<cortrix::llm::OpenAiLlmClient>(std::move(llm_cfg));
        // D5 wiring (2026-06-26): ResolveDocSummaryConfig leaves llm_model at the
        // kDefaultLlmModel "gpt-4o-mini" default — the generator sends call.model =
        // config_.llm_model, so without this the request ALWAYS asks for gpt-4o-mini,
        // ignoring the configured doc_summary_llm.model. Any non-OpenAI provider (GLM,
        // Claude, local) then returns HTTP 400 and every summary silently fails. Wire
        // the configured model through here (mirrors LlmEnricher, which uses its real
        // config_.model). See qa DEFECT#3.
        auto ds_cfg = cortrix::doc_summary::ResolveDocSummaryConfig(f42_config.get());
        if (!config.doc_summary_llm.model.empty()) {
            ds_cfg.llm_model = config.doc_summary_llm.model;
        }
        doc_summary_worker = std::make_unique<cortrix::doc_summary::F41AsyncWorker>(
            ns_pool, doc_summary_llm, ds_cfg, embedder, assembler, &task_mgr);
    }

    // [addendum §3.7] Enrich-backfill worker (kTaskEnrichBackfill): repairs the
    // chunk-enrichment debt recorded in enrich_state. Constructed BEFORE the
    // WorkerPool (handlers must outlive the worker threads); enabled only when the
    // enricher chain has an available member (no LLM ⇒ no debt concept).
    std::unique_ptr<cortrix::spc::EnrichBackfillWorker> enrich_backfill_worker;
    if (enricher_chain.AnyAvailable()) {
        enrich_backfill_worker = std::make_unique<cortrix::spc::EnrichBackfillWorker>(
            ns_pool, &enricher_chain, embedder, &task_mgr);
    }

    // The pool is the LAST async object constructed → the FIRST destroyed (its dtor
    // Stop()s + joins the workers), so the scheduler + both handlers (doc_processor /
    // doc_summary_worker) it dispatches to outlive the worker threads.
    cortrix::async::WorkerPool worker_pool(&task_scheduler, f42_config.get());
    worker_pool.RegisterHandler(cortrix::async::kTaskDocParse, &doc_processor);
    if (doc_summary_worker) {
        worker_pool.RegisterHandler(cortrix::async::kTaskDocSummary, doc_summary_worker.get());
        CORTRIX_LOG_INFO("main", "F41 doc-summary worker enabled (model={})",
                         config.doc_summary_llm.model);
    } else {
        CORTRIX_LOG_INFO("main", "F41 doc-summary disabled (doc_summary_llm not configured)");
    }
    if (enrich_backfill_worker) {
        worker_pool.RegisterHandler(cortrix::async::kTaskEnrichBackfill,
                                    enrich_backfill_worker.get());
        CORTRIX_LOG_INFO("main", "enrich backfill worker enabled (chain=[{}])",
                         [&] {
                             std::string s;
                             for (const auto& n : enricher_chain.Names()) {
                                 if (!s.empty()) s += ",";
                                 s += cortrix::spc::ChainMemberToken(n);
                             }
                             return s;
                         }());
    } else {
        CORTRIX_LOG_INFO("main", "enrich backfill disabled (no enricher chain member)");
    }

    // [⑤c] Install the F41 doc-summary enqueue seam NOW — the WorkerPool exists (the seam
    // captures &worker_pool) and the pipeline has already moved into spc_mgr, so set it via
    // the spc_mgr proxy (Plan B · §4.1.2: this ordering breaks the doc_processor ↔ spc_mgr ↔
    // seam ↔ pool construction cycle). Only when the F41 worker is registered (LLM
    // configured); else the seam stays unset and OnDocumentWritten is a no-op (→ FTS5).
    if (doc_summary_worker) {
        cortrix::async::TaskScheduler* sched = &task_scheduler;
        cortrix::async::WorkerPool* pool = &worker_pool;
        spc_mgr.SetDocSummaryEnqueue(
            [sched, pool](const cortrix::async::SubmitRequest& req) {
                if (sched->Enqueue(req).ok()) pool->Notify();
            });
    }

    // [addendum §3.7] Retry sweeper: enrich_state IS the durable queue SoT; the
    // sweeper turns due 'pending_retry' rows into kTaskEnrichBackfill tasks.
    // Declared after the WorkerPool (destroyed before it; its dtor joins the
    // sweep thread) and Start()ed only after worker_pool.Start() succeeds below.
    cortrix::spc::EnrichRetrySweeper enrich_sweeper(ns_pool, &task_scheduler,
                                                    &worker_pool, f42_config.get());

    // 8b. ConnectorState holds a single DirWatcherRegistry (fan-out), built
    // lazily by RegisterConnectorRoutes. The config watch_dir is subscribed AFTER
    // the routes build the registry (see just after RegisterConnectorRoutes below) —
    // a fresh NS is created via INSRouter::CreateNamespace inside Subscribe (catalog
    // INSERT + F05 AdmitCreate), not the JSON-only NamespaceManager::Create.
    cortrix::ConnectorState connector_state;
    connector_state.data_dir = config.ns.data_dir;  // for watchers.json persistence

    // 9. UploadHandler (F18a op_logger wired via setter after ObservabilityModule, below)
    cortrix::UploadHandler upload_handler(config.upload, spc_mgr);

    // 10. Query components
    cortrix::IntentClassifier classifier(config.semantic_llm);
    cortrix::RRFFusion fusion;

    // 10b. [D3.5 batch1] P09 tenant/permission/quota services over the F12 catalog.
    // All three borrow the already-open catalog.db handle (they do not own it) and
    // must outlive `server` below. Phase-1 OSS uses UnlimitedPlanProvider (no paid
    // plan tiers yet — matches the P09 integration default); paid quota providers
    // land in Cloud-V1.5.
    cortrix::tenant::TenantService tenant_svc(catalog_db.db());
    cortrix::tenant::PermissionService perm_svc(catalog_db.db());
    cortrix::tenant::QuotaService quota_svc(
        std::make_shared<cortrix::tenant::UnlimitedPlanProvider>(), catalog_db.db());

    // [ARCHITECTURE V6] Wire the runtime namespace-authorization seams into the
    // auth middleware: every namespace gate (WithAuth -> ApiKeyAuth::Authorize,
    // the query single-NS check, GET /namespaces list) now resolves through
    // PermissionService::BatchCheck (ownership + ns_acl) instead of a static
    // per-key allow-list. perm_svc + ns_pool are bootstrap-scoped and outlive the
    // server's request loop. The seams are only invoked when auth is enabled
    // (WithAuth short-circuits to full-access in no-auth dev mode before Authorize).
    auth.SetNamespaceAuthorizer(
        [&perm_svc](const cortrix::AuthContext& ctx, const std::string& ns) -> bool {
            auto checks = perm_svc.BatchCheck(ctx, {ns});
            return !checks.empty() && checks.front().can_read;
        });
    auth.SetNamespaceLister(
        [&perm_svc, &ns_pool](const cortrix::AuthContext& ctx) -> std::vector<std::string> {
            std::vector<std::string> active = ns_pool.GetExplainState().active_namespaces;
            if (active.empty()) return active;
            auto checks = perm_svc.BatchCheck(ctx, active);
            std::vector<std::string> out;
            out.reserve(checks.size());
            for (const auto& c : checks) {
                if (c.can_read) out.push_back(c.ns_id);
            }
            return out;
        });

    // 10c. [D3.5 batch1/F18a] Operation-log query surface. ObservabilityModule owns
    // the CE OperationLogger (over catalog.db, schema migrated at startup above) +
    // the daily UTC-02:00 cleanup sweep. Phase-1 OSS uses an in-memory global config
    // (retention/row-cap defaults); a file-based config lands with full F18a ops.
    auto global_config = std::make_shared<cortrix::InMemoryGlobalConfig>();
    // Seed the agent_llm view from the YAML config (F48 §6.1 priority: env >
    // config.yaml > admin API > defaults). Without this the GET surface shows
    // an empty config even though cortrix-agent runs with the YAML values.
    if (config.agent_llm.IsConfigured()) {
        cortrix::AgentLlmConfig seed;
        seed.provider = config.agent_llm.provider;
        seed.api_key = config.agent_llm.api_key;
        seed.model = config.agent_llm.model;
        seed.base_url = config.agent_llm.base_url;
        global_config->SetAgentLlmConfig(seed);
    }
    // [F10 §3.4 · D3.5] Wire the per-NS CleaningConfig resolver onto the SPC pipeline
    // (proxied through spc_mgr). The NS cleaning_config lives in the catalog (not on
    // the per-request façade), so the pipeline reaches it through this seam: resolve
    // global defaults once from global_config, then per task merge global ←
    // namespaces.cleaning_config (INSRouter::GetNamespace). A missing NS / absent blob
    // / merge failure degrades to the global defaults (never blocks ingest). Wired here
    // (not at the §8 pipeline build) because global_config is constructed in this block.
    const cortrix::spc::CleaningConfig cleaning_global =
        cortrix::spc::LoadGlobalCleaningConfig(global_config.get());
    spc_mgr.SetCleaningConfigResolver(
        [&ns_router, cleaning_global](
            const std::string& ns_id) -> cortrix::spc::CleaningConfig {
            std::string blob;
            if (auto md = ns_router.GetNamespace(ns_id);
                md.ok() && md.value().cleaning_config) {
                blob = *md.value().cleaning_config;
            }
            if (auto resolved =
                    cortrix::spc::ResolveCleaningConfig(cleaning_global, blob);
                resolved.ok()) {
                return resolved.value();
            }
            return cleaning_global;  // bad NS blob → safe global defaults
        });
    cortrix::observability::ObservabilityModule obs_module(catalog_db.db(), global_config);
    upload_handler.SetOperationLogger(obs_module.logger());  // upload-site operation_log
    // [F13 TC4 · A4] agent_trace now lives in the global catalog.db. Build ONE writer over
    // that handle, shared by the F13 90-day retention cleanup here AND the F-path query
    // EngineInstrumentation write side (below, ~line 710). Register the F13 retention
    // callbacks on the shared F18a CleanupScheduler BEFORE Initialize() so its startup
    // catch-up sweep covers agent_trace + interaction_log too (Initialize() registers
    // operation_log then StartScheduler()s the catch-up). agent_trace is single-db
    // (delegated to writer->Cleanup); interaction_log is per-NS, so it gets a fan-out
    // sweeper rather than a single-db RegisterTable callback.
    auto at_writer = std::make_shared<cortrix::agent_trace::AgentTraceWriterImpl>(
        catalog_db.db(), global_config);
    {
        // The registrar is transient: RegisterAgentTrace's callback captures at_writer
        // (shared_ptr), not the registrar. db=nullptr — interaction_log is per-NS (swept
        // below), not reachable through one catalog.db handle.
        cortrix::agent_trace::F13CleanupRegistrar f13_cleanup(at_writer, /*db=*/nullptr,
                                                              global_config);
        f13_cleanup.RegisterAgentTrace(obs_module.scheduler());
    }
    auto il_sweeper = std::make_shared<cortrix::agent_trace::InteractionLogSweeper>(
        &ns_router, &ns_pool, global_config);
    obs_module.scheduler().RegisterTable("interaction_log",
                                         [il_sweeper] { il_sweeper->RunOnce(); });
    // Recurring reclaim of orphaned batch inputs. The startup sweep alone is not a
    // recovery path: it skips files inside its grace window and then does not run
    // again until the next restart, so a fresh orphan could survive indefinitely.
    // Riding the daily 02:00 UTC sweep bounds that without adding a thread.
    // (F42's own TaskCleanupCron is not constructed anywhere in production, so it
    // is not an option here.)
    {
        const std::string batch_tmp = cortrix::server::BatchTempDir(config.ns.data_dir);
        cortrix::async::TaskManager* tm = &task_mgr;
        obs_module.scheduler().RegisterTable("batch_tmp_inputs", [batch_tmp, tm] {
            cortrix::server::SweepOrphanedBatchInputs(batch_tmp, tm);
        });
    }
    obs_module.Initialize();

    // 10d. [D3.5 r2 · Wave P · P2] F16a DB-import assembly. The ConnectionManager
    // (encrypted secret store + ref lifecycle) and ImportManager (validate → fetch
    // → textualize → feed) are built over: the validating QueryExecutor (real PG
    // connect → D3.5, so it returns CONNECTION_FAILED after the §3.4 security gate
    // today), in-memory secret/conn stores + import task store, plus —
    // [P2b] real SPCManager feed (textualized rows → live Pipeline → searchable
    // blocks, F16a §3.2 step 6) + [P2c] real D3 full-overwrite cleanup
    // (RealBlockCleaner deletes the prior import's docs+blocks by source_path prefix
    // over the F05 pool, so a re-import replaces rather than duplicates) + the real
    // F18a operation logger (db_connection_register/revoke + database_import audit,
    // S6). ImportHandler holds shared_ptrs; kept alive in this scope.
    auto import_op_logger = obs_module.logger();
    auto import_conn_mgr = std::make_shared<cortrix::import::ConnectionManager>(
        std::make_shared<cortrix::import::InMemorySecretStore>(),
        std::make_shared<cortrix::import::InMemoryConnectionStore>(),
        import_op_logger);
    auto import_mgr = std::make_shared<cortrix::import::ImportManager>(
        import_conn_mgr,
        std::make_shared<cortrix::import::QueryExecutor>(),
        std::make_shared<cortrix::import::TextSerializer>(),
        std::make_shared<cortrix::import::SpcManagerFeeder>(&spc_mgr),
        std::make_shared<cortrix::import::RealBlockCleaner>(&ns_pool),
        std::make_shared<cortrix::import::InMemoryImportTaskStore>(),
        import_op_logger);
    cortrix::server::ImportHandler import_handler(import_mgr, import_conn_mgr);

    // 11. Create HTTP server and register all routes
    // Rate limiter is declared before the server so the pre-routing
    // handler's capture outlives every in-flight request the server may dispatch.
    cortrix::middleware::RateLimiter rate_limiter;
    cortrix::CortrixHttpServer server(config, auth, ns_mgr);
    server.SetNamespacePool(&ns_pool);       // [wire⑤c] live doc/block counts via per-request façade
    server.SetNamespaceRouter(&ns_router);   // [wire⑤c/F13] namespace-create path
    server.SetOperationLogger(obs_module.logger().get());  // ns_create/ns_delete operation_log
    server.RegisterRoutes();
    // Pre-routing rate-limit gate (per-ip / per-api-key / global token
    // buckets). A denied request short-circuits with 429 + the GEN-Agent error
    // envelope before any route handler runs — defends unauthenticated DDoS /
    // brute force. api_key is read from the same headers as WithAuth so per-key
    // strata key off the real caller; both inputs may be empty (that stratum skips).
    server.server().set_pre_routing_handler(
        [&rate_limiter](const httplib::Request& req, httplib::Response& res) {
            std::string api_key = req.get_header_value("X-API-Key");
            if (api_key.empty()) {
                const std::string ah = req.get_header_value("Authorization");
                const std::string pfx = "Bearer ";
                if (ah.size() > pfx.size() && ah.compare(0, pfx.size(), pfx) == 0)
                    api_key = ah.substr(pfx.size());
            }
            const auto d = rate_limiter.Allow(req.remote_addr, api_key);
            if (!d.allowed) {
                res.status = 429;
                res.set_content(
                    cortrix::middleware::RateLimiter::BuildErrorBody(d).dump(),
                    "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    // P02a web UI: serve the SPA bundle when a dist dir is provided (container
    // entrypoint exports CORTRIX_WEB_UI_DIR=/app/web-ui; unset = headless).
    if (const char* web_ui_dir = std::getenv("CORTRIX_WEB_UI_DIR");
        web_ui_dir != nullptr && web_ui_dir[0] != '\0') {
        server.EnableWebUi(web_ui_dir);
    }

    // F48 P-3: same-origin agent surface — reverse proxy /api/v1/agent/* and
    // /agent/* to the internal agent service (container entrypoint exports
    // CORTRIX_AGENT_BASE_URL=http://127.0.0.1:8000; unset = no agent).
    if (const char* agent_base = std::getenv("CORTRIX_AGENT_BASE_URL");
        agent_base != nullptr && agent_base[0] != '\0') {
        cortrix::RegisterAgentProxyRoutes(server.server(), agent_base);
    }

    // Register document, query, and memory routes on the raw httplib::Server.
    // [gap②] Document upload consults the disk monitor (507 CX_ERR_DISK_FULL at CRIT).
    cortrix::RegisterDocumentRoutes(server.server(), upload_handler, ns_pool, auth,
                                    &f24_disk_monitor);
    // [D3.5-R2 Wave Q1/Q2] F04 cross-NS query: POST /api/v1/query is now the
    // ScatterGather link (namespaces[] array + 8-field meta + child_id/content_hash
    // ResultItems), replacing the MVP single-NS route. The wiring owns the F04 stack
    // (executor engine / reranker / live executor / scatter / handler) and must
    // outlive `server`, so it is a local that lives to the end of this scope.
    // F36 query-variant expansion reuses the semantic LLM (F36 deps: "reuse LLM
    // client"); when semantic_llm is unconfigured, the null client leaves F36 off.
    std::shared_ptr<cortrix::llm::ILlmClient> query_llm;
    if (config.semantic_llm.IsConfigured()) {
        cortrix::llm::LlmClientConfig q_cfg;
        q_cfg.endpoint = config.semantic_llm.base_url.empty()
                             ? "https://api.openai.com/v1"
                             : config.semantic_llm.base_url;
        q_cfg.api_key = config.semantic_llm.api_key;
        q_cfg.default_model = config.semantic_llm.model;
        query_llm = std::make_shared<cortrix::llm::OpenAiLlmClient>(std::move(q_cfg));
    }
    // [F13 · F] Query-path agent_trace write side: one EngineInstrumentation over the
    // shared global at_writer (built at ~line 640). op_logger is now wired
    // (was nullptr) so the query site ALSO writes operation_log on success — the
    // EngineInstrumentation::Record success path emits the F18a entry (action="query").
    // The closure reads trace/session/agent/user from the thread-local ObservabilityContext
    // that WithAuth fills (auth_middleware InstallObservabilityContext).
    auto engine_instr = std::make_shared<cortrix::agent_trace::EngineInstrumentation>(
        at_writer, import_op_logger);
    cortrix::query::CrossNsQueryWiring cross_ns_query_wiring(
        ns_pool, embedder, fusion, perm_svc, &sparse_index_registry, query_llm, engine_instr,
        config.reranker.model_dir, config.query_complexity.model_dir,
        config.retrieval.candidate_multiplier, config.retrieval.max_candidates,
        config.reranker.execution_provider);
    if (!cross_ns_query_wiring.IsReady()) {
        CORTRIX_LOG_ERROR(
            "main", "Reranker initialization failed (configured_ep={}, model_dir={})",
            config.reranker.execution_provider, config.reranker.model_dir);
        return 1;
    }
    cross_ns_query_wiring.Register(server.server(), auth);

    // [M1] MEM02 extraction service: a MemoryQueue draining interactions through
    // per-namespace MemoryExtractors. Reuses the enricher-chain LLM (F03 OpenAiLlmClient
    // shared, MEM02 D1) + obs_module's operation logger. Disabled (no enricher LLM) =>
    // interaction_log only. Started after registration so the route can enqueue.
    cortrix::memory::MemoryExtractorConfig mem02_cfg;
    mem02_cfg.enabled = (enricher_chain_llm != nullptr);
    // The extractor reuses the enricher LLM client, so the model id
    // must follow that provider too - the struct default (gpt-4o-mini) 400s on
    // any non-OpenAI endpoint ("model does not exist").
    if (config.enricher_llm.IsConfigured()) {
        mem02_cfg.llm_model = config.enricher_llm.model;
    }
    // Plumb the extraction LLM timeout from config — it was hardcoded at the
    // 30s struct default with no override path, too short for slow structured
    // extraction plus the contradiction judge's second call (the failure the R4
    // black-box hit as an extraction timeout). enricher_llm.timeout_ms > 0 overrides
    // the 60s default; the extractor reuses that same LLM client.
    if (config.enricher_llm.timeout_ms > 0) {
        mem02_cfg.llm_timeout_ms = config.enricher_llm.timeout_ms;
    }
    cortrix::memory::MemoryQueue::Config mem02_queue_cfg;  // worker_count=4, 1h periodic
    cortrix::memory::MemoryExtractionService mem02_service(
        ns_pool, enricher_chain_llm, embedder, obs_module.logger(), mem02_cfg,
        mem02_queue_cfg);
    cortrix::MemoryServices mem_services;
    mem_services.extraction = &mem02_service;
    mem_services.op_logger = obs_module.logger();  // F18a audit for MEM03 CRUD
    mem02_service.Start();

    // [D3.5 r2 · Wave P · P3] POST /api/v1/documents/batch — mount the TD-F42-BULK
    // batch submit route. The service fans each doc out through the frozen F42
    // scheduler (F42TaskSubmitterAdapter::Submit → TaskScheduler::Enqueue + a worker
    // Notify). Default BatchLimits (100 docs / 100MB / 10MB-per-doc). Both the
    // adapter + the service outlive `server` (RAII order: server destructs first).
    cortrix::server::F42TaskSubmitterAdapter batch_submitter(&task_scheduler, &worker_pool);
    cortrix::server::BatchSubmitService batch_service(&batch_submitter);
    // [P3b] Materialize each batch doc's inline content under the data dir so the
    // F42 doc-parse worker (DocumentProcessor → ParseDocument(filepath)) reads real
    // files — batch submissions now truly enter the pipeline + land as blocks.
    batch_service.SetMaterializeDir(cortrix::server::BatchTempDir(config.ns.data_dir));
    // Reclaim materialized inputs no live task still needs, BEFORE the route is
    // mounted (nothing is in flight yet, so the live-set read cannot race a new
    // submission). This is the required backstop for the terminal release above:
    // SweepZombies/SweepTimedOut end tasks with a bulk UPDATE that never reaches a
    // handler, and a crash can leave a file whose task never terminated at all.
    if (auto swept = cortrix::server::SweepOrphanedBatchInputs(
            cortrix::server::BatchTempDir(config.ns.data_dir), &task_mgr);
        !swept.ok()) {
        spdlog::warn("batch temp sweep skipped: {}", swept.status().message());
    }
    // Ownership hand-back for materialized inputs that end up with no owner:
    //   - the scheduler reports a debounce merge/refresh (it alone sees those);
    //   - the batch service reports a per-doc submit that produced no task.
    // Both funnel into the same containment + live-reference checked release.
    {
        const std::string managed_dir = cortrix::server::BatchTempDir(config.ns.data_dir);
        cortrix::async::TaskManager* tm = &task_mgr;
        task_scheduler.SetUnadoptedInputReleaser(
            [managed_dir, tm](const std::string& path, const std::string& owner_task_id) {
                cortrix::async::ReleaseManagedPath(managed_dir, path, owner_task_id, tm);
            });
        batch_service.SetInputReleaser([managed_dir, tm](const std::string& path) {
            cortrix::async::ReleaseManagedPath(managed_dir, path, /*exclude=*/"", tm);
        });
    }
    cortrix::RegisterBatchRoutes(server.server(), batch_service, auth);
    // [D3.5 r2 · Wave S routes] Flat design-surface /documents family. Mounts the
    // openapi/SDK/MCP shape (POST/GET/GET{id}/DELETE{id} + tasks progress/cancel) on
    // top of the live per-NS handlers while keeping nested routes. POST reuses
    // batch_service as a batch-of-1; the F42 task progress/cancel bodies come from a
    // DocumentTaskHandler over the already-wired scheduler/task_mgr/worker_pool (must
    // outlive `server`, so declared here at end-of-scope).
    cortrix::async::DocumentTaskHandler doc_task_handler(
        &task_scheduler, &task_mgr, &worker_pool, f42_config.get(),
        cortrix::server::BatchTempDir(config.ns.data_dir));
    // [F41 · A6] GET /api/v1/documents/discover — doc-summary granularity recall (HNSW
    // over doc_summary blocks + FTS5 fallback). MUST be mounted BEFORE RegisterFlatDocumentRoutes
    // below, whose GET /documents/{id} catch-all would otherwise swallow the literal
    // "discover" path segment. The DocSummaryConfig is resolved once from f42_config — the
    // same source the F41AsyncWorker write side uses (~line 560), so read and write agree.
    {
        auto ds_cfg = cortrix::doc_summary::ResolveDocSummaryConfig(f42_config.get());
        server.server().Get(
            "/api/v1/documents/discover",
            cortrix::WithAuth(auth, cortrix::kPermRead,
                [&ns_pool, &embedder, ds_cfg](const httplib::Request& req,
                                              httplib::Response& res,
                                              const cortrix::RequestContext& rctx) {
                    cortrix::doc_summary::HandleDocumentsDiscover(
                        req, res, rctx, ns_pool, embedder, ds_cfg);
                }));
    }
    cortrix::RegisterFlatDocumentRoutes(server.server(), ns_pool, upload_handler,
                                        batch_service, doc_task_handler, auth);
    cortrix::RegisterMemoryRoutes(server.server(), auth, ns_pool, spc_mgr,
                                  embedder, classifier, fusion, config.memory, mem_services);
    // [F13 TC4] Agent-observability routes split across two DBs: agent_trace moved back to
    // the global catalog.db (a session may span namespaces -> trace read is GLOBAL, no
    // ?namespace=), while interaction_log + interaction_sources stay per-NS memory.db.
    // RegisterTracesRoutesGlobal builds its own writer + the section 8.1 cross-NS owner
    // resolver internally (global agent_trace -> namespace_id -> per-NS memory.db user_id
    // via pool). interaction reads stay per-NS (?namespace= / namespace_id selector).
    cortrix::RegisterTracesRoutesGlobal(server.server(), catalog_db.db(), ns_pool,
                                        global_config, auth);
    cortrix::RegisterInteractionsRoutesPerNs(server.server(), ns_pool, auth);
    cortrix::RegisterConnectorRoutes(server.server(), connector_state, ns_pool, ns_router, spc_mgr, auth);
    // The registry is now built (RegisterConnectorRoutes lazily creates it).
    // Subscribe the config watch_dir as a fan-out watcher over its namespace. Subscribe
    // creates a fresh NS via INSRouter (catalog INSERT + F05 AdmitCreate) so the importer
    // can Acquire it; autostart is deferred to StartAll() below.
    if (config.watch_dir.watch_enabled && !config.watch_dir.data_dir.empty() &&
        connector_state.registry) {
        connector_state.registry->Subscribe(config.watch_dir.data_dir,
                                             {config.watch_dir.namespace_name},
                                             /*recursive=*/true, /*autostart=*/false);
    }
    // [D3.5 r2 · Wave S routes] /watch aliases (openapi/SDK/MCP fan-out shape) mapped
    // onto the same connector registry (POST = one Subscribe(path, target_namespaces)).
    cortrix::RegisterWatchAliasRoutes(server.server(), connector_state, ns_pool, ns_router, spc_mgr, auth);
    // [D3.5 batch1] P09 tenant/permission/quota REST (15 endpoints, sec 4.5). In CE
    // no-auth mode WithAuth grants admin so the admin-gated endpoints are reachable
    // for local integration; full multi-tenant enforcement activates with P08 auth.
    cortrix::RegisterTenantRoutes(server.server(), tenant_svc, perm_svc, quota_svc, auth);
    // [D3.5 batch1/F18a] operation-log query route (GET /api/v1/operations, read-only).
    cortrix::RegisterOperationsRoutes(server.server(), *obs_module.logger(), auth);
    // [addendum §3.7 G4] enrichment-backfill ops surface (admin; audit + enqueue).
    {
        std::vector<std::string> enrich_chain_tokens;
        if (enrich_backfill_worker) {
            for (const auto& n : enricher_chain.Names()) {
                enrich_chain_tokens.push_back(cortrix::spc::ChainMemberToken(n));
            }
        }
        cortrix::RegisterEnrichRoutes(server.server(), ns_pool, auth, &enrich_sweeper,
                                      std::move(enrich_chain_tokens),
                                      obs_module.logger().get());
    }
    // [D3.5 r2 · Wave P · P1] P08-CE auth REST surface. Bootstrap GET+POST are NOT
    // WithAuth-wrapped (on first run no key exists; the bootstrap token IS the
    // credential) — AdminGuard's loopback IP filter on /api/v1/admin/* is the
    // ambient gate. /auth/api-keys CRUD is WithAuth(admin).
    cortrix::RegisterBootstrapRoutes(server.server(), bootstrap_handler);
    cortrix::RegisterApiKeyRoutes(server.server(), api_key_service, auth);
    // CE Web UI session probe (GET /auth/me): auth-disabled → 200 single-operator so
    // the self-hosted UI is usable out-of-the-box; auth-enabled → 401 (login). The
    // JWT /auth/me is cloud-enterprise; this CE shim avoids the no-auth login dead-end.
    cortrix::RegisterAuthSessionRoute(server.server(), auth);
    // [FA1 R11] P08 §2.13-bis admin/users 5 endpoints (admin-gated; mutations write
    // operation_log) + §2.11 JWT secret rotation endpoint. AdminGuard adds the
    // loopback IP layer on the /api/v1/admin/* prefix.
    cortrix::RegisterAdminUsersRoutes(server.server(), admin_users_service, auth,
                                      obs_module.logger().get());
    cortrix::RegisterJwtRotateRoute(server.server(), jwt_secret_service, auth);
    // [D3.5 r2 · Wave P · P2] F16a DB-import 6-endpoint surface (admin-gated;
    // /admin/db-connections* under AdminGuard, /import/* Layer-2 only per §6.1).
    cortrix::RegisterImportRoutes(server.server(), import_handler, auth);
    // [D3.5 r2 · Wave P · P4] F48 §6.3 agent_llm_config GET (read) / PUT (admin).
    // Backed by the shared global_config (api_key encrypted at rest). The PUT path
    // is not under AdminGuard, so it enforces admin in-handler.
    cortrix::RegisterSystemConfigRoutes(server.server(), *global_config, auth);
    // [OPEN-2] GC + maintenance ops routes (/api/v1/gc/*, /api/v1/maintenance/*).
    cortrix::RegisterGcRoutes(server.server(), gc_manager, gc_doc_sweeper, auth);

    // Install AdminGuard pre-routing filter: admin paths are restricted to
    // the configured client-IP whitelist (loopback-only by default). Runs before
    // route dispatch / API-key auth. Config comes from CORTRIX_ADMIN_BIND.
    cortrix::middleware::InstallAdminGuard(server.server(),
        {config.security.admin_bind, config.security.allow_public_admin});
    // Deployment wiring (Wave D-R1, additive). The DiskMonitor itself is
    // constructed earlier (6b) so the SPC admission gate + upload route hold it.
    // Dual health endpoint (F24 main impl): /api/v1/system/health/{live,ready}.
    // /ready aggregates the F20 ReadinessRegistry (unified
    // abstraction — replaced F24's standalone HealthProviders). Register the readiness
    // components here (design F20 §8.3/§8.4). Every probe below reflects REAL runtime state
    // — none is a constant-200 stub (a false-ready probe is worse than an honest deferral,
    // because it makes K8s route traffic to a not-yet-ready pod). Probes are lambdas
    // evaluated per request, so they observe live state (e.g. before spc_mgr.Start() the
    // worker pool reports 0 workers → spc_pipeline not_ready → /ready returns 503, as
    // designed for the "warming up" window).
    cortrix::health::ReadinessRegistry readiness;

    // disk (real): DiskMonitor admission gate.
    readiness.Register("disk", [&f24_disk_monitor]() {
        cortrix::health::ComponentReadiness r;
        r.ready = !f24_disk_monitor.ShouldRejectWrites();
        r.detail["stage"] = std::string(cortrix::deploy::ToString(f24_disk_monitor.Usage().stage));
        return r;
    });

    // catalog (real): the global catalog.db handle is non-null only after a successful
    // CatalogDb::Open() above (which also ran the schema migration). That is the
    // startup-completion signal design §8.4 refers to as "catalog.db ready". The per-NS F12
    // IBloomFilter::IsReady() lives on each namespace's BloomFilter (no global accessor in
    // bootstrap scope — namespaces are created lazily via INSRouter), so bloom_filter_ready
    // is reported per-NS through that Feature's own surface, not aggregated here; the
    // catalog component honestly probes only the global catalog.db open state.
    readiness.Register("catalog", [&catalog_db]() {
        cortrix::health::ComponentReadiness r;
        r.ready = (catalog_db.db() != nullptr);
        r.detail["catalog_db_open"] = r.ready;
        return r;
    });

    // secret_provider (real, F20-6): construct the configured provider (env in V1.0) and
    // register the F20 adapter, which reports ready iff ISecretProvider::IsHealthy() and
    // surfaces provider_type.
    auto secret_provider = cortrix::security::CreateSecretProvider();
    readiness.Register(
        std::make_shared<cortrix::security::SecretProviderReadiness>(secret_provider));

    // spc_pipeline (real, F06/F03): ready iff the WorkerPool has live worker threads
    // (worker_count() > 0 only after WorkerPool::Start(), driven by spc_mgr.Start() below).
    // queue_depth = SPCManager::QueueSize() — design §8.3 example field. Captures the two
    // objects by reference so the probe sees the current (post-Start) state.
    readiness.Register("spc_pipeline", [&worker_pool, &spc_mgr]() {
        cortrix::health::ComponentReadiness r;
        const int workers = worker_pool.worker_count();
        r.ready = (workers > 0);
        r.detail["workers"] = workers;
        r.detail["queue_depth"] = static_cast<int64_t>(spc_mgr.QueueSize());
        if (!r.ready) {
            r.detail["reason"] = "workers_not_started";
        }
        return r;
    });

    auto build_ep_readiness = [](const cortrix::ml::ExecutionProviderRuntimeState& state,
                                 bool model_configured) {
        cortrix::health::ComponentReadiness r;
        r.ready = state.ready;
        r.detail["configured_ep"] = state.configured_ep;
        r.detail["active_ep"] = state.active_ep;
        r.detail["preferred_ep"] = state.preferred_ep;
        r.detail["model_configured"] = model_configured;
        r.detail["fallback"] = state.fallback;
        r.detail["policy_mismatch"] = state.policy_mismatch;
        if (!r.ready) {
            r.detail["reason"] = "execution_provider_policy_mismatch";
        }
        return r;
    };

    const bool embedding_model_configured = !config.embedding.model_path.empty();
    readiness.Register(
        "embedding_execution_provider",
        [&embedder, embedding_model_configured, build_ep_readiness]() {
            return build_ep_readiness(
                cortrix::ml::EvaluateExecutionProviderRuntimeState(
                    embedder.configured_ep(), embedder.active_ep(),
                    embedding_model_configured),
                embedding_model_configured);
        });

    const bool reranker_model_configured = !config.reranker.model_dir.empty();
    readiness.Register(
        "reranker_execution_provider",
        [reranker_model_configured, build_ep_readiness]() {
            auto& metrics = cortrix::reranker::RerankerMetrics::Instance();
            return build_ep_readiness(
                cortrix::ml::EvaluateExecutionProviderRuntimeState(
                    metrics.ConfiguredEp(), metrics.ActiveEp(),
                    reranker_model_configured),
                reranker_model_configured);
        });

    // Docker QuickStart writes this marker only after it has ingested the
    // bundled fixture and completed a source-backed query with real embedding
    // and reranker invocation counters. Native and non-QuickStart deployments
    // do not set the environment variable, so their readiness contract is
    // unchanged.
    const char* quickstart_ready_file_env =
        std::getenv("CORTRIX_QUICKSTART_READY_FILE");
    if (quickstart_ready_file_env != nullptr && quickstart_ready_file_env[0] != '\0') {
        const std::filesystem::path quickstart_ready_file(quickstart_ready_file_env);
        readiness.Register("quickstart_demo", [quickstart_ready_file]() {
            cortrix::health::ComponentReadiness r;
            std::error_code ec;
            const auto marker_status =
                std::filesystem::symlink_status(quickstart_ready_file, ec);
            if (ec == std::make_error_code(std::errc::no_such_file_or_directory)) {
                ec.clear();
            }
            r.ready = !ec && std::filesystem::is_regular_file(marker_status);
            r.detail["proof_file_present"] = r.ready;
            if (ec) {
                r.detail["reason"] = "proof_file_check_failed";
            } else if (!r.ready) {
                r.detail["reason"] = "source_backed_demo_pending";
            }
            return r;
        });
    }

    // vector_index + memory_store (MEM): honest deferred. Both are PER-NAMESPACE in
    // V1.0 — the P-HNSW index (model load + WAL replay) and memory.db are created lazily
    // when a namespace is first used, so there is no single global "loaded/initialized"
    // signal to probe at the process level. Rather than stub a false 200, we do NOT register
    // them here (so they are absent from the /ready body, not falsely "ok"). Per-NS readiness
    // is exposed through F01 / MEM surfaces; a global aggregate is a Phase-1.5 item once a
    // namespace registry with readiness rollup exists.
    cortrix::deploy::RegisterHealthRoutes(
        server.server(), readiness,
        []() { return g_shutting_down.load(); },
        cortrix::kCortrixVersion);
    // OpenMetrics endpoint on a loopback-only, separate port. The MetricsServer
    // always renders the DeployMetrics gauges (disk/shutdown/uptime/build_info); the
    // §5.3 subsystem recorders are AddSource()'d below.
    cortrix::deploy::DeployMetrics::Instance().SetBuildInfo(cortrix::kCortrixVersion, "", "");
    cortrix::deploy::DeployMetrics::Instance().MarkStart();
    int metrics_port = 9091;
    if (const char* value = std::getenv("CORTRIX_METRICS_PORT"); value && *value) {
        try {
            std::size_t parsed = 0;
            const int candidate = std::stoi(value, &parsed);
            if (parsed != std::string(value).size() || candidate <= 0 || candidate > 65535) {
                throw std::out_of_range("metrics port");
            }
            metrics_port = candidate;
        } catch (const std::exception&) {
            CORTRIX_LOG_ERROR(
                "metrics", "CORTRIX_METRICS_PORT must be an integer from 1 to 65535");
            return 1;
        }
    }
    auto f24_metrics_server = cortrix::deploy::RegisterMetricsServer(metrics_port);
    if (f24_metrics_server) {
        // [D3.5 wire · gap④] Aggregate the §5.3 subsystem metric recorders into the
        // /metrics body. Each is a process-wide singleton (MET round standalone recorders);
        // AddSource captures its render fn, concatenated by MetricsServer::RenderAll().
        // The catalog IBloomFilter source needs a BF accessor → deferred alongside the
        // health catalog component (F20-S5).
        auto& ms = *f24_metrics_server;
        ms.AddSource([] { return cortrix::async::F42Metrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::query::ScatterMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::query::RagFusionMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::query::QueryRouterMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::scoring::ScoringMetrics::Instance().Render(); });
        ms.AddSource([] { return cortrix::resource::NsPoolMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::reranker::RerankerMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::onnx::OnnxMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::spc::EnricherMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::spc::ContextualRetrievalMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::spc::HypeMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::retrieval::CragMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::retrieval::SparseMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::metadata::MetadataMetrics::Instance().Render(); });
        ms.AddSource([] { return cortrix::doc_summary::DocSummaryMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::import::ImportMetrics::Instance().Render(); });
        ms.AddSource([] { return cortrix::memory::Mem02Metrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::memory::transparency::Mem03Metrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::memory::immunity::Mem04Metrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::memory::Mem05Metrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::agent_trace::AgentTraceMetrics::Instance().RenderOpenMetrics(); });
        ms.AddSource([] { return cortrix::observability::OplogMetrics::Instance().RenderOpenMetrics(); });
        CORTRIX_LOG_INFO("metrics", "OpenMetrics endpoint on :{}/metrics (22 subsystem recorders)",
                         metrics_port);
    } else {
        CORTRIX_LOG_WARN("metrics", "OpenMetrics endpoint failed to bind :{} (port in use?)",
                         metrics_port);
    }
    // [D3.5 wire · gap⑤ · plan B, F24 §7.2.bis] Graceful-shutdown coordinator.
    // The signal stays with the existing SignalHandler (g_shutting_down +
    // server.Stop()); Run() executes on the MAIN thread after server.Start()
    // returns — zero races with the RAII teardown (InstallGracefulShutdown's
    // signal-takeover waiter is deliberately NOT used here). Drain scope = the
    // in-memory SPC queue only: F42 tasks live in tasks.db (status=queued is
    // re-picked by Dequeue on next start); flush_wal / persist_memory stay null —
    // P-HNSW WAL (fdatasync per write) + SQLite WAL are already crash-safe.
    cortrix::deploy::ShutdownConfig gs_cfg;
    gs_cfg.data_dir = config.ns.data_dir;
    cortrix::deploy::GracefulShutdown graceful(gs_cfg, {
        /*close_http=*/ nullptr,  // server already stopped before Run() (plan B)
        /*drain_tasks=*/ [&worker_pool, &spc_mgr, &enrich_sweeper](std::chrono::steady_clock::time_point) {
            // Order matters (original Plan B teardown): the enrich sweeper stops
            // first (no new backfill enqueues), then the F42 pool stops so
            // doc_processor→spc_mgr quiesces, then the SPC drain joins its workers
            // (each finishes at most its in-hand task) and exports the untouched
            // queue remainder for .pending_tasks.json.
            enrich_sweeper.Stop();
            worker_pool.Stop();
            std::vector<cortrix::deploy::PendingTask> leftovers;
            for (const auto& t : spc_mgr.DrainRemainingTasks()) {
                cortrix::deploy::PendingTask pt;
                pt.namespace_id = t->namespace_name;
                pt.filename = t->source_path;
                // http_upload re-extracts from the blob store on resume; watcher
                // tasks keep their on-disk source path.
                pt.filepath = (t->source_type == "http_upload") ? "" : t->source_path;
                pt.doc_id = t->doc_id;
                pt.content_hash = t->content_hash;
                pt.task_type = 1;  // doc parse
                leftovers.push_back(std::move(pt));
            }
            return leftovers;
        },
        /*flush_wal=*/ nullptr,
        /*persist_memory=*/ nullptr,
        /*persist_file=*/ nullptr,  // default atomic write
    });

    // 11b. Embedder extension point (interface-extension isolation): extra
    // routes/background services assembled on top of the stock server. The
    // stock binary passes no hooks, so this is a no-op for cortrix-server.
    ServerContext extension_ctx{server, config, ns_pool, spc_mgr, auth};
    if (extensions.on_assembled) {
        extensions.on_assembled(extension_ctx);
    }

    // 12. Register signal handlers
    g_server = &server;
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // 13. Start SPC workers, the F42 async worker pool, and the HTTP server.
    spc_mgr.Start();
    if (cortrix::Status ws = worker_pool.Start(); !ws.ok()) {
        CORTRIX_LOG_ERROR("main", "F42 WorkerPool start failed: {}", ws.message());
        spc_mgr.Stop();
        cortrix::ShutdownLogging();
        return 1;
    }
    CORTRIX_LOG_INFO("main", "SPC + F42 workers started ({} SPC threads)", config.spc.worker_count);
    if (enrich_backfill_worker) {
        enrich_sweeper.Start();
        // D7b: report the EFFECTIVE interval (KV-seeded knob or default), not the
        // compile-time constant — the constant masked live knob state twice.
        CORTRIX_LOG_INFO("main", "enrich retry sweeper started ({}s interval)",
                         enrich_sweeper.SweepIntervalSec());
    }

    // [OPEN-2] Launch the background GC thread (no-op when gc.enabled=false).
    gc_thread.Start();
    if (gc_thread.started()) {
        CORTRIX_LOG_INFO("main", "GC background thread started ({}h scan interval)",
                         config.gc.scan_interval_hours);
    }

    // [gap⑤] Resume SPC tasks persisted by a prior forced shutdown
    // (.pending_tasks.json, F24 §7.2). Lossy PendingTask fields (mime/metadata/
    // size) are re-hydrated from the documents row; vanished namespace/doc → skip.
    {
        int resumed = graceful.ResumeOnStartup(
            [&ns_pool, &spc_mgr](const cortrix::async::SubmitRequest& req) {
                cortrix::resource::NamespaceFacade facade(ns_pool, req.namespace_id);
                if (!facade.Acquire().ok()) return;  // namespace gone → skip
                cortrix::CortrixDoc doc;
                if (facade.store().doc_get(req.doc_id, doc) != 0) return;  // doc gone → skip
                auto task = std::make_shared<cortrix::SPCTask>();
                task->doc_id = req.doc_id;
                task->namespace_name = req.namespace_id;
                task->source_path = doc.source_path.empty() ? req.filename : doc.source_path;
                task->source_type = doc.source_type;
                task->content_hash = req.content_hash;
                task->mime_type = doc.mime_type;
                task->file_size = doc.file_size;
                task->metadata_json = doc.metadata_json;
                spc_mgr.Submit(task);
            });
        if (resumed > 0) {
            CORTRIX_LOG_INFO("main", "Resumed {} SPC tasks from a prior shutdown", resumed);
        }
    }

    // 13b. Start all subscribed directory watchers (fan-out registry; one OS
    // watcher per directory, autostart was deferred to here at Subscribe time).
    if (connector_state.registry) {
        connector_state.registry->StartAll();
        CORTRIX_LOG_INFO("main", "Directory watcher registry started");
    }

    CORTRIX_LOG_INFO("main", "Cortrix server v{} ready on {}:{}",
                    cortrix::kCortrixVersion, config.server.host, config.server.port);
    s = server.Start();
    if (!s.ok()) {
        CORTRIX_LOG_ERROR("main", "Server failed: {}", s.message());
        spc_mgr.Stop();
        cortrix::ShutdownLogging();
        return 1;
    }

    // Graceful shutdown — stop all directory watchers. The registry serializes
    // its own teardown (its dtor also StopAll()s, but do it explicitly before the
    // catalog/pool handles are torn down).
    if (connector_state.registry) {
        connector_state.registry->StopAll();
    }
    // [OPEN-2] stop the GC thread (also RAII-handled, but explicit so an in-flight
    // sweep finishes + the thread joins before the catalog handle is torn down).
    gc_thread.Stop();
    // stop the deployment background services (also handled by RAII, but
    // stop explicitly so scrapes/disk checks quiesce before final teardown).
    if (f24_metrics_server) f24_metrics_server->Stop();
    f24_disk_monitor.Stop();
    // Embedder extension teardown (mirror of on_assembled) — before core drain.
    if (extensions.on_shutdown) {
        extensions.on_shutdown();
    }
    // [gap⑤ · plan B] Ordered graceful shutdown on the MAIN thread (F24 §7.2.bis,
    // replaces the bare worker_pool.Stop() + spc_mgr.Stop() pair — Run()'s drain
    // hook performs both, preserving the Plan B order: F42 pool first so
    // doc_processor→spc_mgr quiesces, then the SPC drain joins workers and persists
    // the untouched queue remainder to .pending_tasks.json for next-start resume.
    // F42 DB tasks need no persist: status=queued is re-picked by Dequeue.)
    cortrix::deploy::ShutdownStatus final_status = graceful.Run();
    if (final_status == cortrix::deploy::ShutdownStatus::kForced) {
        CORTRIX_LOG_WARN("main", "Graceful shutdown persisted pending SPC tasks for resume");
    }
    cortrix::ShutdownLogging();
    return 0;
}

}  // namespace cortrix::server
