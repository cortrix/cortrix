#!/bin/bash
set -e

# ==============================================================
#  Cortrix Container Entrypoint
#  env setup → pre-flight checks → data dir init → supervisord
# ==============================================================

echo "============================================"
echo "  Cortrix v1.0.0"
echo "  Semantic Storage for the Agentic Era"
echo "============================================"

# ---------- 1. Environment defaults ----------
export CORTRIX_DATA_DIR="${CORTRIX_DATA_DIR:-/data}"
export CORTRIX_HTTP_PORT="${CORTRIX_HTTP_PORT:-8420}"
export CORTRIX_LOG_LEVEL="${CORTRIX_LOG_LEVEL:-info}"
export CORTRIX_ENABLE_MEMORY="${CORTRIX_ENABLE_MEMORY:-true}"

export CORTRIX_LLM_PROVIDER="${CORTRIX_LLM_PROVIDER:-openai}"
export CORTRIX_LLM_MODEL="${CORTRIX_LLM_MODEL:-gpt-4o-mini}"
export CORTRIX_LLM_BASE_URL="${CORTRIX_LLM_BASE_URL:-https://api.openai.com/v1}"
export CORTRIX_LLM_API_KEY="${CORTRIX_LLM_API_KEY:-}"

export CORTRIX_EMBEDDING_MODEL="${CORTRIX_EMBEDDING_MODEL:-bge-m3-onnx}"
export CORTRIX_EMBEDDING_DEVICE="${CORTRIX_EMBEDDING_DEVICE:-cpu}"
export CORTRIX_SPC_WORKERS="${CORTRIX_SPC_WORKERS:-2}"
export CORTRIX_SPC_QUEUE_SIZE="${CORTRIX_SPC_QUEUE_SIZE:-100}"

export CORTRIX_WEB_UI_ENABLED="${CORTRIX_WEB_UI_ENABLED:-true}"
export CORTRIX_WEB_UI_PATH="${CORTRIX_WEB_UI_PATH:-/ui}"
# The C++ server mounts the SPA at "/" when this dir is set (bootstrap.cpp).
if [ "$CORTRIX_WEB_UI_ENABLED" = "true" ]; then
    export CORTRIX_WEB_UI_DIR="${CORTRIX_WEB_UI_DIR:-/app/web-ui}"
fi
# ---------- 1b. First-run provisioning (PROFILE != lite) ----------
# Models and the docling/paddleocr parser stack are NOT baked into the image;
# on first run (PROFILE=full, the default) they are provisioned into the /data
# volume so they persist across restarts (only the first boot pays the cost).
# PROFILE=lite skips this: txt/md ingestion + BM25 still work, embedding runs in
# stub mode, and PDF/image parsing is unavailable.
export CORTRIX_PROFILE="${CORTRIX_PROFILE:-full}"
CORTRIX_MODELS_DIR="$CORTRIX_DATA_DIR/models"
CORTRIX_OCR_VENV="$CORTRIX_DATA_DIR/ocr_venv"
# Keep parser model caches on the volume (docling→HF hub, paddleocr→PaddleX).
export HF_HOME="${HF_HOME:-$CORTRIX_DATA_DIR/cache/huggingface}"
export PADDLE_PDX_CACHE_HOME="${PADDLE_PDX_CACHE_HOME:-$CORTRIX_DATA_DIR/cache/paddlex}"
mkdir -p "$CORTRIX_MODELS_DIR" "$HF_HOME" "$PADDLE_PDX_CACHE_HOME" 2>/dev/null || true

if [ "$CORTRIX_PROFILE" = "lite" ]; then
    echo "PROFILE=lite — skipping model/parser provisioning"
    echo "  (txt/md + BM25 available; embedding=stub; PDF/image parsing off)"
else
    # (1) ONNX models → data volume. bge-m3 always; reranker/query-complexity
    #     only when their *_MODEL_URL env is set (see download-models.sh).
    if [ ! -f "$CORTRIX_MODELS_DIR/bge-m3/model.onnx" ]; then
        echo "[provision] fetching ONNX models into $CORTRIX_MODELS_DIR"
        echo "            (first run only; bge-m3 ~2GB, may take 10-20 min)…"
        bash /app/scripts/download-models.sh "$CORTRIX_MODELS_DIR" \
            || echo "WARN: model fetch incomplete — affected features degrade (embedding/reranker stub, heuristic complexity)"
    fi
    # (2) docling/paddleocr parser stack → venv on the data volume.
    if [ ! -x "$CORTRIX_OCR_VENV/bin/python3" ]; then
        echo "[provision] creating parser venv + installing docling/paddleocr"
        echo "            (first run only; ~2GB, may take several minutes)…"
        if python3 -m venv "$CORTRIX_OCR_VENV"; then
            "$CORTRIX_OCR_VENV/bin/pip" install --no-cache-dir -q --upgrade pip >/dev/null 2>&1 || true
            "$CORTRIX_OCR_VENV/bin/pip" install --no-cache-dir -q -r /app/scripts/requirements-parser.txt \
                || echo "WARN: parser stack install failed — PDF/image ingestion off (text formats still work)"
        else
            echo "WARN: parser venv creation failed — PDF/image ingestion off"
        fi
    fi
fi

# F02 reranker (real model when provisioned/bind-mounted; absent = stub).
if [ -d "${CORTRIX_RERANKER_MODEL_DIR:-/data/models/bge-reranker-v2-m3}" ]; then
    export CORTRIX_RERANKER_MODEL_DIR="${CORTRIX_RERANKER_MODEL_DIR:-/data/models/bge-reranker-v2-m3}"
fi

# F39/F37 query-complexity classifier (D3.5 r2 #26): point the server at the
# bind-mounted model dir when present; absent = heuristic fallback by design.
if [ -d "${CORTRIX_QUERY_COMPLEXITY_MODEL_DIR:-/data/models/query-complexity}" ]; then
    export CORTRIX_QUERY_COMPLEXITY_MODEL_DIR="${CORTRIX_QUERY_COMPLEXITY_MODEL_DIR:-/data/models/query-complexity}"
fi

export CORTRIX_AGENT_ENABLED="${CORTRIX_AGENT_ENABLED:-true}"
export CORTRIX_AGENT_PORT="${CORTRIX_AGENT_PORT:-8000}"
# Same-origin agent surface (F48 P-3): the C++ server reverse-proxies
# /api/v1/agent/* and /agent/* to the cortrix-agent service.
if [ "$CORTRIX_AGENT_ENABLED" = "true" ]; then
    export CORTRIX_AGENT_BASE_URL="${CORTRIX_AGENT_BASE_URL:-http://127.0.0.1:${CORTRIX_AGENT_PORT}}"
fi


# ---------- 2. Pre-flight checks ----------

# 2a. Data directory writable?
if [ ! -d "$CORTRIX_DATA_DIR" ] || [ ! -w "$CORTRIX_DATA_DIR" ]; then
    echo "ERROR: $CORTRIX_DATA_DIR is not writable."
    echo "  Mount a volume:  docker run -v /host/path:/data ..."
    exit 1
fi

# 2b. LLM key
if [ -z "$CORTRIX_LLM_API_KEY" ]; then
    echo "WARN: CORTRIX_LLM_API_KEY is not set — LLM features will be unavailable."
fi

# 2c. Core binary
if [ ! -x "/app/cortrix_server" ]; then
    echo "ERROR: /app/cortrix_server not found or not executable."
    exit 1
fi


# ---------- 3. Data directory init ----------
mkdir -p "$CORTRIX_DATA_DIR/blobs" \
         "$CORTRIX_DATA_DIR/p-hnsw" \
         "$CORTRIX_DATA_DIR/p-hnsw/wal" \
         "$CORTRIX_DATA_DIR/logs" 2>/dev/null || true

# First-run DB will be created by cortrix_server on startup

# ---------- 4. Print config summary ----------
echo ""
echo "Configuration:"
echo "  Data Dir ......  $CORTRIX_DATA_DIR"
echo "  HTTP Port .....  $CORTRIX_HTTP_PORT"
echo "  Log Level .....  $CORTRIX_LOG_LEVEL"
echo "  LLM Provider ..  $CORTRIX_LLM_PROVIDER ($CORTRIX_LLM_MODEL)"
echo "  Embedding .....  $CORTRIX_EMBEDDING_MODEL ($CORTRIX_EMBEDDING_DEVICE)"
echo "  SPC Workers ...  $CORTRIX_SPC_WORKERS"
echo "  Web UI ........  $CORTRIX_WEB_UI_ENABLED"
echo "  Agent .........  $CORTRIX_AGENT_ENABLED (port $CORTRIX_AGENT_PORT)"
echo "  Memory ........  $CORTRIX_ENABLE_MEMORY"
echo ""

# ---------- 5. Dispatch ----------
case "${1:-start}" in
    start)
        echo "Starting services via supervisord …"
        exec /usr/bin/supervisord -n -c /etc/supervisor/conf.d/cortrix.conf
        ;;
    healthcheck)
        exec /app/healthcheck.sh
        ;;
    shell)
        exec /bin/bash
        ;;
    *)
        exec "$@"
        ;;
esac
