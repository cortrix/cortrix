# Cortrix Quick Start model provenance

The Docker Quick Start downloads two pinned local neural model families into the
`cortrix-data` volume. They are not baked into the image. External LLM
configuration is independent: the Quick Start keeps every LLM role disabled.

| Model | Role | Downloaded assets | Pinned source | Upstream source and license recorded in the manifest |
|---|---|---:|---|---|
| BGE-M3 | embedding | 585,562,394 bytes | [`onnx-community/bge-m3-ONNX@25b9af8…`](https://huggingface.co/onnx-community/bge-m3-ONNX/tree/25b9af8e87a38eb120cfe87125383677b9cd309e) | [`BAAI/bge-m3@5617a9f…`](https://huggingface.co/BAAI/bge-m3/tree/5617a9f61b028005a4858fdac845db406aefb181), MIT |
| bge-reranker-v2-m3 | reranking | 587,809,994 bytes | [`onnx-community/bge-reranker-v2-m3-ONNX@6f5ff65…`](https://huggingface.co/onnx-community/bge-reranker-v2-m3-ONNX/tree/6f5ff65298512715a1e669753bc754d2bc8f367b) | [`BAAI/bge-reranker-v2-m3@953dc6f…`](https://huggingface.co/BAAI/bge-reranker-v2-m3/tree/953dc6f6f85a1b2dbfca4c34a2796e7dde08d41e), Apache-2.0 |

The canonical machine-readable identity is
[`model-manifest.tsv`](./model-manifest.tsv), which records each model and
tokenizer file's repository, revision, source path, expected byte size, SHA-256,
upstream repository, upstream revision, and upstream license.

---

## Docker Quick Start provisioning

The Quick Start provisions the manifest into `/data/models` on the first start:

```bash
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

No model URL overrides are required for this profile. The downloader resolves
the exact revision and source path in the manifest, rejects symbolic links,
checks the expected byte count and SHA-256, and atomically installs each file.
Any mismatch or failed download keeps the container unready.

The readiness gate also requires both CPU model sessions and the source-backed
demo fixture to finish bootstrap. The Quick Start does not silently fall back to
stub embedding or stub reranking.

To remove the cached models:

```bash
docker compose -f deploy/docker-compose.yml down --volumes
```

---

## Other model and parser surfaces

The Docker Quick Start intentionally covers only the source-backed text fixture,
BGE-M3 embedding, and bge-reranker-v2-m3 reranking. It does not install or
validate PDF, DOCX, image, OCR, query-complexity routing, external LLM roles, or
the built-in Agent.

Those are separate feature and operations surfaces. Review
[`docs/compatibility.md`](../docs/compatibility.md) before relying on them, and
use their focused documentation and tests instead of generalizing from the Quick
Start.

## Maintainer verification

The [First-value SupportOps demo](../examples/first-value-supportops/README.md)
is the deeper source-build path. It records model identity, execution providers,
inference counters, reranking scores, trace assertions, cleanup, and server-stop
evidence. It is not required for a user to run the Docker Quick Start.
