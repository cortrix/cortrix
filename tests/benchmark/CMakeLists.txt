add_executable(cortrix_benchmarks
    bench_store_sqlite.cpp
    # bench_vector_search.cpp removed (P6): it benchmarked the MVP CortrixVectorHnswlib
    # store, deleted when P-HNSW replaced it — bench_phnsw.cpp covers the same
    # add/search/topk/recovery workloads on the current vector index.
    bench_phnsw.cpp
    bench_bm25_search.cpp
    # bench_query_pipeline.cpp excluded (P6): its harness is built on the deleted MVP
    # query path — it constructs CortrixVectorHnswlib (removed) and feeds it to a
    # pipeline signature replaced by ScatterGather/namespace pool in this D3.5 round. Re-porting
    # it to the new query path is a fresh benchmark, not a fix; parked until then.
    bench_http_api.cpp
    bench_spc_pipeline.cpp
    bench_memory_system.cpp
    bench_enricher.cpp
    bench_sparse.cpp
)

target_link_libraries(cortrix_benchmarks PRIVATE
    cortrix_core
    benchmark::benchmark
    benchmark::benchmark_main
)

target_include_directories(cortrix_benchmarks PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/mocks
)
