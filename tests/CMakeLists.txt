enable_testing()

# --- Unit tests ---
add_executable(cortrix_unit_tests
    sqlite_test_env.cpp
    unit/test_status.cpp
    unit/test_config.cpp
    unit/test_api_key_auth.cpp
    unit/test_auth_error.cpp
    unit/test_auth_schema.cpp
    unit/test_auth_config_init.cpp
    unit/test_register.cpp
    unit/test_jwt_utils.cpp
    unit/test_login.cpp
    unit/test_pbkdf2_hasher.cpp
    unit/test_auth_middleware_jwt.cpp
    unit/test_password_reset.cpp
    unit/test_api_keys.cpp
    # --- D3.5 r2 · Wave P wiring logic (ApiKeyAuth DB fallback + agent_llm codec) ---
    unit/test_wavep_wiring.cpp
    unit/test_jwt_secret_rotate.cpp
    unit/test_smtp_admin_api.cpp
    unit/test_namespace_manager.cpp
    unit/test_logging.cpp
    unit/test_recursive_chunker.cpp
    unit/test_document_parser.cpp
    unit/test_parser_interface.cpp
    unit/test_parser_factory.cpp
    unit/test_docling_parser.cpp
    unit/test_paddleocr_parser.cpp
    unit/test_parser_fallback.cpp
    unit/test_parser_page_progress.cpp
    unit/test_parser_schema_provider.cpp
    unit/test_parser_agent_friendly.cpp
    unit/test_parser_branches.cpp
    unit/test_parser_subprocess_branches.cpp
    unit/test_data_cleaner.cpp
    unit/test_cleaning_errors.cpp
    unit/test_cleaning_plugin_stub.cpp
    unit/test_block_assembler.cpp
    unit/test_spc_task_queue.cpp
    unit/test_spc_manager.cpp
    unit/test_block_header.cpp
    unit/test_f09_schema_provider.cpp
    unit/test_group_commit_writer.cpp
    unit/test_pending_entry.cpp
    unit/test_pending_log_writer.cpp
    unit/test_write_coordinator.cpp
    unit/test_write_coordinator_recovery.cpp
    unit/test_iindex_factory.cpp
    unit/test_phnsw.cpp
    unit/test_phnsw_group_commit.cpp
    unit/test_phnsw_recovery.cpp
    unit/test_phnsw_concurrency.cpp
    unit/test_phnsw_autosnapshot.cpp
    unit/test_wal_entry.cpp
    unit/test_wal_writer.cpp
    unit/test_executor_engine.cpp
    unit/test_openai_client.cpp
    # --- F03 NER + Summary Enricher (Wave B) ---
    unit/test_enricher_error.cpp
    unit/test_spc_enricher.cpp
    unit/test_prompt_template.cpp
    unit/test_enricher_response_parser.cpp
    unit/test_llm_enricher.cpp
    unit/test_enricher_metrics.cpp
    unit/test_llm_enricher_resilience.cpp
    unit/test_budget_tracker.cpp
    unit/test_enricher_startup.cpp
    unit/test_enricher_config_resolver.cpp
    unit/test_f03_schema_provider.cpp
    unit/test_f34_schema_provider.cpp
    unit/test_f35_schema_provider.cpp
    unit/test_f38_schema_provider.cpp
    unit/test_f38_hype_enricher.cpp
    unit/test_f38_hype_block.cpp
    unit/test_f38_hype_fusion.cpp
    unit/test_f38_hype_error.cpp
    unit/test_f38_hype_ns_config.cpp
    unit/test_f38_hype_metrics.cpp
    # --- F35 Contextual Retrieval (Wave C-R2) ---
    unit/test_f35_contextual_error.cpp
    unit/test_f35_contextual_metrics.cpp
    unit/test_f35_contextual_enricher.cpp
    unit/test_enricher_store.cpp
    # --- I1/I2 enricher chain + contextualized persist (D3.5 round-2 Wave I) ---
    unit/test_enricher_chain.cpp
    unit/test_contextual_store.cpp
    # --- M1 memory store adapters + user_facts (D3.5 round-2 Wave M) ---
    unit/test_memory_block_adapter.cpp
    # --- M6 F13 observability per-NS routes (D3.5 round-2 Wave M) ---
    unit/test_observability_routes_per_ns.cpp
    unit/test_mock_response_builder.cpp
    mocks/mock_response_builder.cpp
    unit/test_agent_friendly_error.cpp
    unit/test_observability_context.cpp
    unit/test_agent_trace_schema.cpp
    unit/test_interaction_sources_schema.cpp
    unit/test_agent_trace_error.cpp
    unit/test_agent_trace_metrics.cpp
    unit/test_agent_trace_writer.cpp
    unit/test_http_observability_middleware.cpp
    unit/test_mcp_session_handler.cpp
    unit/test_interactions_handler.cpp
    unit/test_traces_handler.cpp
    unit/test_observability_routes.cpp
    unit/test_engine_instrumentation.cpp
    unit/test_f13_cleanup_registrar.cpp
    unit/test_interaction_log_sweeper.cpp
    unit/test_observability_audit_log.cpp
    unit/test_operation_log_error.cpp
    unit/test_operation_log_schema.cpp
    unit/test_operation_logger.cpp
    unit/test_operations_routes.cpp
    unit/test_cleanup_scheduler.cpp
    unit/test_schema_migrator.cpp
    unit/test_catalog_schema.cpp
    unit/test_catalog_schema_extra.cpp
    unit/test_catalog_providers.cpp
    unit/test_catalog_error.cpp
    unit/test_ns_router_contract.cpp
    unit/test_ttl_lru_cache.cpp
    unit/test_default_ns_router.cpp
    unit/test_ns_router_branches2.cpp
    unit/test_unit_router.cpp
    unit/test_content_ref_store.cpp
    unit/test_gc_manager.cpp
    unit/test_gc_manager_extra.cpp
    unit/test_gc_thread.cpp
    unit/test_gc_blob_sink.cpp
    unit/test_gc_routes.cpp
    unit/test_document_gc_sweeper.cpp
    unit/test_unit_scatter_executor.cpp
    unit/test_file_based_global_config.cpp
    unit/test_config_resolver.cpp
    unit/test_bloom_filter.cpp
    unit/test_batch_response.cpp
    unit/test_chunk_store.cpp
    unit/test_sqlite_chunk_store.cpp
    unit/test_global_config.cpp
    unit/test_cortrix_store_sqlite.cpp
    unit/test_cortrix_blob_local.cpp
    unit/test_memory_store.cpp
    unit/test_memory_scorer.cpp
    unit/test_mem05_user_isolation.cpp
    unit/test_memory_writer.cpp
    unit/test_memory_injector.cpp
    unit/test_memory_searcher.cpp
    unit/test_memory_searcher_mem05.cpp
    unit/test_memory_routes.cpp
    unit/test_query_routes.cpp
    unit/test_rrf_fusion.cpp
    unit/test_intent_classifier.cpp
    unit/test_post_filter.cpp
    unit/test_degradation_manager.cpp
    unit/test_query_pipeline.cpp
    unit/test_query_request.cpp
    unit/test_query_explain.cpp
    unit/test_bm25_searcher.cpp
    unit/test_file_filter.cpp
    unit/test_file_utils.cpp
    unit/test_file_watcher.cpp
    unit/test_dir_watcher_registry.cpp
    unit/test_watcher_schema_provider.cpp
    unit/test_upload_handler.cpp
    unit/test_scored_block.cpp
    unit/test_vector_searcher.cpp
    unit/test_sql_executor.cpp
    unit/test_spc_pipeline.cpp
    unit/test_subprocess_utils.cpp
    unit/test_ocr_processor.cpp
    unit/test_onnx_embedder.cpp
    unit/test_f40_onnx_embedder_sparse.cpp
    unit/test_f40_sparse_error.cpp
    unit/test_f40_sparse_codec.cpp
    unit/test_f40_sparse_retriever_interface.cpp
    unit/test_f40_schema_provider.cpp
    unit/test_f40_splade_write.cpp
    unit/test_sparse_vec_store.cpp
    unit/test_f40_splade_query.cpp
    unit/test_f40_sparse_ns_config.cpp
    unit/test_f40_sparse_metrics.cpp
    unit/test_f40_sparse_rrf.cpp
    unit/test_f40_sparse_fallback.cpp
    unit/test_sparse_index_registry.cpp
    # --- F41 Document Summary Index (Wave C-R2) ---
    unit/test_f41_doc_summary_error.cpp
    unit/test_f41_doc_summary_metrics.cpp
    unit/test_f41_schema_provider.cpp
    unit/test_f41_doc_fts5_index.cpp
    unit/test_f41_doc_summary_generator.cpp
    unit/test_f41_async_worker.cpp
    unit/test_onnx_runtime.cpp
    unit/test_onnx_error.cpp
    unit/test_onnx_startup_validator.cpp
    unit/test_startup_validator_branches.cpp
    unit/test_onnx_metrics.cpp
    unit/test_onnx_cli.cpp
    unit/test_hf_tokenizer.cpp
    unit/test_tokenizer_registry_branches.cpp
    unit/test_spc_router.cpp
    unit/test_http_server.cpp
    unit/test_document_routes.cpp
    unit/test_document_routes_extra.cpp
    unit/test_connector_routes.cpp
    unit/test_directory_importer.cpp
    unit/test_http_transport_unit.cpp
    unit/test_flat_document_routes.cpp
    unit/test_namespace_pool.cpp
    unit/test_namespace_pool_branches.cpp
    unit/test_sqlite_conn_branches.cpp
    unit/test_ns_pool_metrics.cpp
    unit/test_reranker_interface.cpp
    unit/test_onnx_reranker_skeleton.cpp
    unit/test_reranker_init_failfast.cpp
    unit/test_reranker_coreml_metrics.cpp
    unit/test_reranker_thread_pool.cpp
    unit/test_reranker_thread_pool_blocking.cpp
    unit/test_reranker_guc.cpp
    unit/test_reranker_circuit_breaker.cpp
    unit/test_reranker_status.cpp
    unit/test_reranker_max_seq_length.cpp
    unit/test_reranker_startup_compat.cpp
    unit/test_reranker_input_preprocessor.cpp
    unit/test_reranker_task_failure.cpp
    unit/test_reranker_schema_provider.cpp
    unit/test_reranker_ns_config.cpp
    unit/test_reranker_config_resolution.cpp
    unit/test_reranker_coverage.cpp
    unit/test_reranker_score_fusion.cpp
    # --- F34 Parent-Child Chunking (Wave 1-3) ---
    unit/test_ulid.cpp
    unit/test_chunker_config.cpp
    unit/test_parent_child_chunker.cpp
    unit/test_parent_chunk_store.cpp
    unit/test_parent_chunk_store_errors.cpp
    unit/test_child_parent_dedup.cpp
    unit/test_chunker_agent_error.cpp
    unit/test_chunker_fallback_perf.cpp
    # --- F04 Cross-NS Query (Wave 1 + Wave 2) ---
    scatter/test_cross_ns_error.cpp
    scatter/test_content_hash.cpp
    scatter/test_authorize_namespaces.cpp
    scatter/test_single_unit_executor.cpp
    scatter/test_scatter_gather.cpp
    scatter/test_cross_ns_response.cpp
    # --- F04 Cross-NS Query (Wave 3 + Wave 4) ---
    scatter/test_cross_ns_dedup.cpp
    scatter/test_scatter_metrics.cpp
    scatter/test_scatter_guc.cpp
    scatter/test_scatter_plan.cpp
    scatter/test_cross_ns_query_handler.cpp
    # --- F04 Cross-NS Query (Wave 5 — 7 §7.5 integration scenarios) ---
    scatter/test_scatter_integration.cpp
    # --- F36 RAG-Fusion (Wave B) ---
    unit/test_rag_fusion.cpp
    unit/test_query_response_branches.cpp
    # --- F37 CRAG Retrieval Evaluation (Wave C-R1) ---
    unit/test_crag_error.cpp
    unit/test_crag_evaluator.cpp
    unit/test_crag_metrics.cpp
    unit/test_retrieval_fallback.cpp
    unit/test_crag_context.cpp
    unit/test_crag_eval.cpp
    # --- F39 Query Complexity Router (Wave C-R2) ---
    unit/test_f39_router_error.cpp
    unit/test_f39_router_metrics.cpp
    unit/test_f39_complexity_backend.cpp
    unit/test_f39_complexity_classifier.cpp
    unit/test_wordpiece_tokenizer.cpp
    unit/test_onnx_complexity_backend.cpp
    # --- F42 Document Async Processing (Wave B-R2) ---
    unit/test_f42_error.cpp
    unit/test_task_manager.cpp
    unit/test_task_cleanup_cron.cpp
    unit/test_task_scheduler.cpp
    unit/test_task_finalizer.cpp
    unit/test_document_processor.cpp
    unit/test_worker_pool.cpp
    unit/test_worker_pool_dispatch.cpp
    unit/test_doc_summary_async_dispatch.cpp
    unit/test_doc_parse_spc_e2e.cpp
    unit/test_document_task_handler.cpp
    unit/test_f42_metrics.cpp
    # --- F16a DB Manual Import (Wave B-R2) ---
    unit/test_import_error.cpp
    unit/test_f16a_schema_provider.cpp
    unit/test_connection_manager.cpp
    unit/test_query_executor.cpp
    unit/test_query_executor_injection.cpp
    unit/test_text_serializer.cpp
    unit/test_import_task_queue.cpp
    unit/test_import_manager.cpp
    unit/test_import_metrics.cpp
    # --- F08 Metadata Block (Wave A) ---
    unit/test_metadata_error.cpp
    unit/test_metadata_schema_provider.cpp
    unit/test_rule_based_metadata_generator.cpp
    unit/test_metadata_metrics.cpp
    # --- F07 Semantic Score (Wave B) ---
    unit/test_scoring_error.cpp
    unit/test_score_map.cpp
    unit/test_semantic_scorer.cpp
    unit/test_scoring_schema_provider.cpp
    unit/test_scoring_metrics.cpp
    unit/test_scoring_store.cpp
    # --- MEM02 LLM Memory Extraction (Wave B-R3) ---
    unit/test_mem02_error.cpp
    unit/test_mem02_metrics.cpp
    # --- MEM03 Memory Transparency (Wave C-R1) ---
    unit/test_mem03_error.cpp
    unit/test_mem03_metrics.cpp
    unit/test_memory_transparency.cpp
    # --- MEM04 Memory Immunity (Wave C-R2) ---
    unit/test_mem04_error.cpp
    unit/test_mem04_metrics.cpp
    unit/test_opt_out_manager.cpp
    # --- MEM05 Per-user Isolation metrics (D35-MET-09) ---
    unit/test_mem05_metrics.cpp
    unit/test_prompt_templates.cpp
    unit/test_memory_queue.cpp
    unit/test_memory_extractor.cpp
    unit/test_memory_extraction_service.cpp
    # --- P09 Tenant Management (Wave D-R1) ---
    unit/test_tenant_error.cpp
    unit/test_tenant_types.cpp
    unit/test_tenant_service.cpp
    unit/test_permission_service.cpp
    unit/test_quota_service.cpp
    unit/test_permission_middleware.cpp
    # --- TD-F42 Bulk Submit (Wave D-R1) ---
    unit/test_batch_error.cpp
    unit/test_batch_submit_service.cpp
    unit/test_batch_routes.cpp
    # --- F20 Security Hardening (Wave D-R1) ---
    unit/test_env_secret_provider.cpp
    unit/test_log_sanitizer.cpp
    unit/test_admin_guard.cpp
    unit/test_rate_limiter.cpp
    unit/test_readiness.cpp
    # --- F24 docker-compose Deployment (Wave D-R1) ---
    unit/test_f24_disk.cpp
    unit/test_f24_metrics.cpp
    unit/test_f24_reason_vocabulary.cpp
    unit/test_f24_graceful_shutdown.cpp
    # --- ID System Infrastructure (D3.5 Batch 0 prerequisite) ---
    unit/test_siphash.cpp
    unit/test_id_hash.cpp
)

target_link_libraries(cortrix_unit_tests PRIVATE
    cortrix_core
    gtest_main
    gmock
)

# F23 §4.6 syscall fault-injection seam: MUST be a SHARED library — dyld
# only applies __DATA,__interpose sections found in dylibs, never in the
# main executable. Test-only; production targets never link it.
# macOS-only: dyld interpose and F_FULLFSYNC are not available on Linux.
# ASAN builds force this OFF: the __DATA,__interpose table and ASAN's own
# interceptors wrap the same POSIX I/O symbols and SIGSEGV at startup. The
# sweeps stay covered by the non-ASAN unit run; ASAN coverage for them is
# the Linux LD_PRELOAD lane (#481 cloud).
if(APPLE)
    option(CORTRIX_ENABLE_FAULT_INJECT
        "Build the dyld-interpose syscall fault-injection seam + sweeps" ON)
    if(CORTRIX_SANITIZE STREQUAL "address" AND CORTRIX_ENABLE_FAULT_INJECT)
        message(STATUS "CORTRIX_ENABLE_FAULT_INJECT forced OFF under ASAN "
                       "(interpose/interceptor conflict)")
        set(CORTRIX_ENABLE_FAULT_INJECT OFF)
    endif()
    if(CORTRIX_ENABLE_FAULT_INJECT)
        add_library(cortrix_fault_inject SHARED
            fault_inject/fault_inject.cpp
        )
        target_sources(cortrix_unit_tests PRIVATE
            unit/test_syscall_fault_inject.cpp
        )
        target_link_libraries(cortrix_unit_tests PRIVATE cortrix_fault_inject)
    endif()
endif() # APPLE

target_include_directories(cortrix_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/scatter
    ${CMAKE_SOURCE_DIR}/include
)

include(GoogleTest)
# F23: label suites so CI selects them via `ctest -L unit|integration`
# (ctest runs one process per test — never run the full suite single-process)
# DISCOVERY_MODE PRE_TEST: defer test enumeration to `ctest` time instead of a
# POST_BUILD step. A coverage-instrumented -O0 binary listing 4400 tests can
# exceed the POST_BUILD discovery timeout, and on timeout make DELETES the just-
# built binary — turning a slow-listing into a hard build failure. PRE_TEST
# enumerates lazily at test time, so the build never runs (or deletes) the binary.
gtest_discover_tests(cortrix_unit_tests PROPERTIES LABELS unit
    DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 300)

# --- Integration tests ---
add_executable(cortrix_integration_tests
    integration/test_health_endpoint.cpp
    integration/test_auth_middleware.cpp
    integration/test_namespace_api.cpp
    integration/test_storage_roundtrip.cpp
    integration/test_spc_end_to_end.cpp
    integration/test_directory_import.cpp
    integration/test_memory_api.cpp
    integration/test_mem05_integration.cpp
    integration/test_document_upload_api.cpp
    integration/test_cross_feature_e2e.cpp
    integration/test_e2e_error_recovery.cpp
    integration/test_e2e_document_lifecycle.cpp
    integration/test_e2e_concurrent.cpp
    integration/test_e2e_persistence.cpp
    integration/test_onnx_real_inference.cpp
    integration/test_real_spc_pipeline.cpp
    # --- F02 reranker real cross-encoder inference (SKIPs if model absent) ---
    integration/test_reranker_real_inference.cpp
    # --- F06 real docling/paddleocr bridges (SKIPs without CORTRIX_PARSER_PYTHON) ---
    integration/test_parser_real_inference.cpp
    integration/test_e2e_full_server.cpp
    integration/test_catalog_startup.cpp
    integration/test_phnsw_e2e.cpp
    # --- F36 RAG-Fusion (Wave B) ---
    integration/test_rag_fusion_e2e.cpp
    # --- F03 NER + Summary Enricher (Wave B, standalone integration) ---
    integration/test_enricher_integration.cpp
    # --- F42 Document Async Processing (Wave B-R2, standalone integration) ---
    integration/test_f42_async_lifecycle.cpp
    # --- F16a DB Manual Import (Wave B-R2, standalone integration — 9-issue matrix) ---
    integration/test_f16a_import_integration.cpp
    # --- F40 BGE-M3 Sparse (Wave C, standalone integration) ---
    integration/test_f40_sparse_integration.cpp
    # --- F38 HyPE Index (Wave C, standalone integration) ---
    integration/test_f38_hype_integration.cpp
    # --- MEM03 Memory Transparency (Wave C-R1, standalone integration) ---
    integration/test_memory_transparency_integration.cpp
    # --- P09 Tenant Management (Wave D-R1, standalone integration) ---
    integration/test_p09_tenant_integration.cpp
    mocks/mock_response_builder.cpp
)

target_link_libraries(cortrix_integration_tests PRIVATE
    cortrix_core
    gtest_main
    gmock
)

target_include_directories(cortrix_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/unit   # F42 integration reuses unit/parser_stub.h
    ${CMAKE_SOURCE_DIR}/include
)

gtest_discover_tests(cortrix_integration_tests PROPERTIES LABELS integration
    DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 300)

# --- Security tests ---
add_subdirectory(security)

# --- Stability tests ---
add_subdirectory(stability)

# --- Benchmarks ---
add_subdirectory(benchmark)
