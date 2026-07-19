# First-value SupportOps demo

This synthetic demo starts an attested local Cortrix binary, loads three small Markdown documents, asks one source-backed question, checks versioned evidence assertions, verifies a trace, removes its unique namespace, and stops the server.

The default profile uses real local ONNX embedding and reranker models with `rerank=true`. All LLM roles remain disabled, so no provider key or network call is needed after model provisioning.

## Prerequisites

- A clean Cortrix source checkout.
- CMake and a C++17 toolchain supported by the main project build.
- Python 3.12, `curl`, and at least 15 GB of working disk space for pinned model sources, converted files, build output, and caches.
- An unused loopback API port and local metrics port `9091`.

The fixture is submitted as direct Markdown content through the JSON API. This path does not install or validate PDF, DOCX, image, OCR, or other external parsers.

## Provision pinned models

Keep models, conversion dependencies, caches, build output, and runtime evidence outside the source checkout:

```bash
export CORTRIX_WORK_ROOT="${CORTRIX_WORK_ROOT:-$HOME/.cache/cortrix/first-value}"
export CORTRIX_MODEL_DIR="$CORTRIX_WORK_ROOT/models"
export CORTRIX_MODEL_TOOLS_VENV="$CORTRIX_WORK_ROOT/model-tools-venv"
export XDG_CACHE_HOME="$CORTRIX_WORK_ROOT/cache/xdg"
export PIP_CACHE_DIR="$CORTRIX_WORK_ROOT/cache/pip"
export TMPDIR="$CORTRIX_WORK_ROOT/tmp"
mkdir -p "$XDG_CACHE_HOME" "$PIP_CACHE_DIR" "$TMPDIR"
./scripts/setup_quickstart_models.sh
```

The setup script downloads BAAI/bge-m3 at revision `5617a9f61b028005a4858fdac845db406aefb181` and BAAI/bge-reranker-v2-m3 at revision `953dc6f6f85a1b2dbfca4c34a2796e7dde08d41e`. It verifies every source weight, tokenizer, converted ONNX graph, and external-data file against [the model lock](model-lock.json). The embedding model is MIT-licensed; the reranker is Apache-2.0-licensed. The reranker is converted locally with the pinned tool versions recorded in the lock.

Provisioning fails closed on a missing tool, failed download, size mismatch, SHA-256 mismatch, converter identity mismatch, or non-matching converted artifact.

## Build and run the primary profile

```bash
export CMAKE_BUILD_DIR="$CORTRIX_WORK_ROOT/build"
export CORTRIX_DEMO_EVIDENCE_DIR="$CORTRIX_WORK_ROOT/evidence"
cmake -S . -B "$CMAKE_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCORTRIX_USE_ONNX=ON
cmake --build "$CMAKE_BUILD_DIR" --target cortrix-server -j
python3 examples/first-value-supportops/run_demo.py \
  --core-repo . \
  --build-dir "$CMAKE_BUILD_DIR" \
  --models-dir "$CORTRIX_MODEL_DIR" \
  --output-root "$CORTRIX_DEMO_EVIDENCE_DIR"
```

The runner requires a clean Core commit/tree and a Release build from that exact source with `CORTRIX_USE_ONNX=ON`. It creates an unauthenticated server only on `127.0.0.1`, attests `llm_enabled=false`, and requires readiness evidence for configured, active, non-fallback CPU embedding and reranker sessions. The query sends `rerank=true`; numeric `rerank_score` values and increasing embedding/reranker inference counters prove that the real model paths participated in the request.

It also fails closed on an occupied port, health or HTTP error, incomplete ingest task, empty results, missing source/snippet/provenance, missing trace identity, forbidden claim, cleanup failure, residual live namespace, or server shutdown failure.

## Expected observation

A passing run prints:

```text
FIRST_VALUE_DEMO_CONTRACT=PASS
expected_file_loaded=true
trace_assertion=PASS
remaining_matching_namespaces=0
```

Read `machine-summary.json` in the printed evidence directory for model identities, build and binary hashes, readiness components, inference counters, HTTP evidence, trace assertion, and cleanup state.

## Secondary ONNX-off contract

The runner retains an explicit secondary profile for low-cost API/evidence-contract testing:

```bash
cmake -S . -B "$CORTRIX_WORK_ROOT/build-onnx-off" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCORTRIX_USE_ONNX=OFF
cmake --build "$CORTRIX_WORK_ROOT/build-onnx-off" --target cortrix-server -j
python3 examples/first-value-supportops/run_demo.py \
  --profile onnx-off-contract \
  --core-repo . \
  --build-dir "$CORTRIX_WORK_ROOT/build-onnx-off" \
  --output-root "$CORTRIX_DEMO_EVIDENCE_DIR"
```

That secondary run uses stub embedding, skips the real reranker, and sends `rerank=false`. It is not the Quick Start quality path and must not be used to claim semantic or reranking behavior.

## Boundary

This is a synthetic, loopback-only first-value contract for direct Markdown content. It is not a customer story, production-readiness or security claim, parser-coverage claim, PostgreSQL compatibility claim, or retrieval-quality benchmark.
