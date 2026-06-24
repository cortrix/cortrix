#!/usr/bin/env bash
# =============================================================================
# One-click model + parser provisioning for a LOCAL (non-container) Cortrix run.
#
# Mirrors what deploy/entrypoint.sh does automatically inside the container:
#   1. download the ONNX models into ./models       (bge-m3 always)
#   2. create the docling/paddleocr parser venv at  ./scripts/ocr_venv
#
# Usage (from the repo root):
#   ./scripts/setup_models.sh                 # full: models + parser stack
#   ./scripts/setup_models.sh --models-only   # ONNX models only, skip parser venv
#   ./scripts/setup_models.sh --parser-only   # parser venv only, skip downloads
#
# reranker / query-complexity have no public ONNX download. Set their tarball
# URLs to auto-fetch; otherwise reranker falls back to a stub and F39 query
# complexity falls back to the heuristic backend:
#   CORTRIX_RERANKER_MODEL_URL=...  \
#   CORTRIX_QUERY_COMPLEXITY_MODEL_URL=...  ./scripts/setup_models.sh
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODELS_DIR="$ROOT/models"
OCR_VENV="$ROOT/scripts/ocr_venv"
PY="${PYTHON:-python3.12}"

DO_MODELS=1
DO_PARSER=1
case "${1:-}" in
    --models-only) DO_PARSER=0 ;;
    --parser-only) DO_MODELS=0 ;;
    "") ;;
    *) echo "Unknown argument: $1"; exit 1 ;;
esac

echo "=== Cortrix local model setup ==="
echo "  repo root : $ROOT"
echo "  models    : $MODELS_DIR"
echo "  parser env: $OCR_VENV"
echo ""

# ---- 1. ONNX models ----
if [ "$DO_MODELS" = 1 ]; then
    echo "[1/2] Downloading ONNX models (bge-m3 ~2GB; first run may take 10-20 min)…"
    bash "$ROOT/deploy/download-models.sh" "$MODELS_DIR"
else
    echo "[1/2] Skipped (--parser-only)"
fi

# ---- 2. docling/paddleocr parser venv ----
if [ "$DO_PARSER" = 1 ]; then
    if [ ! -x "$OCR_VENV/bin/python3" ]; then
        echo "[2/2] Creating parser venv ($PY) + installing docling/paddleocr (~2GB)…"
        "$PY" -m venv "$OCR_VENV"
        "$OCR_VENV/bin/pip" install --quiet --upgrade pip
        "$OCR_VENV/bin/pip" install -r "$ROOT/scripts/requirements-parser.txt"
    else
        echo "[2/2] Parser venv already exists, skipping"
    fi
else
    echo "[2/2] Skipped (--models-only)"
fi

echo ""
echo "=== Done ==="
echo "Models present:"
for d in bge-m3 bge-reranker-v2-m3 query-complexity; do
    if [ -f "$MODELS_DIR/$d/model.onnx" ]; then echo "  [x] $d"; else echo "  [ ] $d (absent → stub/heuristic fallback)"; fi
done
[ -x "$OCR_VENV/bin/python3" ] && echo "  [x] parser venv (docling/paddleocr)" || echo "  [ ] parser venv"
echo ""
echo "Next: ./dev.sh   (set LLM keys in build/config.yaml for the enrich/agent roles)"
