#include "cortrix/server/bootstrap.h"

#include "cortrix/config/config.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

#include <sqlite3.h>

#include "cortrix/logging/logging.h"
#include "cortrix/common/version.h"  // [P5] single SoT for the version string
#include "cortrix/auth/api_key_auth.h"
// [D3.5 r2 · Wave P · P1] P08-CE auth: platform.db + bootstrap + API Keys REST.
#include "cortrix/auth/platform_db.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/bootstrap_handler.h"
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
#include "cortrix/spc/contextual_enricher.h"       // I2 ContextualRetrievalEnricher / ResolveContextualConfig
#include "cortrix/spc/hype_enricher.h"             // I3 HyPEEnricher / HyPEConfig
#include "cortrix/llm/openai_client.h"             // OpenAiLlmClient (shared enricher LLM)
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/spc_manager.h"
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
#include "cortrix/server/http_server.h"
// [F20] security hardening
#include "cortrix/middleware/admin_guard.h"
#include "cortrix/logging/sanitizer.h"
// [F24] deployment: dual health endpoint + OpenMetrics :9091 + disk monitor + graceful shutdown
#include "cortrix/deploy/graceful_shutdown.h"
#include "cortrix/resource/namespace_facade.h"   // [gap⑤] resume re-hydrates tasks from the doc row
#include "cortrix/deploy/health_routes.h"
#include "cortrix/health/readiness.h"  // F20 readiness contract — components registered at wiring
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

    // 2. Initialize logging
    cortrix::InitLogging(config.log);
    CORTRIX_LOG_INFO("main", "Cortrix server starting...");

    // [F20] Load log sanitization rules + validate admin access config.
    // ValidateConfigOrAbort() is the V3-E-06 hard-fail: CORTRIX_ADMIN_BIND=0.0.0.0
    // without CORTRIX_ALLOW_PUBLIC_ADMIN=true aborts startup here.
    cortrix::logging::LogSanitizer::Initialize("/app/config/sensitive_fields.yaml");
    cortrix::middleware::AdminGuard::ValidateConfigOrAbort();

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
    if (auto token = bootstrap_handler.GenerateToken(); token.ok() && !token.value().empty()) {
        // First start: surface the one-time setup URL on stdout (P08 §2.13.1). The
        // token lives in memory only; visiting either bootstrap endpoint mints the
        // admin API Key and invalidates it. AdminGuard restricts the path to
        // loopback by default, so this is local-operator-only.
        std::printf(
            "\n>>> First-time admin setup URL (60s, single-use):\n"
            ">>>   http://localhost:8420/api/v1/admin/bootstrap?token=%s\n"
            ">>>\n"
            ">>> Visit this URL in your browser within 60 seconds to obtain the "
            "admin API Key.\n\n",
            token.value().c_str());
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
        // [D3.5 batch1/F18a] migrate the operation_log (F18a) schema into the
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
    embedder.set_gpu_provider(config.embedding.gpu_provider);
    // [F22 D3.5 · A3] Fail-fast ONNX startup validation BEFORE the embedder initializes:
    // an ABI-mismatched runtime, or an out-of-range opset on a PRESENT model, aborts the
    // process here (in the operator's startup window) rather than at the first inference.
    // skip_missing_models=true preserves the OnnxEmbedder stub-fallback contract (a dev
    // box without the model files still starts); only a present+incompatible model (or a
    // runtime ABI mismatch) is the hard stop. F02 reranker / F40 sparse models
    // self-register in CollectRegisteredOnnxModels only once their files exist.
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
        CORTRIX_LOG_WARN("main", "OnnxEmbedder init failed (stub mode): {}", s.message());
    } else {
        CORTRIX_LOG_INFO("main", "OnnxEmbedder initialized (real_model={})", embedder.is_real_model());
    }

    // Auto-resolve worker_count=0: 2 workers in both GPU and CPU-only environments.
    // HfTokenizer::Encode() is const (read-only, thread-safe); Ort::Session::Run()
    // is thread-safe per ONNX Runtime docs. CoreML serializes concurrent Run() calls
    // internally. For file pipelines, parse/OCR (not embedding) is usually the bottleneck,
    // so 2 workers provide real parallelism on IO/parse/chunk stages.
    if (config.spc.worker_count == 0) {
        config.spc.worker_count = embedder.is_coreml_active() ? 2 : 2;
        CORTRIX_LOG_INFO("main", "worker_count auto={} (coreml_active={})",
                        config.spc.worker_count, embedder.is_coreml_active());
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
        CORTRIX_LOG_INFO("main", "F03 enricher enabled (model={})", config.enricher_llm.model);
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
                auto f35 = std::make_shared<cortrix::spc::ContextualRetrievalEnricher>(
                    cortrix::spc::ResolveContextualConfig(enricher_chain_config.get()),
                    enricher_chain_llm,
                    std::make_shared<cortrix::spc::ContextualOnnxEmbedder>(embedder_alias));
                enricher_chain.Append(std::move(f35));
            } else if (tok == "f38" && enricher_chain_llm) {
                cortrix::spc::HyPEConfig hype_cfg;  // K=3 default; parent_text bound in pipeline
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
    f42_config->Set("f06.parser_max_concurrent", "4");

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

    cortrix::async::DocumentProcessor doc_processor(
        &task_mgr, &f06_factory, f42_config.get(), nullptr, &spc_mgr);

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
        doc_summary_worker = std::make_unique<cortrix::doc_summary::F41AsyncWorker>(
            ns_pool, doc_summary_llm,
            cortrix::doc_summary::ResolveDocSummaryConfig(f42_config.get()),
            embedder, assembler, &task_mgr);
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

    // 8b. ConnectorState (multi-watcher; optionally seed from config)
    cortrix::ConnectorState connector_state;
    connector_state.data_dir = config.ns.data_dir;  // for watchers.json persistence
    if (config.watch_dir.watch_enabled && !config.watch_dir.data_dir.empty()) {
        // Auto-create namespace if it doesn't exist yet (idempotent)
        ns_mgr.Create(config.watch_dir.namespace_name);  // ignore AlreadyExists
        unsigned h = static_cast<unsigned>(
            std::hash<std::string>{}(config.watch_dir.data_dir) & 0xFFFFFFFFu);
        char idbuf[9]; snprintf(idbuf, sizeof(idbuf), "%08x", h);
        auto imp = std::make_unique<cortrix::DirectoryImporter>(
            config.watch_dir, ns_pool, spc_mgr);
        connector_state.watchers.push_back({std::string(idbuf), std::move(imp)});
    }

    // 9. UploadHandler
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
    cortrix::observability::ObservabilityModule obs_module(catalog_db.db(), global_config);
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
    cortrix::CortrixHttpServer server(config, auth, ns_mgr);
    server.SetNamespacePool(&ns_pool);       // [wire⑤c] live doc/block counts via per-request façade
    server.SetNamespaceRouter(&ns_router);   // [wire⑤c/F13] namespace-create path
    server.RegisterRoutes();

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
    // shared global at_writer (built at ~line 640). op_logger=nullptr — the query path
    // already audits via F18a, so tracing only here avoids a double operation_log write.
    // The closure reads trace/session/agent/user from the thread-local ObservabilityContext
    // that WithAuth fills (auth_middleware InstallObservabilityContext).
    auto engine_instr = std::make_shared<cortrix::agent_trace::EngineInstrumentation>(
        at_writer, /*op_logger=*/nullptr);
    cortrix::query::CrossNsQueryWiring cross_ns_query_wiring(
        ns_pool, embedder, fusion, perm_svc, &sparse_index_registry, query_llm, engine_instr);
    cross_ns_query_wiring.Register(server.server(), auth);

    // [M1] MEM02 extraction service: a MemoryQueue draining interactions through
    // per-namespace MemoryExtractors. Reuses the enricher-chain LLM (F03 OpenAiLlmClient
    // shared, MEM02 D1) + obs_module's operation logger. Disabled (no enricher LLM) =>
    // interaction_log only. Started after registration so the route can enqueue.
    cortrix::memory::MemoryExtractorConfig mem02_cfg;
    mem02_cfg.enabled = (enricher_chain_llm != nullptr);
    // The extractor reuses the enricher LLM client (MEM02 D1), so the model id
    // must follow that provider too - the struct default (gpt-4o-mini) 400s on
    // any non-OpenAI endpoint ("model does not exist").
    if (config.enricher_llm.IsConfigured()) {
        mem02_cfg.llm_model = config.enricher_llm.model;
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
    batch_service.SetMaterializeDir(config.ns.data_dir + "/batch_tmp");
    cortrix::RegisterBatchRoutes(server.server(), batch_service, auth);
    // [D3.5 r2 · Wave S routes] Flat design-surface /documents family. Mounts the
    // openapi/SDK/MCP shape (POST/GET/GET{id}/DELETE{id} + tasks progress/cancel) on
    // top of the live per-NS handlers (Derek approach A; nested routes stay). POST reuses
    // batch_service as a batch-of-1; the F42 task progress/cancel bodies come from a
    // DocumentTaskHandler over the already-wired scheduler/task_mgr/worker_pool (must
    // outlive `server`, so declared here at end-of-scope).
    cortrix::async::DocumentTaskHandler doc_task_handler(
        &task_scheduler, &task_mgr, &worker_pool, f42_config.get());
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
    cortrix::RegisterConnectorRoutes(server.server(), connector_state, ns_pool, ns_router, ns_mgr, spc_mgr, auth);
    // [D3.5 r2 · Wave S routes] /watch aliases (openapi/SDK/MCP fan-out shape) mapped
    // onto the same connector watcher mechanism (POST reuses ConnectorAddWatcher; each
    // target namespace = one connector watcher over `path`). /connector/* stays.
    cortrix::RegisterWatchAliasRoutes(server.server(), connector_state, ns_pool, ns_router, spc_mgr, auth);
    // [D3.5 batch1] P09 tenant/permission/quota REST (15 endpoints, sec 4.5). In CE
    // no-auth mode WithAuth grants admin so the admin-gated endpoints are reachable
    // for local integration; full multi-tenant enforcement activates with P08 auth.
    cortrix::RegisterTenantRoutes(server.server(), tenant_svc, perm_svc, quota_svc, auth);
    // [D3.5 batch1/F18a] operation-log query route (GET /api/v1/operations, read-only).
    cortrix::RegisterOperationsRoutes(server.server(), *obs_module.logger(), auth);
    // [D3.5 r2 · Wave P · P1] P08-CE auth REST surface. Bootstrap GET+POST are NOT
    // WithAuth-wrapped (on first run no key exists; the bootstrap token IS the
    // credential) — AdminGuard's loopback IP filter on /api/v1/admin/* is the
    // ambient gate. /auth/api-keys CRUD is WithAuth(admin).
    cortrix::RegisterBootstrapRoutes(server.server(), bootstrap_handler);
    cortrix::RegisterApiKeyRoutes(server.server(), api_key_service, auth);
    // [D3.5 r2 · Wave P · P2] F16a DB-import 6-endpoint surface (admin-gated;
    // /admin/db-connections* under AdminGuard, /import/* Layer-2 only per §6.1).
    cortrix::RegisterImportRoutes(server.server(), import_handler, auth);
    // [D3.5 r2 · Wave P · P4] F48 §6.3 agent_llm_config GET (read) / PUT (admin).
    // Backed by the shared global_config (api_key encrypted at rest). The PUT path
    // is not under AdminGuard, so it enforces admin in-handler.
    cortrix::RegisterSystemConfigRoutes(server.server(), *global_config, auth);
    // [OPEN-2] GC + maintenance ops routes (/api/v1/gc/*, /api/v1/maintenance/*).
    cortrix::RegisterGcRoutes(server.server(), gc_manager, gc_doc_sweeper, auth);

    // [F20] Install AdminGuard pre-routing filter: admin paths are restricted to
    // the configured client-IP whitelist (loopback-only by default). Runs before
    // route dispatch / API-key auth. Config comes from CORTRIX_ADMIN_BIND.
    cortrix::middleware::InstallAdminGuard(server.server(),
        {config.security.admin_bind, config.security.allow_public_admin});
    // [F24] Deployment wiring (Wave D-R1, additive). The DiskMonitor itself is
    // constructed earlier (6b) so the SPC admission gate + upload route hold it.
    // Dual health endpoint (F24 main impl): /api/v1/system/health/{live,ready}.
    // [D3.5 wire · Derek approach A] /ready aggregates the F20 ReadinessRegistry (unified
    // abstraction — replaced F24's standalone HealthProviders). Register the readiness
    // components here. `disk` has a real probe today (DiskMonitor::ShouldRejectWrites).
    // The other 4 design-sec-8.4 components — catalog (F12 IBloomFilter), vector_index
    // (F01 model load + WAL replay), spc_pipeline (F06/F03 workers), memory_store (MEM)
    // — plus secret_provider register here as their owning Features expose IsReady()
    // (F20-S5, deferred — intentionally NOT stubbed ready, to avoid a false 200).
    cortrix::health::ReadinessRegistry readiness;
    readiness.Register("disk", [&f24_disk_monitor]() {
        cortrix::health::ComponentReadiness r;
        r.ready = !f24_disk_monitor.ShouldRejectWrites();
        r.detail["stage"] = std::string(cortrix::deploy::ToString(f24_disk_monitor.Usage().stage));
        return r;
    });
    cortrix::deploy::RegisterHealthRoutes(
        server.server(), readiness,
        []() { return g_shutting_down.load(); },
        cortrix::kCortrixVersion);
    // OpenMetrics endpoint on :9091 (anonymous, separate port). The MetricsServer
    // always renders the DeployMetrics gauges (disk/shutdown/uptime/build_info); the
    // §5.3 subsystem recorders are AddSource()'d below.
    cortrix::deploy::DeployMetrics::Instance().SetBuildInfo(cortrix::kCortrixVersion, "", "");
    cortrix::deploy::DeployMetrics::Instance().MarkStart();
    auto f24_metrics_server = cortrix::deploy::RegisterMetricsServer(9091);
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
        CORTRIX_LOG_INFO("metrics", "OpenMetrics endpoint on :9091/metrics (21 subsystem recorders)");
    } else {
        CORTRIX_LOG_WARN("metrics", "OpenMetrics endpoint failed to bind :9091 (port in use?)");
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
        /*drain_tasks=*/ [&worker_pool, &spc_mgr](std::chrono::steady_clock::time_point) {
            // Order matters (original Plan B teardown): the F42 pool stops first so
            // doc_processor→spc_mgr quiesces, then the SPC drain joins its workers
            // (each finishes at most its in-hand task) and exports the untouched
            // queue remainder for .pending_tasks.json.
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

    // 13b. Start directory monitoring (all configured watchers)
    for (auto& wentry : connector_state.watchers) {
        s = wentry.importer->Start();
        if (!s.ok()) {
            CORTRIX_LOG_WARN("main", "Directory monitoring failed to start: {}", s.message());
        } else {
            CORTRIX_LOG_INFO("main", "Directory monitoring started: {}",
                            wentry.importer->GetConfig().data_dir);
        }
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

    // Graceful shutdown — stop all directory watchers
    {
        std::lock_guard<std::mutex> lock(connector_state.mu);
        for (auto& wentry : connector_state.watchers) {
            wentry.importer->Stop();
        }
    }
    // [OPEN-2] stop the GC thread (also RAII-handled, but explicit so an in-flight
    // sweep finishes + the thread joins before the catalog handle is torn down).
    gc_thread.Stop();
    // [F24] stop the deployment background services (also handled by RAII, but
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
