#!/usr/bin/env bash
# =============================================================
# Coverage toolchain + quality gate
#
# Usage:
#   scripts/ci/coverage.sh --report-only   # build+test+HTML, no gate
#   scripts/ci/coverage.sh --check         # + enforce thresholds (hard gate)
#
# Thresholds (topic 2 rev B, Phase 1 V1.0):
#   overall line ≥ 80% · core-17 line ≥ 90% · core-17 branch ≥ 80%
#
# Core-17 feature → source dir map (§4.2). pgcortrix (PG extension,
# separate build) and deployment (shell/deploy artifacts) cannot be instrumented
# by this C++ run and are validated by their own suites.
# =============================================================
set -euo pipefail

MODE="${1:---report-only}"
case "$MODE" in
  --report-only|--check) ;;
  *)
    echo "usage: $0 [--report-only|--check]" >&2
    exit 2
    ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-cov"
OUT="$BUILD/coverage"

OVERALL_LINE_MIN=80
CORE_LINE_MIN=90
CORE_TARGET_TU_EXPECTED=317
CORE_TARGET_SOURCE_SHA256_EXPECTED="4e86934e2109eff7f536e4e538e9ae033109bf7fbb167aa0404c59fc7437b978"

# Index(store/phnsw) write coordinator(store) reranker enricher+semantic score(spc) cross-NS query(query) namespace pool(resource)
# META block(async) catalog(catalog) agent trace(observability+agent_trace) operation log(logging)
# ONNX runtime(ml) memory features(memory)
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
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_CXX_FLAGS="--coverage -O0" \
  -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
  "${ONNX_ARG[@]}" >/dev/null

# Build ONLY the unit-suite binary: the gate runs `ctest -L unit`, so the full
# `all` target would waste instrumented-build time on integration/benchmark
# binaries this run never executes (benchmarks are benchmark domain, #481 cloud).
echo "── build (cortrix_unit_tests)"
# Instrumented (-O0 + coverage mapping) compiles take 2-3 GB PER translation unit;
# the default --parallel (= all 10 cores here) OOMs a 16 GB box (~30 GB peak →
# crash). Cap at COV_JOBS (default 2 => <10 GB). A big-memory CI / cloud VM can
# raise it (COV_JOBS=<cores>) for full speed.
cmake --build "$BUILD" -j "${COV_JOBS:-2}" --target cortrix_unit_tests >/dev/null

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
# Branch gate metric = no-exception-branch (test suite): gcov's raw mode
# counts 2 branches per potentially-throwing call site (C++ exception edges),
# diluting the denominator ~58% with mostly-untriggerable arms.
#
# Ubuntu 22.04 ships lcov 1.15, while newer runners may provide lcov 2.x.
# Options such as --filter and the extended --ignore-errors taxonomy only exist
# in 2.x. Keep the same source extraction, exclusions, and line thresholds on
# both versions; the lcov branch value is informational and the hard branch gate
# remains owned by llvmcov.sh.
if lcov --help 2>&1 | grep -q -- '--filter'; then
  echo "   lcov mode: 2.x filters"
  LCOV_OPTS=(--rc branch_coverage=1 --rc no_exception_branch=1 --rc max_message_count=10
             --filter "branch,function"
             --ignore-errors "mismatch,negative,unused,inconsistent,deprecated,format,count,empty,source,unsupported,graph,path,range")
  LCOV_SUMMARY_OPTS=(--rc branch_coverage=1
                     --ignore-errors "inconsistent,format,count,range")
else
  echo "   lcov mode: 1.x compatibility"
  LCOV_OPTS=(--rc lcov_branch_coverage=1)
  LCOV_SUMMARY_OPTS=(--rc lcov_branch_coverage=1)
fi
# Capture an initial zero-hit baseline before merging the executed counters. This
# keeps built production objects that the static-library linker did not pull into
# the unit binary in the line denominator instead of silently dropping them.
lcov "${LCOV_OPTS[@]}" --capture --initial --directory "$BUILD" \
  --output-file "$OUT/base.info" >/dev/null
lcov "${LCOV_OPTS[@]}" --capture --directory "$BUILD" \
  --output-file "$OUT/run.info" >/dev/null
lcov "${LCOV_OPTS[@]}" --add-tracefile "$OUT/base.info" \
  --add-tracefile "$OUT/run.info" --output-file "$OUT/raw.info" >/dev/null
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

# Fail closed if lcov omits, adds, or swaps a production translation unit. The
# three allowlisted files are intentional empty placeholders and therefore have
# no gcov line records; every other cortrix_core TU must appear in all.info.
python3 - "$ROOT" "$BUILD/compile_commands.json" "$OUT/all.info" \
  "$OUT/core17.info" "$OUT" "$CORE_TARGET_TU_EXPECTED" \
  "$CORE_TARGET_SOURCE_SHA256_EXPECTED" "${CORE_DIRS[@]}" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
compile_commands_path = Path(sys.argv[2])
all_info_path = Path(sys.argv[3])
core_info_path = Path(sys.argv[4])
out = Path(sys.argv[5])
expected_count = int(sys.argv[6])
expected_sha256 = sys.argv[7]
core_dirs = tuple(f"{value}/" for value in sys.argv[8:])
zero_line_allowlist = {
    "src/reranker/reranker_thread_pool.cpp",
    "src/server/routes/health_routes.cpp",
    "src/server/routes/namespace_routes.cpp",
}
translation_unit_suffixes = {".c", ".cc", ".cpp", ".cxx"}

entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
target_sources = set()
for entry in entries:
    output = entry.get("output", "")
    command = entry.get("command", "")
    if "CMakeFiles/cortrix_core.dir/" not in output and \
       "CMakeFiles/cortrix_core.dir/" not in command:
        continue
    source = Path(entry["file"])
    if not source.is_absolute():
        source = Path(entry["directory"]) / source
    target_sources.add(source.resolve().relative_to(root).as_posix())

source_manifest_text = "".join(f"{source}\n" for source in sorted(target_sources))
source_manifest_sha256 = hashlib.sha256(
    source_manifest_text.encode("utf-8")
).hexdigest()
if len(target_sources) != expected_count or source_manifest_sha256 != expected_sha256:
    raise SystemExit(
        "cortrix_core source universe changed: "
        f"expected_count={expected_count} actual_count={len(target_sources)} "
        f"expected_sha256={expected_sha256} actual_sha256={source_manifest_sha256}"
    )
if not zero_line_allowlist <= target_sources:
    raise SystemExit("line-coverage zero-map allowlist is not a subset of cortrix_core")

def info_translation_units(path):
    observed = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("SF:"):
            continue
        source = Path(line[3:])
        if not source.is_absolute():
            source = root / source
        try:
            relative = source.resolve().relative_to(root).as_posix()
        except ValueError:
            continue
        if relative.startswith("src/") and source.suffix in translation_unit_suffixes:
            observed.add(relative)
    return observed

expected_mapped = target_sources - zero_line_allowlist
observed_all = info_translation_units(all_info_path)
if observed_all != expected_mapped:
    raise SystemExit(
        "line coverage source universe mismatch: "
        f"missing={sorted(expected_mapped - observed_all)} "
        f"unexpected={sorted(observed_all - expected_mapped)}"
    )

expected_core = {source for source in expected_mapped if source.startswith(core_dirs)}
observed_core = info_translation_units(core_info_path)
if observed_core != expected_core:
    raise SystemExit(
        "core-17 line coverage source universe mismatch: "
        f"missing={sorted(expected_core - observed_core)} "
        f"unexpected={sorted(observed_core - expected_core)}"
    )

observed_text = "".join(f"{source}\n" for source in sorted(observed_all))
observed_sha256 = hashlib.sha256(observed_text.encode("utf-8")).hexdigest()
(out / "core_target_sources.txt").write_text(source_manifest_text, encoding="utf-8")
(out / "core_target_sources.sha256").write_text(
    f"{source_manifest_sha256}  core_target_sources.txt\n", encoding="utf-8"
)
(out / "line_source_manifest.txt").write_text(observed_text, encoding="utf-8")
(out / "line_source_manifest.sha256").write_text(
    f"{observed_sha256}  line_source_manifest.txt\n", encoding="utf-8"
)
print(
    f"line coverage source universe: {len(observed_all)} mapped production TUs, "
    f"{len(observed_core)} core-17 TUs, manifest sha256={observed_sha256}"
)
PY

genhtml --branch-coverage "$OUT/all.info" --output-directory "$OUT/html" >/dev/null 2>&1 || true

pct() { # pct <info-file> <lines|branches>
  # lcov 2.x summary line: "  lines.......: 91.5% (27133 of 29654 lines)".
  # Match field 1 = "<key>...:" (dots vary) and take the % off field 2.
  lcov "${LCOV_SUMMARY_OPTS[@]}" --summary "$1" 2>&1 \
    | awk -v k="$2" '$1 ~ "^"k"\\.*:" {gsub(/%/,"",$2); print $2; exit}'
}

line_counts() { # line_counts <info-file> -> "hit found"
  awk -F: '
    $1 == "LF" { found += $2 }
    $1 == "LH" { hit += $2 }
    END { printf "%d %d\n", hit, found }
  ' "$1"
}

read -r OVERALL_LINE_HIT OVERALL_LINE_FOUND <<<"$(line_counts "$OUT/all.info")"
read -r CORE_LINE_HIT CORE_LINE_FOUND <<<"$(line_counts "$OUT/core17.info")"
if [ "$OVERALL_LINE_FOUND" -eq 0 ] || [ "$CORE_LINE_FOUND" -eq 0 ]; then
  echo "GATE FAIL: invalid zero-sized line coverage universe"
  exit 1
fi
OVERALL_LINE=$(awk -v h="$OVERALL_LINE_HIT" -v f="$OVERALL_LINE_FOUND" \
  'BEGIN {printf "%.2f", h * 100 / f}')
CORE_LINE=$(awk -v h="$CORE_LINE_HIT" -v f="$CORE_LINE_FOUND" \
  'BEGIN {printf "%.2f", h * 100 / f}')
CORE_BRANCH=$(pct "$OUT/core17.info" "branches" || echo 0)

echo ""
echo "================ Coverage Summary ================"
printf "  overall  line   : %6s%%  (gate ≥ %s%%)\n" "$OVERALL_LINE" "$OVERALL_LINE_MIN"
printf "  core-17  line   : %6s%%  (gate ≥ %s%%)\n" "$CORE_LINE" "$CORE_LINE_MIN"
printf "  core-17  branch : %6s%%  (gcov — INFORMATIONAL only)\n" "$CORE_BRANCH"
echo "  html: $OUT/html/index.html"
echo "=================================================="
echo "  NOTE (test plan §4.1.bis dual-track gate): this script gates the LINE metrics on"
echo "  gcov (where line counting is accurate). The BRANCH gate is enforced by"
echo "  scripts/ci/llvmcov.sh on the clang region/branch metric — gcov's branch %"
echo "  above is phantom-inflated (no_exception filter can't strip mixed-line arms)"
echo "  and is shown for reference only, NOT gated here."

if [ "$MODE" = "--check" ]; then
  fail=0
  [ $((OVERALL_LINE_HIT * 100)) -ge \
    $((OVERALL_LINE_MIN * OVERALL_LINE_FOUND)) ] \
    || { echo "GATE FAIL: overall line $OVERALL_LINE% < $OVERALL_LINE_MIN%"; fail=1; }
  [ $((CORE_LINE_HIT * 100)) -ge \
    $((CORE_LINE_MIN * CORE_LINE_FOUND)) ] \
    || { echo "GATE FAIL: core-17 line $CORE_LINE% < $CORE_LINE_MIN%"; fail=1; }
  # core-17 branch is NOT gated here (dual-track) — run scripts/ci/llvmcov.sh --check.
  [ "$fail" -eq 0 ] && echo "LINE GATE: PASS (branch gate → llvmcov.sh)" || exit 1
fi
