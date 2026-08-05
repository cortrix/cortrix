# src/store/phnsw — P-HNSW vendored hnswlib
#
# hnswlib is header-only, so the vendored fork is exposed as an INTERFACE target
# rather than a compiled static library. Targets that need the HNSW graph types
# (cortrix_core, and later the P-HNSW persistence sources) link `hnswlib_vendored`
# and include it as `<hnswlib/hnswlib.h>`.
#
# This replaces the previous `${hnswlib_SOURCE_DIR}` include path that came from
# CMake FetchContent (cmake/Dependencies.cmake). The source now lives in-tree at
# src/store/phnsw/hnswlib/ (see that folder's README.md for provenance).

add_library(hnswlib_vendored INTERFACE)
add_library(cortrix::hnswlib_vendored ALIAS hnswlib_vendored)

# SYSTEM include: hnswlib is third-party (shallow fork, algorithm untouched), so
# its headers are exposed as system headers. This keeps Cortrix's strict
# -Wall -Wextra -Wpedantic fully active on our own code while suppressing
# upstream hnswlib's own warnings (unused-parameter etc.) — the standard idiom
# for vendored third-party headers, and it also drops the ~15 hnswlib warnings
# the previous FetchContent include path produced.
target_include_directories(hnswlib_vendored SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# hnswlib uses <thread> / std::thread in its parallel helpers.
find_package(Threads REQUIRED)
target_link_libraries(hnswlib_vendored INTERFACE Threads::Threads)
