# Cortrix Quick Start

This guide builds Cortrix from source and runs a source-backed local query with real ONNX embedding and reranking. External LLM roles stay disabled, so the run needs no provider key.

Cortrix is in active pre-release development. Read [Compatibility and known status](compatibility.md) before treating any surface as production-ready.

## What this path verifies

The primary Quick Start:

- builds `cortrix-server` in Release mode with `CORTRIX_USE_ONNX=ON`;
- binds the unauthenticated API and anonymous metrics endpoint to loopback only;
- loads pinned BAAI/bge-m3 embedding and BAAI/bge-reranker-v2-m3 reranker models on CPU;
- keeps every external LLM role disabled;
- ingests three synthetic Markdown documents as direct JSON content;
- sends a plural `namespaces` query with `rerank=true`;
- checks source, snippet, provenance, trace, model readiness, and inference counters;
- deletes its unique namespace and stops its own server.

It does not install or validate PDF, DOCX, image, OCR, or other external parsers. It is a first-value contract, not a retrieval-quality benchmark or production-readiness test.

## Prerequisites

- Git, CMake, and a C++17 compiler
- OpenSSL development headers
- Python 3.12
- `curl`
- at least 15 GB of working disk space for model sources, converted files, dependencies, build output, and runtime evidence

macOS:

```bash
xcode-select --install
brew install cmake openssl python@3.12
```

Ubuntu or Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev git curl python3.12 python3.12-venv
```

## 1. Clone and choose a work directory

```bash
git clone https://github.com/cortrix/cortrix.git
cd cortrix
export CORTRIX_WORK_ROOT="${CORTRIX_WORK_ROOT:-$HOME/.cache/cortrix/first-value}"
export CORTRIX_MODEL_DIR="$CORTRIX_WORK_ROOT/models"
export CORTRIX_MODEL_TOOLS_VENV="$CORTRIX_WORK_ROOT/model-tools-venv"
export CMAKE_BUILD_DIR="$CORTRIX_WORK_ROOT/build"
export CORTRIX_DEMO_EVIDENCE_DIR="$CORTRIX_WORK_ROOT/evidence"
export XDG_CACHE_HOME="$CORTRIX_WORK_ROOT/cache/xdg"
export PIP_CACHE_DIR="$CORTRIX_WORK_ROOT/cache/pip"
export TMPDIR="$CORTRIX_WORK_ROOT/tmp"
mkdir -p "$XDG_CACHE_HOME" "$PIP_CACHE_DIR" "$TMPDIR" "$CORTRIX_DEMO_EVIDENCE_DIR"
```

The work directory keeps large generated data outside the Git checkout. Do not put provider keys in source-controlled files.

## 2. Provision pinned local models

```bash
./scripts/setup_quickstart_models.sh
```

The script downloads exact revisions, verifies source hashes, and converts the reranker locally with pinned Python packages. Identities, licenses, expected file sizes, and SHA-256 values are recorded in [the model lock](../examples/first-value-supportops/model-lock.json).

Expected final line:

```text
QUICKSTART_MODELS=READY
```

Provisioning is intentionally fail closed. A download, checksum, converter, or converted-output mismatch must be resolved rather than bypassed.

## 3. Build the real ONNX profile

```bash
cmake -S . -B "$CMAKE_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCORTRIX_USE_ONNX=ON
cmake --build "$CMAKE_BUILD_DIR" --target cortrix-server -j
```

The binary is created at `$CMAKE_BUILD_DIR/cortrix-server`.

## 4. Run the first-value demo

```bash
python3 examples/first-value-supportops/run_demo.py \
  --core-repo . \
  --build-dir "$CMAKE_BUILD_DIR" \
  --models-dir "$CORTRIX_MODEL_DIR" \
  --output-root "$CORTRIX_DEMO_EVIDENCE_DIR"
```

A passing run prints:

```text
FIRST_VALUE_DEMO_CONTRACT=PASS
expected_file_loaded=true
trace_assertion=PASS
remaining_matching_namespaces=0
```

The runner refuses a dirty source tree, a build from another source directory, a non-Release build, `CORTRIX_USE_ONNX=OFF`, model identity drift, an occupied port, a non-loopback host, model fallback, `rerank=false`, failed evidence assertions, residual namespace data, or an unclean server shutdown.

## 5. Inspect the evidence

Open `machine-summary.json` in the printed evidence directory. A primary pass includes:

- exact Core commit, tree, CMake cache hash, and server binary hash;
- model-lock hash and each model file identity;
- `llm_enabled=false`;
- `embedding_execution_provider` and `reranker_execution_provider` with `model_configured=true`, `active_ep=cpu`, `fallback=false`, and `policy_mismatch=false`;
- increasing embedding inference and reranker scoring counters;
- a query response with numeric `rerank_score` values;
- source, snippet, provenance, trace, cleanup, and server-stop assertions.

The runtime config uses:

```yaml
server:
  host: "127.0.0.1"
auth:
  enabled: false
```

Cortrix rejects `auth.enabled=false` with a non-loopback `server.host` before creating the API listener. The built-in Agent launcher and anonymous metrics endpoint also default to loopback. To bind the API to another interface, explicitly enable and configure the existing authentication controls first.

## Local neural models versus LLM roles

`LLM_ENABLED=false` does not mean embeddings or reranking are disabled. This Quick Start uses two local neural ONNX models but makes no request to an external LLM provider.

LLM roles such as `semantic_llm`, `vision_llm`, `agent_llm`, `doc_summary_llm`, and `enricher_llm` are separate optional surfaces. Configure them only when testing the features that consume those roles.

## Query contract

The current query request uses a plural array:

```json
{
  "query": "How should an agent recover from an expired setup token?",
  "namespaces": ["your_namespace"],
  "top_k": 5,
  "rerank": true,
  "include_sources": true
}
```

The deprecated singular `namespace` query field is rejected. Other endpoints may still use a singular namespace field where their OpenAPI schema requires it.

## Secondary ONNX-off check

An explicit `onnx-off-contract` profile remains available for low-cost API and fixture testing. It uses stub embedding and `rerank=false`, so it is not the Quick Start quality path and cannot establish semantic or reranker behavior. See the [demo README](../examples/first-value-supportops/README.md) for its commands and boundary.

## Next steps

- [Agent access](agent-access.md)
- [OpenAPI spec](../api/openapi.yaml)
- [Compatibility and known status](compatibility.md)
- [Linux NVIDIA CUDA operations](operations/cuda-execution-provider.md)
- [Full model and parser setup](../deploy/MODELS.md)
