#!/usr/bin/env bash
# =============================================================
# F23 llvm-cov region-coverage cross-check ("calibration")
# (design: dev-cortrix-hub F23-tests.md §4.1.bis — measurement
#  calibration for the gcov/lcov branch metric)
#
# Why: gcov emits hidden branch arms for compiler-generated
# constructs (temporary construction in string concatenation,
# short-circuit cleanup paths) even under no_exception_branch,
# deflating the branch% denominator with arms no test can hit.
# Clang source-based coverage (region + branch counts from the
# coverage mapping) has no such phantom arms, so it is the
# calibration reference for the core-17 branch gate.
#
# Usage:  scripts/ci/llvmcov.sh
# Output: build-llvmcov/coverage/report.txt   (per-file table)
#         build-llvmcov/coverage/summary.txt  (overall + core-17)
# =============================================================
set -euo pipefail

MODE="${1:---report-only}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-llvmcov"
OUT="$BUILD/coverage"
PROF="$BUILD/prof"

# F23 §4.1.bis dual-track gate: this script owns the core-17 BRANCH gate on the
# clang region/branch metric (the line gates are owned by coverage.sh on gcov).
CORE_BRANCH_MIN=80

CORE_DIRS=(
  src/store src/reranker src/spc src/query src/resource src/async
  src/catalog src/observability src/agent_trace src/logging src/ml src/memory
)

echo "── configure (clang source-based coverage)"
cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -O0" \
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" >/dev/null

echo "── build (cortrix_unit_tests)"
cmake --build "$BUILD" --parallel --target cortrix_unit_tests >/dev/null

echo "── run unit suite (profile merge pool %5m)"
mkdir -p "$PROF" "$OUT"
rm -f "$PROF"/*.profraw "$PROF"/merged.profdata
# %5m = online-merged pool of 5 files: constant disk footprint across
# 4000+ single-process ctest runs (one .profraw per pid would be ~10s of GB).
# Exclude the P-HNSW concurrency SOAK suite (same rationale as coverage.sh):
# thread-stress tests, ~zero unique region/branch coverage, pathologically slow
# under -O0 instrumentation. Gated for correctness by the Release ctest run.
LLVM_PROFILE_FILE="$PROF/%5m.profraw" \
  ctest --test-dir "$BUILD" -L unit -E 'PHnswConcurrencyTest' --output-on-failure

echo "── merge + report"
xcrun llvm-profdata merge -sparse "$PROF"/*.profraw -o "$PROF/merged.profdata"

BIN="$BUILD/tests/cortrix_unit_tests"
# project code only, same scope as coverage.sh: src/ + include/, minus
# vendored hnswlib; tests and _deps never match (paths under build/_deps
# or tests/ are excluded by the regex).
IGNORE='(/tests/|/_deps/|/hnswlib/|/build[^/]*/)'
xcrun llvm-cov report "$BIN" -instr-profile="$PROF/merged.profdata" \
  -ignore-filename-regex="$IGNORE" > "$OUT/report.txt"

# core-17 aggregate: llvm-cov has no directory grouping, so sum the
# per-file Regions/Branches columns for files under CORE_DIRS.
awk -v root="$ROOT" '
  BEGIN {
    split("store reranker spc query resource async catalog observability agent_trace logging ml memory", a, " ")
    for (i in a) core["src/" a[i] "/"] = 1
  }
  /^-+$/ { next }
  /^Filename/ { next }
  /^TOTAL/ {
    printf "overall : regions %s/%s missed (%.2f%% cov) · branches %s/%s missed (%.2f%% cov)\n",
      $3, $2, (1-$3/$2)*100, $(NF-1), $(NF-2), (1-$(NF-1)/$(NF-2))*100
    next
  }
  NF > 10 {
    f = $1
    iscore = 0
    for (p in core) if (index(f, p) > 0) iscore = 1
    if (iscore) {
      r_tot += $2; r_miss += $3
      b_tot += $(NF-2); b_miss += $(NF-1)
    }
  }
  END {
    if (r_tot > 0)
      printf "core-17 : regions %d/%d missed (%.2f%% cov) · branches %d/%d missed (%.2f%% cov)\n",
        r_miss, r_tot, (1-r_miss/r_tot)*100, b_miss, b_tot, (1-b_miss/b_tot)*100
  }
' "$OUT/report.txt" | tee "$OUT/summary.txt"

echo "full table: $OUT/report.txt"

if [ "$MODE" = "--check" ]; then
  # Parse "core-17 : ... branches N/M missed (XX.XX% cov)" from summary.txt.
  # Portable (BSD awk has no 3-arg match): split on "branches", strip to the %.
  CORE_BRANCH=$(awk '/^core-17/ {split($0,a,"branches"); m=a[2];
                     sub(/.*\(/,"",m); sub(/%.*/,"",m); print m}' "$OUT/summary.txt")
  echo ""
  printf "  core-17 branch : %6s%%  (gate ≥ %s%%, clang region/branch — F23 §4.1.bis)\n" \
    "$CORE_BRANCH" "$CORE_BRANCH_MIN"
  if awk -v v="$CORE_BRANCH" -v m="$CORE_BRANCH_MIN" 'BEGIN{exit !(v+0 >= m)}'; then
    echo "BRANCH GATE: PASS"
  else
    echo "GATE FAIL: core-17 branch $CORE_BRANCH% < $CORE_BRANCH_MIN%"
    exit 1
  fi
fi
