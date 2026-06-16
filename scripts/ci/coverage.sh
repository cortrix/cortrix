#!/usr/bin/env bash
# =============================================================
# F23 coverage toolchain + quality gate
# (design: dev-cortrix-hub F23-tests.md §4.1 / §4.2 / §4.3)
#
# Usage:
#   scripts/ci/coverage.sh --report-only   # build+test+HTML, no gate
#   scripts/ci/coverage.sh --check         # + enforce thresholds (hard gate)
#
# Thresholds (topic 2 rev B, Phase 1 V1.0):
#   overall line ≥ 80% · core-17 line ≥ 90% · core-17 branch ≥ 80%
#
# Core-17 feature → source dir map (§4.2). F14 pgcortrix (PG extension,
# separate build) and F24 (shell/deploy artifacts) cannot be instrumented
# by this C++ run and are validated by their own suites.
# =============================================================
set -euo pipefail

MODE="${1:---report-only}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-cov"
OUT="$BUILD/coverage"

OVERALL_LINE_MIN=80
CORE_LINE_MIN=90
CORE_BRANCH_MIN=80

# F01(store/phnsw) F25(store) F02 F03+F07(spc) F04(query) F05(resource)
# F08(async) F12(catalog) F13(observability+agent_trace) F18a(logging)
# F22(ml) MEM02/03/05(memory)
CORE_DIRS=(
  src/store src/reranker src/spc src/query src/resource src/async
  src/catalog src/observability src/agent_trace src/logging src/ml src/memory
)

echo "── configure (coverage instrumented)"
# Reuse the onnxruntime already fetched into the main build/ to avoid a slow,
# flaky re-download into this instrumented build dir. No-op in CI (or any clean
# checkout) where build/_deps is absent — FetchContent then downloads as usual.
ONNX_SRC="$ROOT/build/_deps/onnxruntime-src"
ONNX_ARG=()
[ -d "$ONNX_SRC" ] && ONNX_ARG=(-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME="$ONNX_SRC")
cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="--coverage -O0" \
  -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
  "${ONNX_ARG[@]}" >/dev/null

# Build ONLY the unit-suite binary: the gate runs `ctest -L unit`, so the full
# `all` target would waste instrumented-build time on integration/benchmark
# binaries this run never executes (benchmarks are F44 domain, #481 cloud).
echo "── build (cortrix_unit_tests)"
cmake --build "$BUILD" --parallel --target cortrix_unit_tests >/dev/null

echo "── run unit suite (one process per test via ctest)"
# Zero stale counters first: on an incremental build, leftover .gcda from a
# prior run mixed with re-compiled .gcno silently drops whole files from the
# capture (mismatch), deflating coverage.
find "$BUILD" -name "*.gcda" -delete
# Exclude timing/concurrency tests from the coverage run only. (1) The P-HNSW
# concurrency SOAK suite re-runs the SAME read/write/lock lines under contention
# (~zero UNIQUE coverage) and is pathologically slow under -O0 +--coverage
# (Concurrent_ReadDuringWrite alone: 47 min instrumented vs ~seconds in Release).
# (2) GcThreadTest.LoopRunsSweep is a known env-flaky GC-thread timing test that
# SEGFAULTs under instrumentation. (3) NamespacePool StartupLoadEightWorkers...
# asserts an elapsed<2s wall-clock bound that -O0 instrumentation always blows.
# All three stay gated for CORRECTNESS by the Release `ctest -L unit` run; this
# exclusion is coverage-measurement only (one flaky abort must not kill the gate).
COV_EXCLUDE='PHnswConcurrencyTest|GcThreadTest.LoopRunsSweep|NamespacePoolTest.StartupLoadEightWorkersConcurrency'
echo "   (coverage-only exclusion: -E '$COV_EXCLUDE' — soak tests, gated in Release)"
ctest --test-dir "$BUILD" -L unit -E "$COV_EXCLUDE" --output-on-failure

echo "── capture lcov"
mkdir -p "$OUT"
# Branch gate metric = no-exception-branch (F23 §4.1.bis): gcov's raw mode
# counts 2 branches per potentially-throwing call site (C++ exception edges),
# diluting the denominator ~58% with mostly-untriggerable arms.
# inconsistent/format/count/deprecated etc.: lcov 2.x added strict checks that
# libc++ system headers and gmock's compiler-generated functions trip
# constantly; ignore them all (1.x behavior).
LCOV_OPTS=(--rc branch_coverage=1 --rc no_exception_branch=1 --rc max_message_count=10
           --filter branch,function
           --ignore-errors mismatch,negative,unused,inconsistent,deprecated,format,count,empty,source,unsupported,graph,path)
lcov "${LCOV_OPTS[@]}" --capture --directory "$BUILD" --output-file "$OUT/raw.info" >/dev/null
# project code only: drop system headers, fetched deps, tests themselves.
# Also drop vendored third-party sources living inside src/ (hnswlib ships its
# own LICENSE/README under src/store/phnsw/hnswlib) — the gate measures OUR
# code, not upstream's.
lcov "${LCOV_OPTS[@]}" --extract "$OUT/raw.info" "$ROOT/src/*" "$ROOT/include/*" \
  --output-file "$OUT/all_pre.info" >/dev/null
lcov "${LCOV_OPTS[@]}" --remove "$OUT/all_pre.info" "*/hnswlib/*" \
  --output-file "$OUT/all.info" >/dev/null

core_patterns=()
for d in "${CORE_DIRS[@]}"; do core_patterns+=("$ROOT/$d/*"); done
lcov "${LCOV_OPTS[@]}" --extract "$OUT/all.info" "${core_patterns[@]}" \
  --output-file "$OUT/core17.info" >/dev/null

genhtml --branch-coverage "$OUT/all.info" --output-directory "$OUT/html" >/dev/null 2>&1 || true

pct() { # pct <info-file> <lines|branches>
  # lcov 2.x summary line: "  lines.......: 91.5% (27133 of 29654 lines)".
  # Match field 1 = "<key>...:" (dots vary) and take the % off field 2.
  lcov --rc branch_coverage=1 --ignore-errors inconsistent,format,count --summary "$1" 2>&1 \
    | awk -v k="$2" '$1 ~ "^"k"\\.*:" {gsub(/%/,"",$2); print $2; exit}'
}

OVERALL_LINE=$(pct "$OUT/all.info" "lines" || echo 0)
CORE_LINE=$(pct "$OUT/core17.info" "lines" || echo 0)
CORE_BRANCH=$(pct "$OUT/core17.info" "branches" || echo 0)

echo ""
echo "================ Coverage Summary ================"
printf "  overall  line   : %6s%%  (gate ≥ %s%%)\n" "$OVERALL_LINE" "$OVERALL_LINE_MIN"
printf "  core-17  line   : %6s%%  (gate ≥ %s%%)\n" "$CORE_LINE" "$CORE_LINE_MIN"
printf "  core-17  branch : %6s%%  (gcov — INFORMATIONAL only)\n" "$CORE_BRANCH"
echo "  html: $OUT/html/index.html"
echo "=================================================="
echo "  NOTE (F23 §4.1.bis dual-track gate): this script gates the LINE metrics on"
echo "  gcov (where line counting is accurate). The BRANCH gate is enforced by"
echo "  scripts/ci/llvmcov.sh on the clang region/branch metric — gcov's branch %"
echo "  above is phantom-inflated (no_exception filter can't strip mixed-line arms)"
echo "  and is shown for reference only, NOT gated here."

if [ "$MODE" = "--check" ]; then
  fail=0
  awk -v v="$OVERALL_LINE" -v m="$OVERALL_LINE_MIN" 'BEGIN{exit !(v+0 >= m)}' \
    || { echo "GATE FAIL: overall line $OVERALL_LINE% < $OVERALL_LINE_MIN%"; fail=1; }
  awk -v v="$CORE_LINE" -v m="$CORE_LINE_MIN" 'BEGIN{exit !(v+0 >= m)}' \
    || { echo "GATE FAIL: core-17 line $CORE_LINE% < $CORE_LINE_MIN%"; fail=1; }
  # core-17 branch is NOT gated here (dual-track) — run scripts/ci/llvmcov.sh --check.
  [ "$fail" -eq 0 ] && echo "LINE GATE: PASS (branch gate → llvmcov.sh)" || exit 1
fi
