#!/usr/bin/env bash
# Offline tests for scripts/ci/check_core_universe.py, the PR-time check that
# the cortrix_core source universe pinned in the nightly coverage scripts still
# matches CMakeLists.txt. Each case runs the check against a copy of the three
# files it reads, edited one way.
set -euo pipefail
cd "$(dirname "$0")/../.."
SCRIPT=scripts/ci/check_core_universe.py
PY="${PYTHON:-python3}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0

fixture() {
  local dir="$TMP/$1"
  mkdir -p "$dir/scripts/ci"
  cp CMakeLists.txt "$dir/"
  cp scripts/ci/coverage.sh scripts/ci/llvmcov.sh "$dir/scripts/ci/"
  echo "$dir"
}

expect_ok() {
  local name="$1" dir="$2"
  if "$PY" "$SCRIPT" --root "$dir" >"$TMP/out" 2>"$TMP/err"; then
    PASS=$((PASS+1))
  else
    FAIL=$((FAIL+1)); echo "FAIL (expected pass): $name"; cat "$TMP/err"
  fi
}
# A rejection is a non-zero exit AND the script's own failure line: a crash
# (traceback) also exits non-zero, and must not pass for a decision.
expect_reject() {
  local name="$1" dir="$2" needle="$3"
  if "$PY" "$SCRIPT" --root "$dir" >"$TMP/out" 2>"$TMP/err"; then
    FAIL=$((FAIL+1)); echo "FAIL (expected reject): $name"
  elif grep -q "core universe check FAILED" "$TMP/err" && grep -qF "$needle" "$TMP/err"; then
    PASS=$((PASS+1))
  else
    FAIL=$((FAIL+1)); echo "FAIL (wrong rejection): $name"; cat "$TMP/err"
  fi
}

# The tree as committed.
expect_ok "committed tree" "$(fixture committed)"

# A new core source without a pin update: the case nightly used to catch alone.
d="$(fixture added)"
sed -i.bak 's|^add_library(cortrix_core STATIC$|&\n    src/common/added_for_test.cpp|' "$d/CMakeLists.txt"
expect_reject "added source" "$d" "CORE_TARGET_TU_EXPECTED"

# A source swapped for another keeps the count but changes the manifest.
d="$(fixture swapped)"
first="$(sed -n '/^add_library(cortrix_core STATIC$/,/^)$/p' CMakeLists.txt | grep -m1 -oE 'src/[^ ]+\.cpp')"
sed -i.bak "s|^\([[:space:]]*\)${first}\([[:space:]]*\)$|\1src/common/swapped_for_test.cpp\2|" "$d/CMakeLists.txt"
expect_reject "swapped source" "$d" "CORE_TARGET_SOURCE_SHA256_EXPECTED"

# Only one of the two scripts updated.
d="$(fixture one-script)"
sed -i.bak 's/^CORE_TARGET_TU_EXPECTED=.*/CORE_TARGET_TU_EXPECTED=1/' "$d/scripts/ci/llvmcov.sh"
expect_reject "stale llvmcov.sh pin" "$d" "scripts/ci/llvmcov.sh: CORE_TARGET_TU_EXPECTED=1"

# The mapped count must stay the target count minus the zero-map allowlist.
d="$(fixture mapped)"
sed -i.bak 's/^CORE_MAPPED_TU_EXPECTED=.*/CORE_MAPPED_TU_EXPECTED=1/' "$d/scripts/ci/llvmcov.sh"
expect_reject "stale mapped count" "$d" "CORE_MAPPED_TU_EXPECTED=1"

# Sources added outside add_library are not seen here, so they are refused.
d="$(fixture target-sources)"
printf '\ntarget_sources(cortrix_core PRIVATE src/common/elsewhere.cpp)\n' >> "$d/CMakeLists.txt"
expect_reject "target_sources" "$d" "target_sources(cortrix_core"

echo "check_core_universe: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
