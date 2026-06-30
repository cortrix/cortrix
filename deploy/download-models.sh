#!/bin/bash
# Model download script for Cortrix Docker build
# Called during Dockerfile build stage

set -e

MODELS_DIR="${1:-/models}"
mkdir -p "$MODELS_DIR/paddleocr"

echo "=== Downloading Cortrix ML Models ==="

# bge-m3 ONNX model graph (~700KB) + external weights (~1.1GB)
echo "[1/4] Downloading bge-m3 ONNX model..."
mkdir -p "$MODELS_DIR/bge-m3"
if [ ! -f "$MODELS_DIR/bge-m3/model.onnx" ]; then
    wget -q --show-progress -O "$MODELS_DIR/bge-m3/model.onnx" \
        "https://hf-mirror.com/BAAI/bge-m3/resolve/main/onnx/model.onnx" || {
        echo "WARN: Failed to download from hf-mirror, trying HuggingFace..."
        wget -q --show-progress -O "$MODELS_DIR/bge-m3/model.onnx" \
            "https://huggingface.co/BAAI/bge-m3/resolve/main/onnx/model.onnx" || {
            echo "ERROR: bge-m3 model download failed"
        }
    }
else
    echo "  Already exists, skipping"
fi
# External weights file (required, contains actual model parameters)
if [ ! -f "$MODELS_DIR/bge-m3/model.onnx_data" ]; then
    wget -q --show-progress -O "$MODELS_DIR/bge-m3/model.onnx_data" \
        "https://hf-mirror.com/BAAI/bge-m3/resolve/main/onnx/model.onnx_data" || {
        echo "WARN: Failed to download from hf-mirror, trying HuggingFace..."
        wget -q --show-progress -O "$MODELS_DIR/bge-m3/model.onnx_data" \
            "https://huggingface.co/BAAI/bge-m3/resolve/main/onnx/model.onnx_data" || {
            echo "ERROR: bge-m3 external weights download failed"
        }
    }
else
    echo "  Already exists, skipping"
fi

# bge-m3 tokenizer (~17MB)
echo "[2/4] Downloading bge-m3 tokenizer..."
if [ ! -f "$MODELS_DIR/bge-m3/tokenizer.json" ]; then
    wget -q --show-progress -O "$MODELS_DIR/bge-m3/tokenizer.json" \
        "https://hf-mirror.com/BAAI/bge-m3/resolve/main/tokenizer.json" || {
        echo "WARN: Failed to download from hf-mirror, trying HuggingFace..."
        wget -q --show-progress -O "$MODELS_DIR/bge-m3/tokenizer.json" \
            "https://huggingface.co/BAAI/bge-m3/resolve/main/tokenizer.json" || {
            echo "ERROR: bge-m3 tokenizer download failed"
        }
    }
else
    echo "  Already exists, skipping"
fi

# bge-m3 config
echo "[3/4] Downloading bge-m3 config..."
wget -q -O "$MODELS_DIR/bge-m3-config.json" \
    "https://hf-mirror.com/BAAI/bge-m3/resolve/main/config.json" 2>/dev/null || \
    echo '{"model_type": "bge-m3", "status": "placeholder"}' > "$MODELS_DIR/bge-m3-config.json"

# bge-reranker-v2-m3 ONNX (F02 reranker). No canonical public ONNX export —
# it is converted from BAAI/bge-reranker-v2-m3 safetensors via
# scripts/convert_reranker_to_onnx.py. Provide a tarball URL of the converted
# dir (model.onnx + model.onnx_data + tokenizer.json) via env to auto-fetch;
# when unset the server falls back to a deterministic reranker stub.
echo "[*] Reranker (bge-reranker-v2-m3)..."
if [ -f "$MODELS_DIR/bge-reranker-v2-m3/model.onnx" ]; then
    echo "  Already exists, skipping"
elif [ -n "${CORTRIX_RERANKER_MODEL_URL:-}" ]; then
    mkdir -p "$MODELS_DIR/bge-reranker-v2-m3"
    wget -q --show-progress -O /tmp/reranker.tar.gz "$CORTRIX_RERANKER_MODEL_URL" && \
        tar -xf /tmp/reranker.tar.gz -C "$MODELS_DIR/bge-reranker-v2-m3" --strip-components=1 && \
        rm -f /tmp/reranker.tar.gz || \
        echo "WARN: reranker download/extract failed — reranker stub fallback in effect"
else
    echo "  CORTRIX_RERANKER_MODEL_URL not set — skipping (reranker stub fallback)"
fi

# query-complexity (F39/F37 DistilBERT classifier). Cortrix-trained artifact,
# no public source — must be hosted by the operator. Provide a tarball URL via
# env to auto-fetch; when unset F39 falls back to the heuristic backend.
echo "[*] Query-complexity classifier..."
if [ -f "$MODELS_DIR/query-complexity/model.onnx" ]; then
    echo "  Already exists, skipping"
elif [ -n "${CORTRIX_QUERY_COMPLEXITY_MODEL_URL:-}" ]; then
    mkdir -p "$MODELS_DIR/query-complexity"
    wget -q --show-progress -O /tmp/qc.tar.gz "$CORTRIX_QUERY_COMPLEXITY_MODEL_URL" && \
        tar -xf /tmp/qc.tar.gz -C "$MODELS_DIR/query-complexity" --strip-components=1 && \
        rm -f /tmp/qc.tar.gz || \
        echo "WARN: query-complexity download/extract failed — heuristic fallback in effect"
else
    echo "  CORTRIX_QUERY_COMPLEXITY_MODEL_URL not set — skipping (heuristic fallback)"
fi

# PaddleOCR models (~150MB)
echo "[4/4] Downloading PaddleOCR models..."
cd "$MODELS_DIR/paddleocr"
if [ ! -d "ch_PP-OCRv3_det_infer" ]; then
    wget -q https://paddleocr.bj.bcebos.com/PP-OCRv3/chinese/ch_PP-OCRv3_det_infer.tar && \
        tar -xf ch_PP-OCRv3_det_infer.tar && rm -f ch_PP-OCRv3_det_infer.tar || \
        echo "WARN: PaddleOCR det model download failed"
fi
if [ ! -d "ch_PP-OCRv3_rec_infer" ]; then
    wget -q https://paddleocr.bj.bcebos.com/PP-OCRv3/chinese/ch_PP-OCRv3_rec_infer.tar && \
        tar -xf ch_PP-OCRv3_rec_infer.tar && rm -f ch_PP-OCRv3_rec_infer.tar || \
        echo "WARN: PaddleOCR rec model download failed"
fi

echo ""
echo "=== Model Download Complete ==="
echo "Models directory: $MODELS_DIR"
ls -lh "$MODELS_DIR"
