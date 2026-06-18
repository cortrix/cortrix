#!/usr/bin/env bash
# Build libFuzzer harnesses for Cortrix parse entry points.
#
# Standalone build -- NOT wired into the main CMake build, because libFuzzer needs the
# homebrew LLVM clang (Apple clang ships no libclang_rt.fuzzer_osx.a) and we do not
# want the product build to depend on it. Outputs go to build-fuzz/ (isolated from
# build/, build-cov/, build-llvmcov/).
#
# Usage:
#   tests/fuzz/build_fuzzers.sh            # build all harnesses
#   tests/fuzz/build_fuzzers.sh wordpiece  # build one (substring match)
#
# Run a built fuzzer, e.g. (corpus/ is gitignored scratch; seeds/ is the tracked
# starting corpus -- pass both so the fuzzer grows corpus/ from the seeds):
#   mkdir -p tests/fuzz/corpus/wordpiece
#   build-fuzz/fuzz_wordpiece_basic_tokenize -max_total_time=60 \
#       tests/fuzz/corpus/wordpiece tests/fuzz/seeds/wordpiece
#
# Replay a saved crash:
#   build-fuzz/fuzz_flatten_metadata tests/fuzz/crashes/flatten_deep_nesting_stackoverflow.json
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

CLANGXX="${CLANGXX:-/opt/homebrew/opt/llvm/bin/clang++}"
OUT="build-fuzz"
mkdir -p "$OUT"

# nlohmann/json single-include header (header-only; pulled from a configured build).
JSON_INC=""
for cand in build-cov/_deps/json-src/single_include \
            build/_deps/json-src/single_include \
            build-llvmcov/_deps/json-src/single_include; do
    if [[ -f "$cand/nlohmann/json.hpp" ]]; then JSON_INC="$cand"; break; fi
done
if [[ -z "$JSON_INC" ]]; then
    echo "ERROR: nlohmann/json.hpp not found under any build*/_deps/json-src. Configure a build first." >&2
    exit 1
fi

if [[ ! -x "$CLANGXX" ]]; then
    echo "ERROR: homebrew clang++ not found at $CLANGXX (brew install llvm)." >&2
    exit 1
fi
# Hard fail early if the fuzzer runtime is missing (Apple clang has none).
if ! ls "$(dirname "$(dirname "$CLANGXX")")"/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a >/dev/null 2>&1; then
    echo "ERROR: libclang_rt.fuzzer_osx.a not found for $CLANGXX -- wrong toolchain." >&2
    exit 1
fi

# -fsanitize=fuzzer,address,undefined: coverage-guided fuzzing + ASan (OOB/UAF) +
# UBSan (signed overflow, invalid casts, ...). -fno-sanitize-recover=all so UBSan
# aborts (libFuzzer captures it as a crash) instead of printing-and-continuing.
SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"
CXXFLAGS="-std=c++20 -g -O1 -fno-omit-frame-pointer $SAN -I include -I $JSON_INC"

FILTER="${1:-}"

build_one() {
    local name="$1"; shift
    if [[ -n "$FILTER" && "$name" != *"$FILTER"* ]]; then return 0; fi
    echo ">>> building $name"
    # shellcheck disable=SC2086
    "$CLANGXX" $CXXFLAGS "$@" -o "$OUT/$name"
    echo "    -> $OUT/$name"
}

H="tests/fuzz"

# 1) WordPiece BasicTokenize -- compiles the real tokenizer TU (no vocab/IO needed).
build_one fuzz_wordpiece_basic_tokenize \
    "$H/fuzz_wordpiece_basic_tokenize.cpp" \
    src/query/wordpiece_tokenizer.cpp

# 2) Single-NS query body: json::parse + QueryRequest::FromJson + Normalize + Validate.
#    Links the real query_request TU (+ status.cpp for any out-of-line Status bits).
build_one fuzz_query_request_fromjson \
    "$H/fuzz_query_request_fromjson.cpp" \
    src/query/query_request.cpp \
    src/common/status.cpp

# 3) Cross-NS body parse (verbatim ParseRequest copy -- see harness header drift guard).
build_one fuzz_cross_ns_parse_request \
    "$H/fuzz_cross_ns_parse_request.cpp" \
    src/common/status.cpp

# 4) FlattenMetadataIntoMap (verbatim copy -- see harness header drift guard).
build_one fuzz_flatten_metadata \
    "$H/fuzz_flatten_metadata.cpp"

echo "All requested fuzzers built into $OUT/"
