# Cortrix Models — provisioning guide

Cortrix uses **5 local models**. None are baked into the Docker image (they are
large and licensing/provenance varies); they live in the data volume / `models/`
dir and are provisioned on demand. LLM configuration is **independent** of these
— the models below run on-device regardless of whether any LLM role is set.

| Model | Role | Size | Source | Absent → fallback |
|---|---|---|---|---|
| bge-m3 | embedding (dense+sparse) | ~2.1 GB | public (HF) | stub vectors + BM25 still work |
| bge-reranker-v2-m3 | F02 rerank | ~2.1 GB | **optional** — generate yourself¹ | deterministic stub |
| query-complexity | F39/F37 routing | ~256 MB | **optional** — generate yourself² | heuristic backend |
| docling (DocLayNet + TableFormer) | PDF/DOCX/PPTX parse | libs ~2 GB + models ~1 GB | pip + auto-download | PDF/Office ingestion off |
| paddleocr | scanned/image OCR | (shares parser venv) | pip + auto-download | image OCR off |

¹ ² **Cortrix does not yet host these two models (hosting + maintenance is
planned — see Roadmap below).** They are optional — when absent the system runs
fine on the listed fallbacks (rerank stub / heuristic routing). For now, generate
them yourself (see
[Generating reranker & query-complexity yourself](#generating-reranker--query-complexity-yourself-optional))
and either place the files under `models/` (local) or host a tarball and set
`CORTRIX_RERANKER_MODEL_URL` / `CORTRIX_QUERY_COMPLEXITY_MODEL_URL` (Docker).

---

## Three ways to provision

### 1. Automatic (Docker, default `PROFILE=full`)
`entrypoint.sh` provisions everything into the `/data` volume on first boot
(only the first start pays the cost; restarts are instant since it persists on
the volume):

```bash
docker compose up -d        # first start downloads models + builds parser venv
```

- **Lite (text-only, no heavy models):** `CORTRIX_PROFILE=lite` — txt/md + BM25
  work, embedding runs in stub mode, PDF/image parsing is off.
- reranker / query-complexity auto-fetch **only** when their `*_MODEL_URL` envs
  are set (see table notes); otherwise they degrade gracefully.

### 2. One-click (local / bare-metal)
```bash
./scripts/setup_models.sh                 # models + parser venv
./scripts/setup_models.sh --models-only   # ONNX only
./scripts/setup_models.sh --parser-only   # parser venv only
```
Downloads the ONNX models into `./models` and builds the parser venv at
`./scripts/ocr_venv`, then `./dev.sh`.

### 3. Manual
```bash
# ONNX models
bash deploy/download-models.sh ./models
# parser stack
python3.12 -m venv scripts/ocr_venv
scripts/ocr_venv/bin/pip install -r scripts/requirements-parser.txt
```

---

## Generating reranker & query-complexity yourself (optional)

> **These two models are optional. Cortrix does not yet host them (hosting +
> maintenance is on the roadmap — see the end of this doc).** Neither has a ready
> public ONNX download (reranker is only published as PyTorch weights;
> query-complexity is trained in-house). When absent the system works on the
> fallbacks above. Generate them yourself only if you want real rerank / learned
> routing.

### reranker (bge-reranker-v2-m3 → ONNX)

The conversion script ships with the repo (`scripts/convert_reranker_to_onnx.py`).
Needs `torch`, `transformers`, `onnx`.

```bash
# 1. download the public PyTorch weights (config.json + model.safetensors + tokenizer.json)
#    from https://huggingface.co/BAAI/bge-reranker-v2-m3  into  ./hf-reranker/
# 2. convert to ONNX
python3 scripts/convert_reranker_to_onnx.py ./hf-reranker ./models/bge-reranker-v2-m3
```
Produces `model.onnx` (+ `model.onnx_data` external weights) and copies the
tokenizer — the layout `OnnxReranker` expects.

### query-complexity (DistilBERT 3-class router → ONNX)

A small fine-tune (~6 min on CPU). Reproduction recipe (reference pipeline:
`build_dataset.py` → `train_classifier.py` → `export_onnx.py`):

- **Base:** `distilbert-base-uncased` (smallest genuinely-pretrained DistilBERT).
- **Labels (Adaptive-RAG, arXiv:2403.14403), question text only:**
  `chat` = SQuAD2.0 unanswerable · `simple` = SQuAD2.0 answerable + NQ-open ·
  `complex` = HotpotQA + MuSiQue. Balanced/deduped, ~16k train / ~1.8k val.
- **Train:** 2 epochs, batch 16, max_len 64, lr 3e-5, AdamW.
- **Export:** ONNX opset 14, softmax in-graph; ship `model.onnx` + tokenizer
  files into `models/query-complexity/`.

**This model is meant to be improved over time.** The reference checkpoint has a
known weak spot on short/greeting queries (domain shift — its labels come from QA
benchmarks). Closing that gap is just more in-domain data: collect real production
queries, label them by the routing they should take, add them to the training set,
re-run the 6-minute fine-tune, and re-export. Each round improves routing accuracy
on your actual query mix.

### Docker distribution (if you host them)

To fold either into the Docker first-run auto-provision, host a `.tar.gz` of the
model dir and set the URL env vars:

```bash
export CORTRIX_RERANKER_MODEL_URL=https://<host>/bge-reranker-v2-m3.tar.gz
export CORTRIX_QUERY_COMPLEXITY_MODEL_URL=https://<host>/query-complexity.tar.gz
```
The tarball is extracted with `--strip-components=1`, so it should contain the
files at the top level (e.g. `model.onnx`, `model.onnx_data`, `tokenizer.json`).
Hugging Face Hub or object storage (S3 / R2) are better fits than GitHub for the
~2 GB reranker (GitHub release assets cap at 2 GiB; repo files at 100 MiB).

---

## Roadmap / maintenance (TODO)

These two models are **temporarily user-generated**; the team plans to own them:

- **Host** the converted reranker + the trained query-complexity (Hugging Face
  Hub or object storage) so they fold into the one-click / Docker auto-provision.
- **Ship** the query-complexity training pipeline into the repo (currently the
  reference scripts live outside version control) so the recipe above is runnable
  as-is, not just reproducible from description.
- **Maintain & improve** query-complexity over time — fold real in-domain query
  traffic into the training set and re-export to lift routing accuracy on the
  production query mix (closing the known short/greeting-query gap).


