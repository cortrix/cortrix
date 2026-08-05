#!/usr/bin/env bash
# =============================================================
# llvm-cov region-coverage cross-check ("calibration")
# for the gcov/lcov branch metric
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
case "$MODE" in
  --report-only|--check) ;;
  *)
    echo "usage: $0 [--report-only|--check]" >&2
    exit 2
    ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-llvmcov"
OUT="$BUILD/coverage"
PROF="$BUILD/prof"

# Test suite dual-track gate: this script owns the core-17 BRANCH gate on the
# clang region/branch metric (the line gates are owned by coverage.sh on gcov).
CORE_BRANCH_MIN=80
CORE_TARGET_TU_EXPECTED=317
CORE_MAPPED_TU_EXPECTED=314
CORE_TARGET_SOURCE_SHA256_EXPECTED="4e86934e2109eff7f536e4e538e9ae033109bf7fbb167aa0404c59fc7437b978"

CORE_DIRS=(
  src/store src/reranker src/spc src/query src/resource src/async
  src/catalog src/observability src/agent_trace src/logging src/ml src/memory
)

find_llvm_tool() {
  local tool="$1"
  if command -v "$tool" >/dev/null 2>&1; then
    command -v "$tool"
  elif command -v xcrun >/dev/null 2>&1; then
    xcrun --find "$tool"
  else
    echo "required LLVM tool not found: $tool" >&2
    return 1
  fi
}

COV_CC_PATH="$(find_llvm_tool "${COV_CC:-clang}")"
COV_CXX_PATH="$(find_llvm_tool "${COV_CXX:-clang++}")"
LLVM_PROFDATA_PATH="$(find_llvm_tool "${LLVM_PROFDATA:-llvm-profdata}")"
LLVM_COV_PATH="$(find_llvm_tool "${LLVM_COV:-llvm-cov}")"

for tool_path in "$COV_CC_PATH" "$COV_CXX_PATH" "$LLVM_PROFDATA_PATH" \
                 "$LLVM_COV_PATH"; do
  if [ ! -x "$tool_path" ]; then
    echo "required LLVM tool is not executable: $tool_path" >&2
    exit 1
  fi
done

LINKER_FLAGS=(-fprofile-instr-generate)
if [ "$(uname -s)" = "Linux" ]; then
  LLD_PATH="$(find_llvm_tool "${COV_LLD:-ld.lld}")"
  if [ ! -x "$LLD_PATH" ]; then
    echo "required low-memory linker is not executable: $LLD_PATH" >&2
    exit 1
  fi
  LINKER_FLAGS+=("-B$(dirname "$LLD_PATH")" "-fuse-ld=lld" "-Wl,--threads=1")
fi
LINKER_FLAGS_STRING="${LINKER_FLAGS[*]}"

CXX_VERSION_LINE="$("$COV_CXX_PATH" --version | sed -n '1p')"
PROFDATA_VERSION_LINE="$("$LLVM_PROFDATA_PATH" show --version | sed -n '1p')"
COV_VERSION_LINE="$("$LLVM_COV_PATH" --version | sed -n '1p')"
printf '%s\n%s\n%s\n' \
  "$CXX_VERSION_LINE" "$PROFDATA_VERSION_LINE" "$COV_VERSION_LINE"

version_major() {
  printf '%s\n' "$1" | sed -E -n 's/^[^0-9]*([0-9]+).*/\1/p'
}
CXX_VERSION_MAJOR="$(version_major "$CXX_VERSION_LINE")"
PROFDATA_VERSION_MAJOR="$(version_major "$PROFDATA_VERSION_LINE")"
COV_VERSION_MAJOR="$(version_major "$COV_VERSION_LINE")"
if [ -z "$CXX_VERSION_MAJOR" ] || [ -z "$PROFDATA_VERSION_MAJOR" ] ||
   [ -z "$COV_VERSION_MAJOR" ] ||
   [ "$CXX_VERSION_MAJOR" != "$PROFDATA_VERSION_MAJOR" ] ||
   [ "$CXX_VERSION_MAJOR" != "$COV_VERSION_MAJOR" ]; then
  echo "Clang and LLVM coverage tool major versions must match" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then
  LLD_VERSION_LINE="$("$LLD_PATH" --version | sed -n '1p')"
  printf '%s\n' "$LLD_VERSION_LINE"
  LLD_VERSION_MAJOR="$(version_major "$LLD_VERSION_LINE")"
  if [ -z "$LLD_VERSION_MAJOR" ] || [ "$CXX_VERSION_MAJOR" != "$LLD_VERSION_MAJOR" ]; then
    echo "Clang and LLD major versions must match" >&2
    exit 1
  fi
fi

echo "── configure (clang source-based coverage)"
# Reuse the onnxruntime already fetched into the main build/ to avoid a slow,
# flaky re-download (see coverage.sh). No-op in CI where build/_deps is absent.
ONNX_SRC="$ROOT/build/_deps/onnxruntime-src"
ONNX_ARG=()
[ -d "$ONNX_SRC" ] && ONNX_ARG=(-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME="$ONNX_SRC")
# Reuse googletest already fetched into the main build/ too: a fresh build dir
# otherwise re-populates it via FetchContent, which is flaky (download step fails).
GTEST_SRC="$ROOT/build/_deps/googletest-src"
GTEST_ARG=()
[ -d "$GTEST_SRC" ] && GTEST_ARG=(-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$GTEST_SRC")
cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_COMPILER="$COV_CC_PATH" \
  -DCMAKE_CXX_COMPILER="$COV_CXX_PATH" \
  -DCMAKE_C_FLAGS_DEBUG="-O0 -g0" \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g0" \
  -DCMAKE_EXE_LINKER_FLAGS="$LINKER_FLAGS_STRING" \
  "${ONNX_ARG[@]}" "${GTEST_ARG[@]}" >/dev/null

echo "── build (cortrix_unit_tests)"
# Instrumented compiles take 2-3 GB per TU; cap at COV_JOBS (default 2 => <10 GB)
# to avoid OOM on a 16 GB box (see coverage.sh). A cloud VM can raise COV_JOBS.
cmake --build "$BUILD" -j "${COV_JOBS:-2}" --target cortrix_unit_tests >/dev/null

echo "── run unit suite (profile merge pool %5m)"
mkdir -p "$PROF" "$OUT"
rm -f "$PROF"/*.profraw "$PROF"/merged.profdata
# %5m = online-merged pool of 5 files: constant disk footprint across
# 4000+ single-process ctest runs (one .profraw per pid would be ~10s of GB).
# Exclude timing/concurrency tests (same rationale + list as coverage.sh):
# P-HNSW soak (slow), GcThreadTest.LoopRunsSweep (env-flaky SEGFAULT under
# instrumentation), NamespacePool StartupLoadEightWorkers... (elapsed<2s bound
# blown by -O0). All gated for correctness by the Release ctest run.
LLVM_PROFILE_FILE="$PROF/%5m.profraw" \
  ctest --test-dir "$BUILD" -L unit \
    -E 'PHnswConcurrencyTest|GcThreadTest.LoopRunsSweep|NamespacePoolTest.StartupLoadEightWorkersConcurrency' \
    --output-on-failure

echo "── merge + report"
"$LLVM_PROFDATA_PATH" merge -sparse "$PROF"/*.profraw -o "$PROF/merged.profdata"

# project code only, same scope as coverage.sh: src/ + include/, minus
# vendored hnswlib; tests and _deps never match (paths under build/_deps
# or tests/ are excluded by the regex).
IGNORE='(/tests/|/_deps/|/hnswlib/|/build[^/]*/)'

# Report every instrumented cortrix_core translation unit, including archive
# members that the unit-test linker did not pull in. LLVM 14 rejects a static
# archive when even one member has no coverage map, so probe the fixed target
# object list and pass mapped objects explicitly. The allowlist is fail-closed:
# adding code to an empty placeholder or adding a new empty TU requires review.
python3 - "$ROOT" "$BUILD/compile_commands.json" "$LLVM_COV_PATH" \
  "$PROF/merged.profdata" "$OUT" "$CORE_TARGET_TU_EXPECTED" \
  "$CORE_MAPPED_TU_EXPECTED" "$CORE_TARGET_SOURCE_SHA256_EXPECTED" <<'PY'
import hashlib
import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
compile_commands_path = Path(sys.argv[2])
build_dir = compile_commands_path.parent.resolve()
llvm_cov = sys.argv[3]
profile = Path(sys.argv[4])
out = Path(sys.argv[5])
expected_target_count = int(sys.argv[6])
expected_mapped_count = int(sys.argv[7])
expected_source_sha256 = sys.argv[8]
zero_map_allowlist = {
    "src/reranker/reranker_thread_pool.cpp",
    "src/server/routes/health_routes.cpp",
    "src/server/routes/namespace_routes.cpp",
}

entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
target_objects = []
for entry in entries:
    output = entry.get("output", "")
    command = entry.get("command", "")
    if "CMakeFiles/cortrix_core.dir/" not in output and \
       "CMakeFiles/cortrix_core.dir/" not in command:
        continue
    source = Path(entry["file"])
    if not source.is_absolute():
        source = Path(entry["directory"]) / source
    object_path = Path(output)
    if not object_path.is_absolute():
        object_path = Path(entry["directory"]) / object_path
    source = source.resolve()
    object_path = object_path.resolve()
    source_rel = source.relative_to(root).as_posix()
    if not object_path.is_file():
        raise SystemExit(f"missing coverage object: {object_path}")
    target_objects.append((source_rel, object_path))

if not target_objects:
    raise SystemExit("no cortrix_core objects found in compile_commands.json")
if len(target_objects) != expected_target_count:
    raise SystemExit(
        f"cortrix_core TU count changed: expected={expected_target_count} "
        f"actual={len(target_objects)}"
    )
if len({source for source, _ in target_objects}) != len(target_objects):
    raise SystemExit("duplicate cortrix_core source in compile_commands.json")
if len({obj for _, obj in target_objects}) != len(target_objects):
    raise SystemExit("duplicate cortrix_core object in compile_commands.json")

source_manifest_text = "".join(
    f"{source}\n" for source, _ in sorted(target_objects)
)
source_manifest_sha256 = hashlib.sha256(
    source_manifest_text.encode("utf-8")
).hexdigest()
if source_manifest_sha256 != expected_source_sha256:
    raise SystemExit(
        "cortrix_core source universe changed: "
        f"expected_sha256={expected_source_sha256} "
        f"actual_sha256={source_manifest_sha256}"
    )

mapped = []
zero_map = []
for source, object_path in sorted(target_objects):
    probe = subprocess.run(
        [llvm_cov, "report", str(object_path), f"-instr-profile={profile}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if probe.returncode == 0:
        mapped.append((source, object_path))
    elif "No coverage data found" in probe.stderr:
        zero_map.append(source)
    else:
        raise SystemExit(
            f"coverage-map probe failed for {source}: {probe.stderr.strip()}"
        )

if set(zero_map) != zero_map_allowlist:
    raise SystemExit(
        "zero-map translation-unit set changed: "
        f"expected={sorted(zero_map_allowlist)} actual={sorted(zero_map)}"
    )
if not mapped:
    raise SystemExit("no mapped cortrix_core objects found")
if len(mapped) != expected_mapped_count:
    raise SystemExit(
        f"mapped cortrix_core TU count changed: expected={expected_mapped_count} "
        f"actual={len(mapped)}"
    )

out.mkdir(parents=True, exist_ok=True)
(out / "core_target_sources.txt").write_text(
    source_manifest_text, encoding="utf-8"
)
(out / "core_target_sources.sha256").write_text(
    f"{source_manifest_sha256}  core_target_sources.txt\n", encoding="utf-8"
)
manifest_text = (
    "source\tobject\tcoverage_mapping\n" +
    "".join(
        f"{source}\t{object_path.relative_to(build_dir).as_posix()}\t"
        f"{'no' if source in zero_map_allowlist else 'yes'}\n"
        for source, object_path in sorted(target_objects)
    )
)
manifest_path = out / "coverage_object_manifest.tsv"
manifest_path.write_text(manifest_text, encoding="utf-8")
manifest_sha256 = hashlib.sha256(manifest_text.encode("utf-8")).hexdigest()
(out / "coverage_object_manifest.sha256").write_text(
    f"{manifest_sha256}  {manifest_path.name}\n", encoding="utf-8"
)
(out / "mapped_object_paths.txt").write_text(
    "".join(f"{object_path}\n" for _, object_path in mapped),
    encoding="utf-8",
)
(out / "mapped_source_paths.txt").write_text(
    "".join(f"{source}\n" for source, _ in mapped),
    encoding="utf-8",
)
print(
    f"coverage object universe: {len(target_objects)} target TUs, "
    f"{len(mapped)} mapped, {len(zero_map)} reviewed empty placeholders, "
    f"manifest sha256={manifest_sha256}"
)
PY

PRIMARY_OBJECT=""
COV_OBJECT_ARGS=()
while IFS= read -r object_path; do
  if [ -z "$PRIMARY_OBJECT" ]; then
    PRIMARY_OBJECT="$object_path"
  else
    COV_OBJECT_ARGS+=(-object "$object_path")
  fi
done < "$OUT/mapped_object_paths.txt"
if [ -z "$PRIMARY_OBJECT" ]; then
  echo "no mapped coverage objects" >&2
  exit 1
fi

"$LLVM_COV_PATH" report "$PRIMARY_OBJECT" "${COV_OBJECT_ARGS[@]}" \
  -instr-profile="$PROF/merged.profdata" \
  -ignore-filename-regex="$IGNORE" > "$OUT/report.txt"
"$LLVM_COV_PATH" export "$PRIMARY_OBJECT" "${COV_OBJECT_ARGS[@]}" \
  -instr-profile="$PROF/merged.profdata" -summary-only \
  -ignore-filename-regex="$IGNORE" > "$OUT/coverage_source_export.json"

python3 - "$ROOT" "$OUT/mapped_source_paths.txt" \
  "$OUT/coverage_source_export.json" "$OUT/core_source_manifest.txt" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
expected = {
    line for line in Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()
    if line
}
payload = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
translation_unit_suffixes = {".c", ".cc", ".cpp", ".cxx"}

observed = set()
for datum in payload.get("data", []):
    for item in datum.get("files", []):
        path = Path(item["filename"])
        if not path.is_absolute():
            path = root / path
        try:
            relative = path.resolve().relative_to(root).as_posix()
        except ValueError:
            continue
        if relative.startswith("src/") and Path(relative).suffix in translation_unit_suffixes:
            observed.add(relative)

missing = expected - observed
unexpected = observed - expected
if missing or unexpected:
    raise SystemExit(
        "coverage source universe mismatch: "
        f"missing={sorted(missing)} unexpected={sorted(unexpected)}"
    )
Path(sys.argv[4]).write_text(
    "".join(f"{source}\n" for source in sorted(observed)), encoding="utf-8"
)
print(f"coverage source universe: {len(observed)} mapped production TUs")
PY

# core-17 aggregate: llvm-cov has no directory grouping, so sum the
# per-file Regions/Branches columns for files under CORE_DIRS.
CORE_DIR_LIST="${CORE_DIRS[*]}"
awk -v core_dirs="$CORE_DIR_LIST" -v counts_path="$OUT/core_branch_counts.txt" '
  BEGIN {
    n = split(core_dirs, a, " ")
    for (i = 1; i <= n; i++) core[a[i] "/"] = 1
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
    printf "%d %d\n", b_tot - b_miss, b_tot > counts_path
    if (r_tot > 0)
      printf "core-17 : regions %d/%d missed (%.2f%% cov) · branches %d/%d missed (%.2f%% cov)\n",
        r_miss, r_tot, (1-r_miss/r_tot)*100, b_miss, b_tot, (1-b_miss/b_tot)*100
  }
' "$OUT/report.txt" | tee "$OUT/summary.txt"

echo "full table: $OUT/report.txt"

if [ "$MODE" = "--check" ]; then
  read -r CORE_BRANCH_COVERED CORE_BRANCH_TOTAL < "$OUT/core_branch_counts.txt"
  if ! [[ "$CORE_BRANCH_COVERED" =~ ^[0-9]+$ ]] ||
     ! [[ "$CORE_BRANCH_TOTAL" =~ ^[0-9]+$ ]] ||
     [ "$CORE_BRANCH_TOTAL" -eq 0 ]; then
    echo "GATE FAIL: invalid core-17 branch counts"
    exit 1
  fi
  CORE_BRANCH=$(awk -v c="$CORE_BRANCH_COVERED" -v t="$CORE_BRANCH_TOTAL" \
    'BEGIN {printf "%.2f", c * 100 / t}')
  echo ""
  printf "  core-17 branch : %6s%%  (gate ≥ %s%%, clang region/branch — F23 §4.1.bis)\n" \
    "$CORE_BRANCH" "$CORE_BRANCH_MIN"
  if [ $((CORE_BRANCH_COVERED * 100)) -ge \
       $((CORE_BRANCH_MIN * CORE_BRANCH_TOTAL)) ]; then
    echo "BRANCH GATE: PASS"
  else
    echo "GATE FAIL: core-17 branch $CORE_BRANCH% < $CORE_BRANCH_MIN%"
    exit 1
  fi
fi
