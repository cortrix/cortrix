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
# Keep parser model caches on the volume so they persist across restarts.
# DEFECT#7 root cause: docling's layout/TableFormer models load via huggingface_hub
# into HF_HOME. On a fresh volume HF_HOME is empty, so the FIRST PDF parse downloads
# ~0.5GB from the HF Hub on the hot path; unauthenticated (no HF_TOKEN) that is
# rate-limited to ~100-150s, which blows the 60s parser subprocess timeout → the
# document errors as "All parsers failed". HF_HOME is already on the volume, but an
# empty cache still pays the download on first use — so (2b) below pre-warms it
# during provisioning, and after provisioning we flip the runtime to offline so a
# warm cache never pays online revalidation latency. The server exports these before
# launch and posix_spawn passes environ through, so the docling_bridge subprocess
# inherits them. DOCLING_CACHE_DIR pins docling's own scratch dir (sentinel lives
# there); the actual models cache under HF_HOME.
export HF_HOME="${HF_HOME:-$CORTRIX_DATA_DIR/cache/huggingface}"
export PADDLE_PDX_CACHE_HOME="${PADDLE_PDX_CACHE_HOME:-$CORTRIX_DATA_DIR/cache/paddlex}"
export DOCLING_CACHE_DIR="${DOCLING_CACHE_DIR:-$CORTRIX_DATA_DIR/cache/docling}"
mkdir -p "$CORTRIX_MODELS_DIR" "$HF_HOME" "$PADDLE_PDX_CACHE_HOME" "$DOCLING_CACHE_DIR" 2>/dev/null || true

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
    # Self-healing gate: re-provision whenever docling is not importable. The old
    # `-x python3` gate skipped repair forever once the venv directory existed, so a
    # venv left half-installed by an interrupted/failed pip run never recovered on a
    # later boot — the import probe catches that case too.
    if ! "$CORTRIX_OCR_VENV/bin/python3" -c "import docling" >/dev/null 2>&1; then
        echo "[provision] creating parser venv + installing docling/paddleocr"
        echo "            (first run only; ~2GB, may take several minutes)…"
        [ -x "$CORTRIX_OCR_VENV/bin/python3" ] || python3 -m venv "$CORTRIX_OCR_VENV"
        if [ -x "$CORTRIX_OCR_VENV/bin/python3" ]; then
            # DEFECT#8: a single `pip install` does not survive a flaky network.
            # pip's --retries re-downloads each wheel from scratch, so a connection
            # that truncates mid-stream (IncompleteRead) never completes a large
            # wheel (torch ~426MB) regardless of retry count. A persistent wheel
            # cache on the volume lets fully-downloaded wheels accumulate ACROSS
            # invocations, so a bounded outer loop converges (verified: a single
            # call with --retries 15 failed against both PyPI and a mirror; the loop
            # succeeded on attempt 2). The cache also lets container recreates skip
            # already-downloaded wheels.
            export PIP_CACHE_DIR="$CORTRIX_DATA_DIR/cache/pip"
            mkdir -p "$PIP_CACHE_DIR" 2>/dev/null || true
            "$CORTRIX_OCR_VENV/bin/pip" install -q --upgrade pip >/dev/null 2>&1 || true
            _attempt=1
            while [ "$_attempt" -le 8 ]; do
                "$CORTRIX_OCR_VENV/bin/pip" install -q --retries 5 --timeout 120 \
                    -r /app/scripts/requirements-parser.txt && break
                echo "[provision] parser install attempt $_attempt/8 failed (network); retrying with cached wheels…"
                _attempt=$((_attempt + 1))
                sleep 3
            done
            "$CORTRIX_OCR_VENV/bin/python3" -c "import docling" >/dev/null 2>&1 \
                || echo "WARN: parser stack install failed after 8 attempts — PDF/image ingestion off (text formats still work)"
        else
            echo "WARN: parser venv creation failed — PDF/image ingestion off"
        fi
    fi
    # (2b) Pre-warm the docling model cache (HF_HOME) by running a REAL parse of a
    #      tiny image PDF, so the FIRST user upload doesn't pay the one-time HF Hub
    #      download on the hot path and blow the 60s parser timeout (DEFECT#7). A
    #      real DocumentConverter().convert() is used on purpose — download_models()
    #      fetches a different artifact layout than the converter loads, leaving the
    #      runtime still hitting the HF Hub; the real converter populates exactly the
    #      blobs the runtime needs (verified: warm parse ~1.5-3s, 0 HF-hub calls).
    #      Gated on a sentinel so it runs once per volume; failure is non-fatal
    #      (runtime falls back to lazy online download).
    if [ -x "$CORTRIX_OCR_VENV/bin/python3" ] && [ ! -f "$DOCLING_CACHE_DIR/.warmed" ]; then
        echo "[provision] pre-warming docling model cache (real parse of a sample PDF)"
        echo "            (first run only; downloads layout+OCR models, a few minutes)…"
        if "$CORTRIX_OCR_VENV/bin/python3" - <<'PYWARM'
import tempfile, os
from PIL import Image, ImageDraw  # Pillow ships as a docling dependency
img = Image.new("RGB", (640, 200), "white")
ImageDraw.Draw(img).text((24, 90), "Cortrix docling warmup sample 0123456789", fill="black")
sample = os.path.join(tempfile.gettempdir(), "docling_warmup.pdf")
img.save(sample, "PDF", resolution=150.0)
# Same code path as the runtime bridge: default DocumentConverter (layout + OCR).
from docling.document_converter import DocumentConverter
DocumentConverter().convert(sample)
print("docling warmup parse OK")
PYWARM
        then
            touch "$DOCLING_CACHE_DIR/.warmed"
            echo "[provision] docling model cache warm (HF_HOME=$HF_HOME)"
        else
            echo "WARN: docling warm-up failed — first PDF may be slow/time out until cached"
        fi
    fi
    # Once the docling cache is warm, pin the runtime offline so cached-model loads
    # never pay HF Hub online revalidation (rate-limited without a token). Only when
    # the warm-up succeeded — otherwise leave online so the runtime can still fetch
    # lazily rather than failing every parse against an incomplete offline cache.
    if [ -f "$DOCLING_CACHE_DIR/.warmed" ]; then
        export HF_HUB_OFFLINE=1
        export TRANSFORMERS_OFFLINE=1
        echo "[provision] docling runtime pinned offline (cache warm)"
    fi
    # Point the F06 parser at the venv (docling/paddleocr live there). The baked
    # config defaults python_bin to system python3 so the txt/md plain-text path
    # always works (incl. lite); repoint it here only when the venv is present so
    # PDF/image parsing is enabled without breaking text ingestion when it is not.
    if [ -x "$CORTRIX_OCR_VENV/bin/python3" ] && [ -w /app/config/cortrix.yaml ]; then
        sed -i "s|^  python_bin:.*|  python_bin: $CORTRIX_OCR_VENV/bin/python3|" /app/config/cortrix.yaml
        echo "[provision] parser python_bin -> $CORTRIX_OCR_VENV/bin/python3"
    fi
fi

# DEFECT#8 (b): surface the OCR parser provisioning outcome to /health so a server
# that came up with PDF/image parsing unavailable does not silently report a bare
# "healthy". The server reads CORTRIX_PARSER_STATUS and adds a components.parser
# entry, degrading overall status only when "unavailable". States:
#   disabled    — PROFILE=lite, OCR intentionally off (not an error)
#   ok          — docling imports, OCR ready
#   unavailable — full profile but the parser stack failed to provision (the alarm)
# Left unset on dev/native runs → the server omits the component (prior behaviour).
if [ "$CORTRIX_PROFILE" = "lite" ]; then
    export CORTRIX_PARSER_STATUS="disabled"
elif "$CORTRIX_OCR_VENV/bin/python3" -c "import docling" >/dev/null 2>&1; then
    export CORTRIX_PARSER_STATUS="ok"
else
    export CORTRIX_PARSER_STATUS="unavailable"
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
