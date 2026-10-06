---
title: Models and ONNX
description: Where the bundled models come from, how they're downloaded, and how their integrity is checked.
weight: 30
---

Cortrix runs two local models. Here's where they come from, how they're verified at startup, and where the files live.

## Models {#models}

A Docker deployment downloads two families of local neural models into the `cortrix-data` volume. They aren't baked into the image.

| Model | Role | Download size | Pinned source | Upstream and license |
|---|---|---:|---|---|
| BGE-M3 | Embedding | 585,562,194 bytes | [`onnx-community/bge-m3-ONNX@25b9af8`](https://huggingface.co/onnx-community/bge-m3-ONNX/tree/25b9af8e87a38eb120cfe87125383677b9cd309e) | [`BAAI/bge-m3@5617a9f`](https://huggingface.co/BAAI/bge-m3/tree/5617a9f61b028005a4858fdac845db406aefb181), MIT |
| bge-reranker-v2-m3 | Reranking | 587,809,994 bytes | [`onnx-community/bge-reranker-v2-m3-ONNX@6f5ff65`](https://huggingface.co/onnx-community/bge-reranker-v2-m3-ONNX/tree/6f5ff65298512715a1e669753bc754d2bc8f367b) | [`BAAI/bge-reranker-v2-m3@953dc6f`](https://huggingface.co/BAAI/bge-reranker-v2-m3/tree/953dc6f6f85a1b2dbfca4c34a2796e7dde08d41e), Apache-2.0 |

The `onnx-community` repositories are format-conversion sources for the BAAI upstream revisions listed above. Cortrix doesn't claim authorship of these models.

These models are independent of any LLM configuration. With no LLM role configured, embedding and reranking work as usual.

## Model manifest {#manifest}

`deploy/model-manifest.tsv` is the authoritative, machine-readable record of model identity. For every model and tokenizer file it records:

- The repository, revision, and file path
- The expected byte size and SHA-256
- The upstream repository, upstream revision, and upstream license

The manifest lists four files:

| Component | Installed at |
|---|---|
| `embedding_model` | `bge-m3/model.onnx` |
| `embedding_tokenizer` | `bge-m3/tokenizer.json` |
| `reranker_model` | `bge-reranker-v2-m3/model.onnx` |
| `reranker_tokenizer` | `bge-reranker-v2-m3/tokenizer.json` |

## Download and verification at startup {#provisioning}

On the first start, the container downloads the manifest's files into `/data/models`. The downloader:

1. Fetches the exact revision and path in the manifest.
2. Rejects symbolic links.
3. Checks the byte size and SHA-256.
4. Installs each file atomically.

If anything doesn't match, or a download fails, the container doesn't report ready. Readiness also requires both models to finish loading and the demo data to finish importing. The quickstart never falls back silently to a stub embedder or reranker.

Later starts reuse the models already in the volume.

## Remove the cached models {#remove}

```bash
docker compose -f deploy/docker-compose.yml down --volumes
```

> [!CAUTION]
> This deletes the whole volume, including your data, not just the models.

## Model settings when you run from source {#source-build}

When you run from source, the configuration file sets the model paths:

```yaml {title="config.yaml"}
embedding:
  model_path: "./models/bge-m3/model.onnx"
  tokenizer_path: "./models/bge-m3/tokenizer.json"
  dimension: 1024
  max_seq_length: 512

reranker:
  model_dir: "./models/bge-reranker-v2-m3"
```

- `dimension` is fixed at 1024 and can't be changed.
- An empty `embedding.model_path` is the only explicit stub mode. If the path is set but the model is missing or invalid, the server fails to start instead of degrading to a stub.
- An empty `reranker.model_dir` explicitly selects the stub reranker. If the path is set, the model and tokenizer must be valid.

## What the quickstart doesn't include {#not-included}

The Docker quickstart covers only text files, BGE-M3 embedding, and bge-reranker-v2-m3 reranking. It doesn't install or validate PDF, DOCX, images, OCR, query-complexity routing, external LLM roles, or the built-in Agent. Those are separate features. Check [Compatibility and status](/docs/resources/compatibility/) before you rely on them.

## Upgrade ONNX Runtime {#onnx-upgrade}

The repository pins the ONNX Runtime version. The steps and verification requirements for upgrading it are written for maintainers. See [ONNX Runtime upgrade and rollback](https://github.com/cortrix/cortrix/blob/release/1.0/docs/operations/onnx-upgrade.md).

## Next steps {#next-steps}

- [Performance tuning](/docs/deploy/performance-tuning/)
- [NVIDIA CUDA](/docs/deploy/cuda/)
- [Configuration](/docs/deploy/configuration/)
