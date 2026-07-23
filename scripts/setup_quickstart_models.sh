#!/usr/bin/env bash
# Provision the pinned ONNX models used by the real-model first-value demo.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODEL_DIR="${CORTRIX_MODEL_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/cortrix/models}"
TOOLS_VENV="${CORTRIX_MODEL_TOOLS_VENV:-${XDG_CACHE_HOME:-$HOME/.cache}/cortrix/model-tools-venv}"
PYTHON_BIN="${CORTRIX_MODEL_PYTHON:-python3.12}"

EMBEDDING_REV="5617a9f61b028005a4858fdac845db406aefb181"
RERANKER_REV="953dc6f6f85a1b2dbfca4c34a2796e7dde08d41e"
EMBEDDING_DIR="$MODEL_DIR/bge-m3"
RERANKER_SOURCE_DIR="$MODEL_DIR/.sources/bge-reranker-v2-m3-$RERANKER_REV"
RERANKER_DIR="$MODEL_DIR/bge-reranker-v2-m3"
CONVERTER="$REPO_ROOT/scripts/convert_reranker_to_onnx.py"

fail() {
    printf 'setup_quickstart_models: %s\n' "$*" >&2
    exit 1
}

for command in curl shasum stat "$PYTHON_BIN"; do
    command -v "$command" >/dev/null 2>&1 || fail "required command not found: $command"
done

file_size() {
    if stat -f '%z' "$1" >/dev/null 2>&1; then
        stat -f '%z' "$1"
    else
        stat -c '%s' "$1"
    fi
}

verify_file() {
    local path="$1"
    local expected_size="$2"
    local expected_sha="$3"
    [[ -f "$path" ]] || return 1
    [[ "$(file_size "$path")" == "$expected_size" ]] || return 1
    [[ "$(shasum -a 256 "$path" | awk '{print $1}')" == "$expected_sha" ]]
}

download_file() {
    local url="$1"
    local destination="$2"
    local expected_size="$3"
    local expected_sha="$4"
    if verify_file "$destination" "$expected_size" "$expected_sha"; then
        printf 'verified %s\n' "$destination"
        return
    fi
    mkdir -p "$(dirname "$destination")"
    local partial="${destination}.part"
    rm -f "$partial"
    curl --fail --location --retry 3 --output "$partial" "$url"
    if ! verify_file "$partial" "$expected_size" "$expected_sha"; then
        rm -f "$partial"
        fail "downloaded file failed size or SHA-256 verification: $destination"
    fi
    mv "$partial" "$destination"
    printf 'downloaded and verified %s\n' "$destination"
}

mkdir -p "$EMBEDDING_DIR" "$RERANKER_SOURCE_DIR" "$RERANKER_DIR"

download_file \
    "https://huggingface.co/BAAI/bge-m3/resolve/$EMBEDDING_REV/onnx/model.onnx" \
    "$EMBEDDING_DIR/model.onnx" 724923 \
    f84251230831afb359ab26d9fd37d5936d4d9bb5d1d5410e66442f630f24435b
download_file \
    "https://huggingface.co/BAAI/bge-m3/resolve/$EMBEDDING_REV/onnx/model.onnx_data" \
    "$EMBEDDING_DIR/model.onnx_data" 2266820608 \
    1eebfb28493f67bba03ce0ef64bfdc7fc5a3bd9d7493f818bb1d78cd798416b4
download_file \
    "https://huggingface.co/BAAI/bge-m3/resolve/$EMBEDDING_REV/tokenizer.json" \
    "$EMBEDDING_DIR/tokenizer.json" 17098108 \
    21106b6d7dab2952c1d496fb21d5dc9db75c28ed361a05f5020bbba27810dd08
download_file \
    "https://huggingface.co/BAAI/bge-m3/resolve/$EMBEDDING_REV/config.json" \
    "$EMBEDDING_DIR/config.json" 687 \
    26159e7ad065073448460117eb24b7a4572f6f4e78eadff65dc0a11c052449fa

download_file \
    "https://huggingface.co/BAAI/bge-reranker-v2-m3/resolve/$RERANKER_REV/model.safetensors" \
    "$RERANKER_SOURCE_DIR/model.safetensors" 2271071852 \
    d9e3e081faff1eefb84019509b2f5558fd74c1a05a2c7db22f74174fcedb5286
download_file \
    "https://huggingface.co/BAAI/bge-reranker-v2-m3/resolve/$RERANKER_REV/tokenizer.json" \
    "$RERANKER_SOURCE_DIR/tokenizer.json" 17098273 \
    69564b696052886ed0ac63fa393e928384e0f8caada38c1f4864a9bfbf379c15
download_file \
    "https://huggingface.co/BAAI/bge-reranker-v2-m3/resolve/$RERANKER_REV/config.json" \
    "$RERANKER_SOURCE_DIR/config.json" 795 \
    13dcd6c31d9fec9d1d8e158702072f62d7fa7d312a64b9fe057bec9a08cfe41a

[[ "$(shasum -a 256 "$CONVERTER" | awk '{print $1}')" == \
    "01c6b0fdd56fc75af2bfd307731faeae9589c5df4fe7eff460f43332e69550b6" ]] \
    || fail "reranker conversion script identity does not match the model lock"

if ! verify_file "$RERANKER_DIR/model.onnx" 655732 \
        6b31a3947fe79e6422641fb54ee01036026871e67eeefbce1ec4f9f0f4f7f271 \
    || ! verify_file "$RERANKER_DIR/model.onnx_data" 2271023104 \
        2f17f314b7947fb5003d04127d5f2d421fe8f2af5fc330b5ccb36882e73165bd \
    || ! verify_file "$RERANKER_DIR/tokenizer.json" 17098273 \
        69564b696052886ed0ac63fa393e928384e0f8caada38c1f4864a9bfbf379c15; then
    "$PYTHON_BIN" -m venv "$TOOLS_VENV"
    "$TOOLS_VENV/bin/python" -m pip install --upgrade pip
    "$TOOLS_VENV/bin/python" -m pip install \
        'torch==2.7.1' 'transformers==4.53.2' 'onnx==1.18.0'
    rm -f "$RERANKER_DIR/model.onnx" "$RERANKER_DIR/model.onnx_data" \
        "$RERANKER_DIR/tokenizer.json"
    "$TOOLS_VENV/bin/python" "$CONVERTER" "$RERANKER_SOURCE_DIR" "$RERANKER_DIR"
fi

verify_file "$RERANKER_DIR/model.onnx" 655732 \
    6b31a3947fe79e6422641fb54ee01036026871e67eeefbce1ec4f9f0f4f7f271 \
    || fail "converted reranker graph does not match the model lock"
verify_file "$RERANKER_DIR/model.onnx_data" 2271023104 \
    2f17f314b7947fb5003d04127d5f2d421fe8f2af5fc330b5ccb36882e73165bd \
    || fail "converted reranker weights do not match the model lock"
verify_file "$RERANKER_DIR/tokenizer.json" 17098273 \
    69564b696052886ed0ac63fa393e928384e0f8caada38c1f4864a9bfbf379c15 \
    || fail "converted reranker tokenizer does not match the model lock"

printf 'QUICKSTART_MODELS=READY\n'
printf 'CORTRIX_MODEL_DIR=%s\n' "$MODEL_DIR"
printf 'embedding_revision=%s\n' "$EMBEDDING_REV"
printf 'reranker_revision=%s\n' "$RERANKER_REV"
