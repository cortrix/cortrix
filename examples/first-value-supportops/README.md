# First-value SupportOps demo

This synthetic demo starts an attested local Cortrix binary, loads three small SupportOps documents, asks one source-backed question, checks two required evidence groups in the versioned expected file, verifies a trace, removes its unique namespace, and stops the server.

## Prerequisites

- A clean Cortrix source checkout and a CMake build directory configured from that checkout with `CORTRIX_USE_ONNX=OFF`.
- Python 3.9 or newer.
- No running backend is accepted: the runner requires an unused loopback port and starts the exact `cortrix-server` binary itself.
- No LLM credential is required. The runner owns the config, requires `llm_enabled=false`, and sends `rerank=false`; this makes the demo a deterministic contract check, not a representative retrieval-quality benchmark.

## Run

From the repository root:

```bash
python3 examples/first-value-supportops/run_demo.py \
  --core-repo . \
  --build-dir build-r4
```

The command verifies the clean Core commit/tree, Release CMake source directory, `CORTRIX_USE_ONNX=OFF`, CMake cache hash, server binary hash, runner-owned PID/config and Core working directory, and `llm_enabled=false`. It fails closed on identity/config mismatch, an occupied port, health or HTTP errors, incomplete ingest tasks, empty results, either missing evidence group, missing same-result source/snippet/provenance, missing trace identity, forbidden claims, timeout, cleanup failure, a namespace that remains active or retains documents/blocks, or server shutdown failure. A Core tombstone with `status=deleted`, `doc_count=0`, and `block_count=0` is recorded as `tombstoned-empty` rather than treated as a live residual namespace.

Machine-readable evidence is written under the system temporary directory by default. Use `--output-root` to select another local evidence directory.

## Expected observation

A passing run prints:

```text
FIRST_VALUE_DEMO_CONTRACT=PASS
expected_file_loaded=true
trace_assertion=PASS
remaining_matching_namespaces=0
```

If the runner prints an evidence directory, read its `machine-summary.json`. A bootstrap failure such as a dirty checkout or invalid CMake build occurs before a machine summary can be created and is printed directly. Fix the identity, runtime, or request contract and rerun from a clean namespace; do not edit the expected file merely to make the demo pass.

## Boundary

This is a synthetic local first-value contract. It is not a customer story, production-readiness claim, security claim, PostgreSQL compatibility claim, or performance benchmark.
