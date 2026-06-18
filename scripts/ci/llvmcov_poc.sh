#!/usr/bin/env bash
# =============================================================
# R9 Stage-2 PoC — does merging the INTEGRATION test binary's coverage into the
# core-17 clang branch metric move the needle on the integration-only arms
# (live_single_unit_executor / http_server / memory_routes / namespace_pool)?
#
# Difference from llvmcov.sh: builds + runs cortrix_integration_tests too and reports
# across BOTH objects. Real-model integration tests self-skip without a model present.
# Reuses the existing build-llvmcov/ (incremental: only the integration binary is new).
# Throwaway PoC — the canonical llvmcov.sh is untouched until we decide to land this.
# =============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-llvmcov"
OUT="$BUILD/coverage-poc"
PROF="$BUILD/prof-poc"

ONNX_SRC="$ROOT/build/_deps/onnxruntime-src"
ONNX_ARG=()
[ -d "$ONNX_SRC" ] && ONNX_ARG=(-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME="$ONNX_SRC")
echo "── configure (reuse build-llvmcov instrumentation)"
cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -O0" \
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" \
  "${ONNX_ARG[@]}" >/dev/null

echo "── build (unit + integration)"
cmake --build "$BUILD" -j "${COV_JOBS:-2}" \
  --target cortrix_unit_tests cortrix_integration_tests >/dev/null

mkdir -p "$PROF" "$OUT"
rm -f "$PROF"/*.profraw "$PROF"/merged.profdata

echo "── run unit suite"
LLVM_PROFILE_FILE="$PROF/u-%5m.profraw" \
  ctest --test-dir "$BUILD" -L unit \
    -E 'PHnswConcurrencyTest|GcThreadTest.LoopRunsSweep|NamespacePoolTest.StartupLoadEightWorkersConcurrency' \
    --output-on-failure >/dev/null 2>&1 || echo "  (some unit tests failed — coverage still collected)"

echo "── run integration suite (real-model tests self-skip without a model)"
LLVM_PROFILE_FILE="$PROF/i-%5m.profraw" \
  ctest --test-dir "$BUILD" -L integration \
    --output-on-failure >/dev/null 2>&1 || echo "  (some integration tests failed/skipped — coverage still collected)"

echo "── merge profiles (unit-only + unit∪integration)"
# Keep the two comparisons on the SAME denominator. The earlier naive run reported
# unit-only against `unit` (16820 branches) and merged against `unit -object int`
# (17034) — different region sets (the two binaries instantiate the same headers'
# templates/inline differently), so 82.81 vs 83.04 was apples-to-oranges, NOT a
# coverage regression. Fix: report BOTH profiles against the SAME object set
# (`unit -object int`), so the denominator is fixed and the delta is pure coverage.
xcrun llvm-profdata merge -sparse "$PROF"/u-*.profraw -o "$PROF/unit.profdata"
xcrun llvm-profdata merge -sparse "$PROF"/*.profraw -o "$PROF/merged.profdata"

UNIT_BIN="$BUILD/tests/cortrix_unit_tests"
INT_BIN="$BUILD/tests/cortrix_integration_tests"
IGNORE='(/tests/|/_deps/|/hnswlib/|/build[^/]*/)'

# Same object set for both → identical denominator; only the instr-profile differs.
xcrun llvm-cov report "$UNIT_BIN" -object "$INT_BIN" \
  -instr-profile="$PROF/unit.profdata" \
  -ignore-filename-regex="$IGNORE" > "$OUT/report_unit.txt"
xcrun llvm-cov report "$UNIT_BIN" -object "$INT_BIN" \
  -instr-profile="$PROF/merged.profdata" \
  -ignore-filename-regex="$IGNORE" > "$OUT/report.txt"

core_branch() {  # $1 = report file → prints "missed total"
  awk '
    BEGIN { split("store reranker spc query resource async catalog observability agent_trace logging ml memory", a, " ");
            for (i in a) core["src/" a[i] "/"] = 1 }
    /^-+$/ { next } /^Filename/ { next } /^TOTAL/ { next }
    NF > 10 { f=$1; ic=0; for (p in core) if (index(f,p)>0) ic=1;
              if (ic) { tot += $(NF-2); miss += $(NF-1) } }
    END { printf "%d %d", miss, tot }
  ' "$1"
}
read U_MISS U_TOT <<<"$(core_branch "$OUT/report_unit.txt")"
read M_MISS M_TOT <<<"$(core_branch "$OUT/report.txt")"
{
  awk -v um="$U_MISS" -v ut="$U_TOT" 'BEGIN{printf "core-17 unit-only        : branches %d/%d missed (%.2f%% cov)\n", um, ut, (1-um/ut)*100}'
  awk -v mm="$M_MISS" -v mt="$M_TOT" 'BEGIN{printf "core-17 unit+integration : branches %d/%d missed (%.2f%% cov)\n", mm, mt, (1-mm/mt)*100}'
  awk -v um="$U_MISS" -v mm="$M_MISS" 'BEGIN{printf "→ integration covered %d more core-17 branches (same denominator)\n", um-mm}'
} | tee "$OUT/summary.txt"

echo "── integration-gap files (branch cov after unit+integration merge):"
grep -iE "live_single_unit_executor|server/http_server.cpp|memory/memory_routes.cpp|resource/namespace_pool.cpp" "$OUT/report.txt" || true
echo "full table: $OUT/report.txt"
