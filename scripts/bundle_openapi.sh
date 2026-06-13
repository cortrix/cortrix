#!/usr/bin/env bash
# bundle_openapi.sh — merge api/openapi.yaml + components/* + paths/* → build/openapi.bundled.yaml
#
# The artifact build/openapi.bundled.yaml = the single source of truth for the whole
# Cortrix API, consumed by:
#   - Swagger UI  (cortrix-server /docs)
#   - P03 Python SDK build-time types/_generated.py
#   - Schemathesis contract tests (S7)
#
# Usage:
#   scripts/bundle_openapi.sh          # bundle + lint
#   scripts/bundle_openapi.sh --no-lint
#
# Dependency: @redocly/cli (if not preinstalled, `npm i -g @redocly/cli`, or this script falls back to npx)
# SoT: P04 § 2.3 + § 6.3
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SPEC="${REPO_ROOT}/api/openapi.yaml"
OUT_DIR="${REPO_ROOT}/build"
OUT="${OUT_DIR}/openapi.bundled.yaml"
DO_LINT=1

for arg in "$@"; do
  case "$arg" in
    --no-lint) DO_LINT=0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Pick how to invoke redocly: use it directly if installed globally, otherwise npx (first run fetches the package).
if command -v redocly >/dev/null 2>&1; then
  REDOCLY="redocly"
else
  echo "[bundle] redocly not installed globally, falling back to npx @redocly/cli (downloads on first run)" >&2
  REDOCLY="npx --yes @redocly/cli"
fi

mkdir -p "${OUT_DIR}"

echo "[bundle] ${SPEC} -> ${OUT}"
${REDOCLY} bundle "${SPEC}" --output "${OUT}" --ext yaml

if [ "${DO_LINT}" -eq 1 ]; then
  echo "[lint] ${OUT}"
  ${REDOCLY} lint "${OUT}"
fi

echo "[bundle] done: ${OUT}"
