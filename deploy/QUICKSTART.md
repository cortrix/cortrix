# Cortrix — Local Quickstart (initial deployment)

From a fresh checkout to a running stack. For the model details and Docker
deployment see [`MODELS.md`](./MODELS.md).

**Prerequisites:** CMake + a C++17 toolchain, `python3.12`, Node.js (for the web
UI), and ~10 GB free disk (models + parser stack).

---

## 1. Build the server

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2 --target cortrix-server
```

> ONNX Runtime is fetched on first configure. If the download is flaky, point at
> a local copy: `-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME=<path-to>/onnxruntime-src`.

## 2. Provision the 5 local models — one-click

```bash
./scripts/setup_models.sh            # downloads ONNX models + builds parser venv
```

This populates `./models` and `./scripts/ocr_venv`:

| Model | Role | Auto |
|---|---|---|
| bge-m3 | embedding | ✅ public download |
| bge-reranker-v2-m3 | rerank | ⚠️ set `CORTRIX_RERANKER_MODEL_URL` (no public ONNX) |
| query-complexity | F39 routing | ⚠️ set `CORTRIX_QUERY_COMPLEXITY_MODEL_URL` (Cortrix-hosted) |
| docling + paddleocr | PDF/image parse | ✅ pip into the venv |

The two ⚠️ models have no public download — host them and set the env vars
(see [`MODELS.md`](./MODELS.md#hosting-reranker--query-complexity-operator-action)),
otherwise they degrade gracefully (reranker stub / heuristic complexity).

**Manual equivalent:**
```bash
bash deploy/download-models.sh ./models
python3.12 -m venv scripts/ocr_venv
scripts/ocr_venv/bin/pip install -r scripts/requirements-parser.txt
```

## 3. Configure

```bash
cp config.yaml.example build/config.yaml
```
Edit `build/config.yaml` to add LLM keys for the roles you want (enricher /
doc-summary / agent / semantic). The model paths already point at `./models`.
Retrieval (vectors + BM25 + rerank) works with **no** LLM configured.

## 4. Launch — one-click

```bash
./dev.sh
```
Starts backend (8420) + Cortrix Agent (8001) + web UI (5173).

> **Fail-fast:** `dev.sh` refuses to start if the local models are missing and
> tells you to run `./scripts/setup_models.sh`. To start anyway in degraded /
> stub mode: `CORTRIX_ALLOW_MISSING_MODELS=1 ./dev.sh`.
>
> First start takes ~20 s while the ONNX models compile (CoreML). That is normal
> — do not Ctrl+C before you see `Backend ready ✓`.

## 5. Verify

```bash
curl -s http://localhost:8420/api/v1/health          # 200
curl -s http://localhost:8420/api/v1/system/version  # {"version":"1.0.0"}
```

Open the web UI at <http://localhost:5173>.

---

## Manual bring-up (without dev.sh)

Run each service yourself (separate terminals):

```bash
# 1) Backend (from the repo root, so ./models resolves)
CORTRIX_OPENAPI_ROOT="$PWD" ./build/cortrix-server --config build/config.yaml

# 2) Cortrix Agent (optional — conversational RAG)
python3.12 -m venv cortrix-agent/venv
cortrix-agent/venv/bin/pip install -r cortrix-agent/requirements.txt
cp cortrix-agent/.env.example cortrix-agent/.env   # set LLM_PROVIDER + key + CORTRIX_BASE_URL=http://localhost:8420
(cd cortrix-agent && venv/bin/python -m uvicorn main:app --host 0.0.0.0 --port 8001)

# 3) Web UI (optional)
(cd web && npm install && npm run dev)
```

| Service | URL |
|---|---|
| Backend API | http://localhost:8420 |
| Cortrix Agent | http://localhost:8001 |
| Web UI | http://localhost:5173 |

---

## Docker (alternative)

A single container provisions everything into the data volume on first run; see
[`MODELS.md`](./MODELS.md#1-automatic-docker-default-profilefull). Text-only
lite mode: `CORTRIX_PROFILE=lite`.
