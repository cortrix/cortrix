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
