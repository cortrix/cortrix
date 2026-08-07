#!/usr/bin/env bash
# test_classify_changes.sh — executable cases for scripts/ci/classify_changes.sh.
#
# Each case builds a scratch git repo, commits a base, applies the case's file
# set on a branch, runs the classifier over base...head and compares the
# verdict. Cases 8, 10 and 11 pin the fail-open defect of the first classifier
# draft (unknown paths and executable content must never be judged docs-only);
# a regression on any case fails this script and with it the CI job.
set -euo pipefail

CLASSIFIER="$(cd "$(dirname "$0")/../.." && pwd)/scripts/ci/classify_changes.sh"
[ -x "$CLASSIFIER" ] || { echo "classifier not executable: $CLASSIFIER"; exit 1; }

PASS=0
FAIL=0

# run_case <name> <expected docs_only> <file>...
# With no files, the diff is empty (base == head).
run_case() {
  local name="$1" expected="$2"; shift 2
  local dir
  dir="$(mktemp -d)"
  (
    cd "$dir"
    git init -q
    git -c user.name=t -c user.email=t@t config commit.gpgsign false
    git config user.name t
    git config user.email t@t
    echo base > base.txt
    git add base.txt
    git commit -qm base
    git branch -m main
    git checkout -qb change
    local f
    for f in "$@"; do
      mkdir -p "$(dirname "$f")"
      echo x > "$f"
      git add "$f"
    done
    if [ "$#" -gt 0 ]; then git commit -qm change; fi
    "$CLASSIFIER" main change | tail -1
  ) > "$dir/out" 2>"$dir/err" || { echo "ERROR  $name (classifier crashed)"; cat "$dir/err"; FAIL=$((FAIL+1)); rm -rf "$dir"; return; }
  local got
  got="$(tail -1 "$dir/out")"
  if [ "$got" = "docs_only=$expected" ]; then
    echo "ok     $name -> $got"
    PASS=$((PASS+1))
  else
    echo "FAIL   $name -> got '$got', want 'docs_only=$expected'"
    FAIL=$((FAIL+1))
  fi
  rm -rf "$dir"
}

run_case "01 root markdown"                false README.md  # S2 VERIFICATION: deliberately wrong, reverted before close
run_case "02 docs markdown"                true  docs/usage/quickstart.md
run_case "03 LICENSE"                      true  LICENSE
run_case "04 two doc files"                true  README.md docs/a.md
run_case "05 mixed doc + source"           false README.md src/main.cpp
run_case "06 root Dockerfile"              false Dockerfile
run_case "07 root Makefile"                false Makefile
run_case "08 github action definition"     false .github/actions/foo/action.yml
run_case "09 dependency manifest"          false requirements.txt
run_case "10 shell script under docs/"     false docs/examples/run.sh
run_case "11 unknown path"                 false newdir/anything.txt
run_case "12 empty diff"                   false
run_case "13 docs image"                   true  docs/img/arch.png
run_case "14 workflow file"                false .github/workflows/pr-ci.yml

echo "----"
echo "passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ]
