cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CORTRIX_RELEASE_VERSION LIMIT_COUNT 1)
string(STRIP "${CORTRIX_RELEASE_VERSION}" CORTRIX_RELEASE_VERSION)
if(NOT CORTRIX_RELEASE_VERSION MATCHES
   "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z]+(\\.[0-9A-Za-z]+)*)?$")
    message(FATAL_ERROR "VERSION must contain one SemVer value without a leading 'v'")
endif()
set(CORTRIX_VERSION_BASE "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
project(cortrix VERSION "${CORTRIX_VERSION_BASE}" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerFlags.cmake)
include(cmake/Dependencies.cmake)

find_package(OpenSSL REQUIRED)

# Vendored hnswlib (P-HNSW shallow fork, F01). Provides the
# cortrix::hnswlib_vendored INTERFACE target; replaces the old FetchContent
# hnswlib include path. Source lives in-tree at src/store/phnsw/hnswlib/.
add_subdirectory(src/store/phnsw)

# --- Static library: cortrix_core ---
add_library(cortrix_core STATIC
    src/common/status.cpp
    src/common/executor_engine.cpp
    src/common/in_memory_global_config.cpp
    # --- F48 §6.2 agent_llm config codec (D3.5 r2 · Wave P · P4) ---
    src/common/agent_llm_config_codec.cpp
    src/config/config.cpp
    src/logging/logging.cpp
    src/auth/api_key_auth.cpp
    src/auth/auth_middleware.cpp
    src/auth/auth_error.cpp
    src/auth/auth_schema.cpp
    src/auth/platform_db.cpp
    src/auth/jwt_secret_service.cpp
    src/auth/auth_config_service.cpp
    src/auth/password_utils.cpp
    src/auth/pbkdf2_password_hasher.cpp
    src/auth/jwt_utils.cpp
    src/auth/auth_service.cpp
    src/auth/jwt_auth_middleware.cpp
    src/auth/email_sender.cpp
    src/auth/api_key_service.cpp
    src/auth/bootstrap_handler.cpp
    src/auth/admin_users_service.cpp
    src/namespace/namespace_manager.cpp
    src/server/bootstrap.cpp
    src/server/gc_cli.cpp
    src/server/http_server.cpp
    src/server/routes/health_routes.cpp
    src/server/routes/namespace_routes.cpp
    src/store/block_header.cpp
    src/store/f09_schema_provider.cpp
    src/store/f34_schema_provider.cpp
    src/store/group_commit_writer.cpp
    src/store/pending_entry.cpp
    src/store/pending_log_writer.cpp
    src/store/write_coordinator.cpp
    src/store/iindex_factory.cpp
    src/store/phnsw/phnsw.cpp
    src/store/phnsw/wal_entry.cpp
    src/store/phnsw/wal_writer.cpp
    src/store/phnsw/snapshot_manager.cpp
    src/store/cortrix_store_sqlite.cpp
    src/store/cortrix_blob_local.cpp
    src/spc/parser_errors.cpp
    src/spc/parser.cpp
    src/spc/parser_subprocess.cpp
    src/spc/parser_factory.cpp
    src/spc/docling_parser.cpp
    src/spc/paddleocr_parser.cpp
    src/spc/parser_schema_provider.cpp
    src/spc/recursive_chunker.cpp
    src/spc/cleaning_errors.cpp
    src/spc/data_cleaner.cpp
    src/spc/cleaning_config_resolver.cpp
    src/spc/onnx_embedder.cpp
    src/spc/onnx_runtime.cpp
    src/spc/onnx_error.cpp
    src/spc/onnx_metrics.cpp
    src/spc/startup_validator.cpp
    src/spc/onnx_cli.cpp
    src/spc/hf_tokenizer.cpp
    src/spc/block_assembler.cpp
    # --- F03 NER + Summary Enricher (Wave B) ---
    src/spc/enricher_error.cpp
    src/spc/enricher_metrics.cpp
    src/spc/budget_tracker.cpp
    src/spc/enricher_startup.cpp
    src/spc/enricher_config_resolver.cpp
    src/spc/f03_schema_provider.cpp
    src/spc/f35_schema_provider.cpp
    src/spc/enrich_state_schema_provider.cpp
    src/spc/enrich_state_store.cpp
    src/spc/enrich_backfill_worker.cpp
    src/spc/enrich_retry_sweeper.cpp
    src/spc/enricher_store.cpp
    src/spc/spc_enricher.cpp
    src/spc/prompt_template.cpp
    src/spc/enricher_response_parser.cpp
    src/spc/llm_enricher.cpp
    src/spc/spc_router.cpp
    src/spc/spc_task_queue.cpp
    src/spc/spc_pipeline.cpp
    src/spc/enricher_chain.cpp
    src/spc/spc_manager.cpp
    # --- F38 HyPE Index (Wave C) ---
    src/spc/f38_schema_provider.cpp
    src/spc/hype_error.cpp
    src/spc/hype_enricher.cpp
    src/spc/hype_block.cpp
    src/spc/hype_fusion.cpp
    src/spc/hype_ns_config.cpp
    src/spc/hype_metrics.cpp
    # --- F35 Contextual Retrieval (Wave C-R2) ---
    src/spc/contextual_error.cpp
    src/spc/contextual_metrics.cpp
    src/spc/contextual_enricher.cpp
    src/spc/contextual_store.cpp
    src/memory/memory_store.cpp
    src/memory/memory_scorer.cpp
    src/memory/memory_writer.cpp
    src/memory/memory_searcher.cpp
    src/memory/memory_injector.cpp
    src/memory/memory_routes.cpp
    # --- MEM02 LLM Memory Extraction (Wave B-R3) ---
    src/memory/mem02_error.cpp
    src/memory/mem02_metrics.cpp
    src/memory/prompt_templates.cpp
    src/memory/memory_extractor.cpp
    src/memory/contradiction_detector.cpp
    src/memory/memory_queue.cpp
    # --- MEM02/03 store adapters and user_facts query ---
    src/memory/memory_block_adapter.cpp
    src/memory/memory_extraction_service.cpp
    # --- MEM03 Memory Transparency (Wave C-R1) ---
    src/memory/mem03_error.cpp
    src/memory/mem03_metrics.cpp
    src/memory/memory_transparency.cpp
    # --- MEM04 Memory Immunity (Wave C-R2) ---
    src/memory/mem04_error.cpp
    src/memory/mem04_metrics.cpp
    src/memory/opt_out_manager.cpp
    # --- MEM05 Per-user Isolation metrics (D35-MET-09) ---
    src/memory/mem05_metrics.cpp
    src/query/rrf_fusion.cpp
    src/query/vector_searcher.cpp
    src/query/bm25_searcher.cpp
    src/query/intent_classifier.cpp
    src/query/post_filter.cpp
    src/query/degradation_manager.cpp
    src/query/query_pipeline.cpp
    src/query/query_request.cpp
    src/query/query_response.cpp
    src/query/query_routes.cpp
    # --- F04 Cross-NS Query (Wave 1-5) ---
    src/query/cross_ns_error.cpp
    src/query/content_hash.cpp
    src/query/cross_ns_types.cpp
    src/query/cross_ns_response.cpp
    src/query/single_unit_executor.cpp
    src/query/authorize_namespaces.cpp
    src/query/cross_ns_dedup.cpp
    src/query/scatter_metrics.cpp
    src/query/scatter_gather.cpp
    src/query/scatter_guc.cpp
    src/query/scatter_plan.cpp
    src/query/cross_ns_query_handler.cpp
    # --- F04 cross-NS query live wiring (D3.5-R2 Wave Q1/Q2) ---
    src/query/live_single_unit_executor.cpp
    src/query/query_wiring.cpp
    # --- F36 RAG-Fusion (Wave B) ---
    src/query/rag_fusion_error.cpp
    src/query/rag_fusion_types.cpp
    src/query/rag_fusion_metrics.cpp
    src/query/query_variant_generator.cpp
    src/query/rag_fusion.cpp
    src/query/rag_fusion_stage.cpp
    # --- F36-LR LLM listwise rerank (2026-07-05 addendum) ---
    src/query/llm_rerank_stage.cpp
    src/query/crag_stage.cpp
    # --- F40 BGE-M3 Sparse (Wave C) ---
    src/retrieval/sparse_error.cpp
    src/retrieval/sparse_codec.cpp
    src/retrieval/f40_schema_provider.cpp
    src/retrieval/splade_sparse_retriever.cpp
    src/retrieval/sparse_ns_config.cpp
    src/retrieval/sparse_metrics.cpp
    src/retrieval/sparse_rrf.cpp
    src/retrieval/sparse_fallback.cpp
    src/retrieval/sparse_index_registry.cpp
    src/retrieval/sparse_vec_store.cpp
    # --- F41 Document Summary Index (Wave C-R2) ---
    src/doc_summary/doc_summary_error.cpp
    src/doc_summary/doc_summary_metrics.cpp
    src/doc_summary/doc_summary_types.cpp
    src/doc_summary/f41_schema_provider.cpp
    src/doc_summary/doc_fts5_index.cpp
    src/doc_summary/doc_summary_generator.cpp
    src/doc_summary/f41_async_worker.cpp
    src/doc_summary/discover_handler.cpp
    src/connector/file_filter.cpp
    src/connector/file_utils.cpp
    src/connector/file_watcher.cpp
    src/connector/directory_importer.cpp
    src/connector/dir_watcher_registry.cpp
    src/connector/watcher_schema_provider.cpp
    src/upload/upload_handler.cpp
    src/server/routes/agent_proxy_routes.cpp
    src/server/routes/document_routes.cpp
    src/server/routes/enrich_routes.cpp
    src/server/routes/flat_document_routes.cpp
    src/server/routes/connector_routes.cpp
    src/server/routes/operations_routes.cpp
    src/server/routes/gc_routes.cpp
    src/llm/openai_client.cpp
    src/llm/http_transport.cpp
    src/agent_friendly/error.cpp
    src/observability/observability_context.cpp
    src/observability/operation_log_error.cpp
    src/observability/operation_log_schema.cpp
    src/observability/operation_logger_impl.cpp
    src/observability/cleanup_scheduler.cpp
    src/observability/operation_log_emitter.cpp
    src/observability/oplog_metrics.cpp
    src/observability/observability_module.cpp
    src/agent_trace/agent_trace_schema.cpp
    src/agent_trace/agent_trace_error.cpp
    src/agent_trace/agent_trace_metrics.cpp
    src/agent_trace/agent_trace_writer_impl.cpp
    src/agent_trace/http_observability_middleware.cpp
    src/agent_trace/mcp_session_handler.cpp
    src/agent_trace/interaction_sources_schema.cpp
    src/agent_trace/interactions_handler.cpp
    src/agent_trace/traces_handler.cpp
    src/server/routes/observability_routes.cpp
    src/agent_trace/engine_instrumentation.cpp
    src/agent_trace/f13_cleanup_registrar.cpp
    src/agent_trace/interaction_log_sweeper.cpp
    src/agent_trace/observability_audit_log.cpp
    src/catalog/schema_migrator.cpp
    src/catalog/catalog_schema.cpp
    src/catalog/catalog_db.cpp
    src/catalog/catalog_error.cpp
    src/catalog/catalog_types.cpp
    src/catalog/default_ns_router.cpp
    src/catalog/default_unit_router.cpp
    src/catalog/default_content_ref_store.cpp
    src/catalog/bloom_filter.cpp
    src/catalog/gc/blob_gc_sink.cpp
    src/catalog/gc/gc_manager.cpp
    src/catalog/gc/gc_thread.cpp
    src/catalog/gc/document_gc_sweeper.cpp
    src/config/file_based_global_config.cpp
    src/resource/pool_error.cpp
    src/resource/sqlite_conn.cpp
    src/resource/namespace_resource_bundle.cpp
    src/resource/namespace_pool.cpp
    src/resource/namespace_facade.cpp
    src/resource/ns_pool_metrics.cpp
    src/ml/execution_provider.cpp
    src/ml/ort_env_singleton.cpp
    src/ml/tokenizer_registry.cpp
    src/reranker/reranker_error.cpp
    src/reranker/reranker_startup_validator.cpp
    src/reranker/reranker_metrics.cpp
    src/reranker/reranker_thread_pool.cpp
    src/reranker/reranker_guc.cpp
    src/reranker/circuit_breaker.cpp
    src/reranker/reranker_status.cpp
    src/reranker/input_preprocessor.cpp
    src/reranker/reranker_schema_provider.cpp
    src/reranker/reranker_ns_config.cpp
    src/reranker/reranker_config_resolver.cpp
    src/reranker/onnx_reranker.cpp
    src/reranker/score_fusion.cpp
    src/reranker/reranker_factory.cpp
    # --- F34 Parent-Child Chunking (Wave 1-3) ---
    src/id/ulid.cpp
    # --- ID System Infrastructure (D3.5 Batch 0 prerequisite: SipHash block_id key) ---
    src/id/hash.cc
    src/chunker/chunker_config.cpp
    src/chunker/chunker_guc.cpp
    src/chunker/chunker_startup_validator.cpp
    src/chunker/parent_child_chunker.cpp
    src/chunker/child_parent_dedup.cpp
    src/chunker/chunker_agent_error.cpp
    src/chunker/chunker_response_meta.cpp
    src/store/sqlite_parent_chunk_store.cpp
    src/store/parent_chunk_store_metrics.cpp
    src/store/sqlite_chunk_store.cpp
    # --- F42 Document Async Processing (Wave B-R2) ---
    src/async/f42_error.cpp
    src/async/f42_metrics.cpp
    src/async/task_manager.cpp
    src/async/task_cleanup_cron.cpp
    src/async/task_scheduler.cpp
    src/async/task_finalizer.cpp
    src/async/document_processor.cpp
    src/async/worker_pool.cpp
    src/async/document_task_handler.cpp
    # --- F16a DB Manual Import (Wave B-R2) ---
    src/import/import_types.cpp
    src/import/import_error.cpp
    src/import/import_response.cpp
    src/import/f16a_schema_provider.cpp
    src/import/connection_manager.cpp
    src/import/query_executor.cpp
    src/import/text_serializer.cpp
    src/import/spc_feed.cpp
    # --- F16a real SPCManager feed (D3.5 r2 · Wave P · P2b) ---
    src/import/spc_manager_feeder.cpp
    src/import/import_task_queue.cpp
    src/import/import_manager.cpp
    src/import/import_metrics.cpp
    src/server/import_handler.cpp
    # --- F16a DB-import REST surface (D3.5 r2 · Wave P · P2) ---
    src/server/routes/import_routes.cpp
    # --- F08 Metadata Block (Wave A) ---
    src/metadata/metadata_error.cpp
    src/metadata/metadata_metrics.cpp
    src/metadata/metadata_schema_provider.cpp
    src/metadata/rule_based_metadata_generator.cpp
    # --- F07 Semantic Score (Wave B) ---
    src/scoring/scoring_error.cpp
    src/scoring/score_map.cpp
    src/scoring/semantic_scorer.cpp
    src/scoring/scoring_metrics.cpp
    src/scoring/scoring_schema_provider.cpp
    src/scoring/scoring_store.cpp
    # --- F37 CRAG Retrieval Evaluation (Wave C-R1) ---
    src/retrieval/crag_error.cpp
    src/retrieval/crag_evaluator.cpp
    src/retrieval/crag_metrics.cpp
    src/query/query_context_explain.cpp
    # --- F39 Query Complexity Router (Wave C-R2) ---
    src/query/router_error.cpp
    src/query/query_router_metrics.cpp
    src/query/query_complexity_classifier.cpp
    src/query/wordpiece_tokenizer.cpp
    src/query/onnx_complexity_backend.cpp
    # --- P09 Tenant Management (Wave D-R1) ---
    src/tenant/tenant_types.cpp
    src/tenant/tenant_error.cpp
    src/tenant/tenant_service.cpp
    src/tenant/permission_service.cpp
    src/tenant/quota_service.cpp
    src/server/routes/tenant_routes.cpp
    # --- P08 CE auth REST surface (D3.5 r2 · Wave P · P1) ---
    src/server/routes/auth_routes.cpp
    # --- F48 agent_llm_config admin API (D3.5 r2 · Wave P · P4) ---
    src/server/routes/system_config_routes.cpp
    # --- TD-F42 Bulk Submit (Wave D-R1) ---
    src/server/batch_error.cpp
    src/server/batch_submit_service.cpp
    src/server/batch_temp_store.cpp
    src/server/routes/batch_routes.cpp
    # --- F20 Security Hardening (Wave D-R1) ---
    src/security/env_secret_provider.cpp
    src/logging/sanitizer.cpp
    src/middleware/admin_guard.cpp
    src/middleware/rate_limiter.cpp
    src/health/readiness.cpp
    # --- F24 docker-compose Deployment (Wave D-R1) ---
    src/deploy/f24_error.cpp
    src/deploy/deploy_metrics.cpp
    src/deploy/disk_monitor.cpp
    src/deploy/reason_vocabulary.cpp
    src/deploy/graceful_shutdown.cpp
    src/deploy/health_routes.cpp
    src/deploy/metrics_server.cpp
)
target_include_directories(cortrix_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(cortrix_core PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
)
target_link_libraries(cortrix_core PUBLIC
    httplib
    yaml-cpp
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    sqlite3
    OpenSSL::Crypto
    OpenSSL::SSL
    z
    cortrix::hnswlib_vendored
)
if(APPLE)
    target_link_libraries(cortrix_core PUBLIC "-framework CoreServices")
endif()
# ONNX Runtime (optional, for real embeddings)
if(CORTRIX_USE_ONNX)
    target_link_libraries(cortrix_core PUBLIC onnxruntime_lib)
    target_compile_definitions(cortrix_core PUBLIC CORTRIX_HAS_ONNXRUNTIME)
endif()
# CoreML GPU acceleration (Apple platforms only)
if(CORTRIX_HAS_COREML AND CORTRIX_USE_ONNX)
    target_link_libraries(cortrix_core PUBLIC
        ${COREML_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
    )
endif()
# hnswlib include path now comes from the vendored cortrix::hnswlib_vendored
# target linked above (src/store/phnsw/), not FetchContent.

# --- Executable: cortrix-server ---
# The full assembly lives in the reusable bootstrap (src/server/bootstrap.cpp,
# part of cortrix_core) — this binary is a thin RunServer() shim. Embedders
# build their own entry point on the same bootstrap (ServerExtensions hooks).
add_executable(cortrix-server src/main.cpp)
target_link_libraries(cortrix-server PRIVATE cortrix_core)

# --- Tests ---
# BUILD_TESTS=OFF (e.g. the production Docker image) skips every test
# target; default ON for dev/CI builds.
option(BUILD_TESTS "Build the cortrix test suites" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
