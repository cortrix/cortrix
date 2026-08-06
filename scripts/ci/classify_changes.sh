#!/usr/bin/env bash
# classify_changes.sh — decide whether a change set is documentation-only.
#
# Usage: classify_changes.sh <base-ref> <head-ref>
# Output (stdout, last line): docs_only=true | docs_only=false
#
# Fail-closed by design: docs_only=true only when the change set is non-empty
# AND every changed file matches the pure-documentation allow list. Everything
# else — unknown paths, empty diffs, executable content under docs/ — is
# docs_only=false and runs the full lane. Any edit to this script or to the
# workflows lives outside the allow list, so changing the classifier itself
# always triggers a full run.
set -euo pipefail
BASE="${1:?base ref required}"
HEAD="${2:?head ref required}"

# Pure-documentation allow list (only these paths can be judged docs-only).
DOCS_ALLOW='^(docs/.*\.(md|png|jpg|jpeg|svg|gif)|[^/]*\.md|LICENSE|NOTICE|\.github/(ISSUE_TEMPLATE|PULL_REQUEST_TEMPLATE)/.*)$'
# Deny list: even under docs/, executable content never counts as docs-only.
DOCS_DENY='\.(sh|bash|py|js|ts|yml|yaml|cmake|mk)$|(^|/)(Dockerfile|Makefile|CMakeLists\.txt)$'

FILES="$(git diff --name-only "$BASE...$HEAD")"

# An empty diff is not docs-only (fail closed).
if [ -z "$FILES" ]; then echo "docs_only=false"; exit 0; fi

while IFS= read -r f; do
  [ -z "$f" ] && continue
  if printf '%s' "$f" | grep -qE "$DOCS_DENY"; then echo "docs_only=false"; exit 0; fi
  if ! printf '%s' "$f" | grep -qE "$DOCS_ALLOW"; then echo "docs_only=false"; exit 0; fi
done <<< "$FILES"

echo "docs_only=true"
