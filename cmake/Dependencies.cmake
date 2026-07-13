include(FetchContent)

# cpp-httplib (header-only HTTP server)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
)

# yaml-cpp (YAML parser)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
)

# nlohmann/json (JSON library)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# spdlog (logging)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
)

# Google Test (testing framework)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
)

# rapidcheck (property-based testing; R9 robustness lane). Test-only — linked only
# by the unit-test target. RC_ENABLE_GTEST exposes the `rapidcheck_gtest` interface
# target that provides the <rapidcheck/gtest.h> RC_GTEST_PROP macro, which integrates
# a property as a normal gtest case. Pinned to a commit (rapidcheck is unversioned).
FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        ff6af6fc683159deb51c543b065eba14dfcf329b  # 2023-12, last stable HEAD
)
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)

# hnswlib (vector index): NOT fetched anymore — vendored in-tree as the F01
# P-HNSW shallow fork at src/store/phnsw/hnswlib/ (see CMakeLists.txt there).

# SQLite amalgamation
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Google Benchmark (performance benchmarking)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(httplib yaml-cpp json spdlog googletest benchmark rapidcheck)

# --- CoreML support (Apple platforms only) ---
if(APPLE)
    find_library(COREML_FRAMEWORK CoreML)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    if(COREML_FRAMEWORK AND FOUNDATION_FRAMEWORK)
        set(CORTRIX_HAS_COREML ON)
        add_compile_definitions(CORTRIX_HAS_COREML)
        message(STATUS "CoreML support enabled")
    else()
        message(STATUS "CoreML not available, using CPU only")
    endif()
endif()

# --- ONNX Runtime (pre-built binary for real embeddings) ---
option(CORTRIX_USE_ONNX "Enable ONNX Runtime for real embedding inference" ON)
set(CORTRIX_ONNX_RUNTIME_FLAVOR "cpu" CACHE STRING
    "ONNX Runtime distribution flavor (cpu or cuda; cuda is Linux x86_64 only)")
set_property(CACHE CORTRIX_ONNX_RUNTIME_FLAVOR PROPERTY STRINGS cpu cuda)
string(TOLOWER "${CORTRIX_ONNX_RUNTIME_FLAVOR}" CORTRIX_ONNX_RUNTIME_FLAVOR)
if(NOT CORTRIX_ONNX_RUNTIME_FLAVOR MATCHES "^(cpu|cuda)$")
    message(FATAL_ERROR
        "CORTRIX_ONNX_RUNTIME_FLAVOR must be 'cpu' or 'cuda', got '${CORTRIX_ONNX_RUNTIME_FLAVOR}'")
endif()

# F22: lock the ONNX Runtime ABI *major* version this binary is built against.
# Within one major (1.x), ONNX Runtime keeps ABI compatibility, so a same-major
# upgrade (e.g. 1.17 -> 1.19) is just a `.so`/dylib swap + restart — no rebuild.
# Crossing a major (1.x -> 2.x, every few years) is the only case that needs a
# rebuild with this flag bumped. cortrix::onnx::StartupValidator compares this
# compiled expectation against the loaded runtime's actual version at startup and
# fails fast (CX_ERR_ONNXRT_VERSION_MISMATCH) if they disagree. Default "1".
set(ONNXRT_MAJOR_VERSION "1" CACHE STRING "ONNX Runtime ABI major version (compatible range this binary is built for)")
add_compile_definitions(CORTRIX_ONNXRT_EXPECTED_MAJOR=${ONNXRT_MAJOR_VERSION})

if(CORTRIX_USE_ONNX)
    set(ONNXRUNTIME_VERSION "1.17.1")
    if(CORTRIX_ONNX_RUNTIME_FLAVOR STREQUAL "cuda")
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
           NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            message(FATAL_ERROR
                "CORTRIX_ONNX_RUNTIME_FLAVOR=cuda requires Linux x86_64")
        endif()
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}.tgz")
        # Official v1.17.1 asset: 170787228 bytes. GitHub's legacy release metadata
        # does not publish a digest; this SHA-256 was independently reproduced from
        # three retained copies whose name and byte size match the official asset.
        set(ORT_URL_HASH
            "SHA256=613c53745ea4960ed368f6b3ab673558bb8561c84a8fa781b4ea7fb4a4340be4")
        add_compile_definitions(CORTRIX_HAS_CUDA)
        message(STATUS "ONNX Runtime CUDA flavor enabled (Linux x86_64)")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}.tgz")
        set(ORT_URL_HASH
            "SHA256=89566f424624a7ad9a7d9d5e413c44b9639a994d7171cf409901d125b16e2bb3")
    elseif(APPLE)
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-osx-x86_64-${ONNXRUNTIME_VERSION}.tgz")
        set(ORT_URL_HASH
            "SHA256=86c6b6896434084ff5086eebc4e9ea90be1ed4d46743f92864f46ee43e7b5059")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-aarch64-${ONNXRUNTIME_VERSION}.tgz")
        set(ORT_URL_HASH
            "SHA256=70b6f536bb7ab5961d128e9dbd192368ac1513bffb74fe92f97aac342fbd0ac1")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz")
        set(ORT_URL_HASH
            "SHA256=89b153af88746665909c758a06797175ae366280cbf25502c41eb5955f9a555e")
    else()
        message(WARNING "ONNX Runtime: unsupported platform, disabling")
        set(CORTRIX_USE_ONNX OFF)
    endif()

    if(CORTRIX_USE_ONNX)
        FetchContent_Declare(
            onnxruntime
            URL ${ORT_URL}
            URL_HASH ${ORT_URL_HASH}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(onnxruntime)

        # Create interface target for ONNX Runtime
        add_library(onnxruntime_lib INTERFACE)
        target_include_directories(onnxruntime_lib INTERFACE
            ${onnxruntime_SOURCE_DIR}/include
        )
        if(APPLE)
            target_link_libraries(onnxruntime_lib INTERFACE
                ${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.${ONNXRUNTIME_VERSION}.dylib
            )
        else()
            target_link_libraries(onnxruntime_lib INTERFACE
                ${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so.${ONNXRUNTIME_VERSION}
            )
        endif()
    endif()
endif()

# SQLite: build as a library from amalgamation
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
    FetchContent_Populate(sqlite3)
    add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_JSON1
        # SQLITE_THREADSAFE=1 (serialized), NOT =2 (multi-thread). The catalog.db
        # handle (owned by CatalogDb) is borrowed and used concurrently by multiple
        # threads — the GC background thread (GcManager) and HTTP request threads
        # (F12) — with no owner-level mutex. Under multi-thread mode (=2), concurrent
        # use of a single connection is undefined behavior and crashed intermittently
        # (GcThreadTest.LoopRunsSweep SEGFAULT, ~2/3 in isolation). Serialized mode
        # gives each connection its own recursive mutex: independent connections
        # (per-Unit store.db in the F05 pool) still run fully in parallel; only
        # concurrent use of the SAME connection is serialized. The cost is an
        # uncontended lock/unlock per API call on single-threaded connections
        # (negligible) plus real serialization on the shared catalog connection
        # (low-volume metadata, cached behind F12 bloom filter).
        # Phase-2 perf upgrade path (per-connection owner mutex / per-thread WAL
        # connections) is tracked in hub PHASE2_BACKLOG (TD-CATALOG-CONN-CONCURRENCY).
        SQLITE_THREADSAFE=1
    )
endif()
