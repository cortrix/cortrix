# Cortrix User Guide

> **Version**: v0.1.0 | **Updated**: 2026-02

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Environment Setup](#environment-setup)
3. [Building](#building)
4. [Configuration](#configuration)
5. [One-Command Startup (recommended)](#one-command-startup-recommended)
6. [Manual Startup](#manual-startup)
7. [Verification and Testing](#verification-and-testing)
8. [Using the Features](#using-the-features)
9. [LLM Configuration in Detail](#llm-configuration-in-detail)
10. [Vector Model Configuration](#vector-model-configuration)
11. [Authentication Configuration](#authentication-configuration)
12. [Automatic Directory Watching](#automatic-directory-watching)
13. [Production Deployment](#production-deployment)
14. [FAQ](#faq)

---

## Architecture Overview

Cortrix consists of three services, managed uniformly via `dev.sh`:

```
┌─────────────────────────────────────────────────────┐
│                  Browser :5173                        │
│              React + Vite frontend                    │
└──────────────────┬──────────────────────────────────┘
                   │ /api/*       │ /agent/*
                   ▼              ▼
    ┌──────────────────┐  ┌──────────────────────┐
    │  C++ backend :8420│  │  Python Agent :8001   │
    │  cortrix-server    │  │  cortrix-agent (FastAPI)  │
    │                   │  │                        │
    │  • Doc ingestion   │  │  • Frontend chat/RAG   │
    │  • Vector search   │  │  • LLM settings mgmt   │
    │  • Memory mgmt     │  │  • Session history     │
    │  • Intent classify │  │                        │
    │  config: build/   │  │  config: cortrix-agent/.env │
    │        config.yaml│  │                        │
    └──────────────────┘  └──────────────────────────┘
```

**LLM configuration (five role-based sections):**

The C++ backend reads its LLM settings directly from `build/config.yaml`. Each of
five roles is its own flat section with `provider` / `api_key` / `model` /
`base_url` (see [LLM Configuration in Detail](#llm-configuration-in-detail)):

| Role | Consumer | Purpose |
|------|------|------|
| `semantic_llm` | C++ backend | Intent classification + reranking (every query; fast/cheap) |
| `vision_llm` | C++ backend | OCR image refinement (vision model) |
| `agent_llm` | Python Agent | Conversational RAG/chat |
| `doc_summary_llm` | C++ backend | Ingest-side document summary (F41) |
| `enricher_llm` | C++ backend | SPC ingest enricher — NER + summary (F03) |

> The Python Agent (`cortrix-agent/`) resolves its provider from `cortrix-agent/.env`,
> falling back to the `agent_llm` section of `config.yaml`. See
> [LLM Configuration in Detail](#llm-configuration-in-detail).

**Performance metrics (MVP, measured)**: API P50 ~127μs, vector search 1K ~304μs, BM25 full-text search 1K ~72μs

---

## Environment Setup

### macOS

```bash
xcode-select --install          # Clang + build tools
brew install cmake openssl node  # CMake, OpenSSL, Node.js
```

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev git curl
# Node.js (required by the frontend)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
```

### Python Environment (document parsing)

```bash
# OCR virtual environment (for PDF/DOCX/image parsing)
python3 -m venv scripts/ocr_venv
source scripts/ocr_venv/bin/activate
pip install pymupdf python-docx pillow pytesseract

# Cortrix Agent virtual environment (for the frontend chat service)
python3 -m venv cortrix-agent/venv
source cortrix-agent/venv/bin/activate
pip install -r cortrix-agent/requirements.txt
```

---

## Building

```bash
git clone https://github.com/cortrix/cortrix.git
cd cortrix-codes/cortrix

# Standard build (includes ONNX vector model support)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

The build takes about 2-3 minutes; dependencies are downloaded automatically. Artifact: `build/cortrix-server`

**Lightweight build without ONNX** (no semantic search, BM25 full-text search only):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORTRIX_USE_ONNX=OFF
```

---

## Initial Configuration (required on first run)

The repository ships a configuration template; **copy and fill it in before the first run**:

```bash
cp config.yaml.example build/config.yaml
```

**User workflow:**

1. `cp config.yaml.example build/config.yaml`
2. In `build/config.yaml`, find the LLM role sections you need, fill in each
   role's `provider` / `api_key` / `model` / `base_url`, and uncomment it
3. Run `./dev.sh`

> **Note**: `build/config.yaml` is not committed to git (`build/` is already in `.gitignore`),
> so API Keys are kept safe locally. `config.yaml.example` contains no real keys and can be committed safely.

**Minimal example** (using Zhipu GLM):

```yaml
# Edit build/config.yaml and uncomment the role sections you need, filling in the
# Key. Each role is a flat section with the same four fields:
semantic_llm:
  provider: "glm"
  api_key: "your-real-key-here"      # ← fill in here
  model: "glm-4-flash"
  base_url: "https://open.bigmodel.cn/api/paas/v4"

agent_llm:
  provider: "glm"
  api_key: "your-real-key-here"
  model: "glm-4.6"
  base_url: "https://open.bigmodel.cn/api/paas/v4"
```

> The five roles are `semantic_llm`, `vision_llm`, `agent_llm`, `doc_summary_llm`,
> and `enricher_llm`. Configure only the ones you need; an unconfigured role simply
> stays off. See [LLM Configuration in Detail](#llm-configuration-in-detail) for
> the full list and per-provider notes (OpenAI, Anthropic Claude, …).

---

## Configuration

### Main config file: `build/config.yaml`

Edit this file to configure the C++ backend's behavior. For the fully-commented version, see `build/config.yaml` (it has detailed inline documentation).

**The config keys you'll most often need to change:**

```yaml
# Data storage directory (relative to codes/cortrix/)
namespace:
  data_dir: "./build/data"

# Vector model path (must be downloaded, see the "Vector Model Configuration" section)
embedding:
  model_path: "./models/bge-m3/model.onnx"

# LLM roles (each a flat section; uncomment and fill in api_key to activate)
semantic_llm:
  provider: "glm"
  api_key: "your-glm-api-key"   # ← fill in the real Key
  model: "glm-4-flash"
  base_url: "https://open.bigmodel.cn/api/paas/v4"
vision_llm:
  provider: "glm"
  api_key: "your-glm-api-key"
  model: "glm-4v-flash"
  base_url: "https://open.bigmodel.cn/api/paas/v4"
agent_llm:
  provider: "glm"
  api_key: "your-glm-api-key"
  model: "glm-4.6"
  base_url: "https://open.bigmodel.cn/api/paas/v4"

# Local directory auto-watching (file changes are ingested automatically)
watch_dir:
  data_dir: "/path/to/your/documents"
  namespace_name: "local"
  watch_enabled: true
```

---

## One-Command Startup (recommended)

```bash
cd codes/cortrix

# Start all three services (backend + Agent + frontend)
./dev.sh
```

Output after startup:
```
[cortrix] Using config file: .../build/config.yaml
[cortrix] Backend ready ✓
[cortrix] Cortrix Agent ready ✓  (LLM: glm)
[cortrix] =====================================
[cortrix]   Frontend: http://localhost:5173
[cortrix]   Backend:  http://localhost:8420/api/v1/health
[cortrix]   Agent:    http://localhost:8001/health
[cortrix]   Config:   build/config.yaml
[cortrix]   LLM:      cortrix-agent/.env
[cortrix]   Press Ctrl+C to stop all services
[cortrix] =====================================
```

Open **http://localhost:5173** in a browser.

### Startup Parameters

```bash
./dev.sh                          # default config
./dev.sh --config /path/x.yaml   # specify a config file
./dev.sh --auth                   # force authentication on
./dev.sh --no-auth                # force authentication off
```

---

## Manual Startup

To start each service individually:

```bash
# 1. Start the C++ backend
./build/cortrix-server --config build/config.yaml

# 2. Start the Cortrix Agent (in a new terminal)
cd cortrix-agent
venv/bin/python -m uvicorn main:app --host 0.0.0.0 --port 8001

# 3. Start the frontend (in a new terminal)
npm run dev --prefix web
```

---

## Verification and Testing

```bash
# Check the backend health status
curl http://localhost:8420/api/v1/health

# Expected response (llm_enabled=true means the LLM is loaded):
# {
#   "status": "healthy",
#   "version": "0.1.0",
#   "llm_enabled": true,
#   "llm_model": "glm-4-flash",
#   "llm_provider": "openai"
# }

# Check the Agent status
curl http://localhost:8001/health
# {"status":"ok"}

# Check the Agent's current LLM configuration
curl http://localhost:8001/config/llm/providers | python3 -m json.tool
```

---

## Using the Features

### Create a Namespace

```bash
curl -X POST http://localhost:8420/api/v1/namespaces \
  -H "Content-Type: application/json" \
  -d '{"name": "my-docs"}'
```

### Upload Documents

```bash
# Upload a single file
curl -X POST http://localhost:8420/api/v1/namespaces/my-docs/documents \
  -F "file=@/path/to/document.pdf"

# Upload with attached metadata
curl -X POST http://localhost:8420/api/v1/namespaces/my-docs/documents \
  -F "file=@report.pdf" \
  -F 'metadata={"author":"Jane Doe","year":2025}'
```

### Query Document Processing Status

```bash
curl http://localhost:8420/api/v1/namespaces/my-docs/documents/1/status
# status: pending → processing → ready / error
```

### Semantic Search

```bash
curl -X POST http://localhost:8420/api/v1/query \
  -H "Content-Type: application/json" \
  -d '{
    "query": "What is the refund policy?",
    "namespace": "my-docs",
    "top_k": 5
  }'
```

### Using the Web UI

Open http://localhost:5173, where you can:
- **Document management**: upload, view processing status, delete documents
- **Semantic search**: real-time hybrid search (vector + BM25)
- **Chat**: RAG conversation based on the document knowledge base
- **LLM settings**: gear icon in the top-right → switch LLM provider and model

---

## LLM Configuration in Detail

### Role-based configuration

LLM settings live in `build/config.yaml` as **five flat role sections**, each
with the same four fields. A role with `provider` + `api_key` + `model` all set
counts as configured; anything else leaves that role's feature off.

```yaml
<role>:
  provider: "openai" | "glm" | "claude" | "ollama" | "deepseek" | "mock"
  api_key:  "..."          # vendor API key
  model:    "..."          # vendor model id
  base_url: "..."          # API endpoint (see the wire-protocol note below)
```

Changes take effect on the next service restart — there is no separate runtime
provider registry and no database-backed role selection.

### The five LLM roles

| Role | Consumer | Purpose | Model suggestion |
|------|------|------|---------|
| `semantic_llm` | C++ backend | Intent classification + reranking (every query) | fast/cheap, e.g. glm-4-flash |
| `vision_llm` | C++ backend | OCR image enhancement (optional) | a vision model, e.g. glm-4v-flash |
| `agent_llm` | Python Agent | Frontend RAG conversation | high quality, e.g. glm-4.6 / gpt-4o |
| `doc_summary_llm` | C++ backend | Ingest-side document summary (F41) | fast/cheap |
| `enricher_llm` | C++ backend | SPC ingest enricher — NER + summary (F03) | fast/cheap |

> `vision_llm` inherits any unset `provider` / `api_key` / `base_url` from
> `semantic_llm` (typically only the `model` differs).

### Two consumers, two wire protocols

This determines which providers you can use for each role:

- **`agent_llm`** is consumed by the **Python Agent** (`cortrix-agent/`), which
  has a per-provider adapter and speaks each vendor's **native** protocol. So
  `provider: "claude"` here talks to the Anthropic Messages API directly — no
  proxy needed. (For Claude, do **not** set `base_url`: the adapter pins the
  Anthropic endpoint and ignores it.)
- **The other four roles** are consumed by the **C++ backend**, which speaks
  **only the OpenAI-compatible wire** (`POST {base_url}/chat/completions` with
  `Authorization: Bearer`). Any OpenAI-compatible service works directly (OpenAI,
  GLM, DeepSeek, a local vLLM/Ollama gateway, …). A provider whose native API is
  **not** OpenAI-compatible (e.g. Anthropic Claude) must be reached through an
  OpenAI-compatible **proxy gateway**, with `base_url` pointing at that proxy.

### Provider reference

| Provider | `provider` value | API Key source | Notes |
|------|------------|-------------|------|
| Zhipu GLM | `glm` | https://open.bigmodel.cn | OpenAI-compatible; works in any role |
| OpenAI | `openai` | https://platform.openai.com | OpenAI-compatible; works in any role |
| DeepSeek | `deepseek` | https://platform.deepseek.com | OpenAI-compatible; works in any role |
| Ollama (local) | `ollama` | no key needed, runs locally | OpenAI-compatible gateway |
| Anthropic Claude | `claude` | https://console.anthropic.com | Native in `agent_llm`; needs an OpenAI-compatible proxy for the other four roles |

Example — OpenAI (works in any role, OpenAI-compatible):

```yaml
semantic_llm:
  provider: "openai"
  api_key: "your-api-key"
  model: "gpt-4o-mini"
  base_url: "https://api.openai.com/v1"
agent_llm:
  provider: "openai"
  api_key: "your-api-key"
  model: "gpt-4o"
  base_url: "https://api.openai.com/v1"
```

Example — Anthropic Claude in `agent_llm` (native; no `base_url`):

```yaml
agent_llm:
  provider: "claude"
  api_key: "your-api-key"
  model: "claude-haiku-4-5-20251001"   # fast/low-cost; "claude-sonnet-4-6" for stronger results
```

> The Python Agent can also be configured (and overridden) via `cortrix-agent/.env`
> — see `cortrix-agent/.env.example`. Resolution order, highest first: real
> environment variables > `cortrix-agent/.env` > `config.yaml` `agent_llm` >
> built-in defaults.

### How Configuration Changes Take Effect

| Change type | How it takes effect |
|---------|---------|
| Editing any role section in `config.yaml` | Takes effect after a service restart |
| Editing `cortrix-agent/.env` (Python Agent) | Takes effect after restarting the Agent service |

---

## Vector Model Configuration

Cortrix uses **bge-m3** (1024-dim) for semantic vectorization via ONNX Runtime 1.17.1, requiring about 2.8GB of disk.

```bash
# Download the model files (about 2.8GB) to codes/cortrix/models/bge-m3/
# Required files:
#   model.onnx        (~708KB, model graph structure)
#   model.onnx_data   (~2.1GB, model weights)
#   tokenizer.json    (~16MB, tokenizer vocabulary)
mkdir -p models/bge-m3
# Download from Hugging Face: BAAI/bge-m3
```

After downloading, verify the configuration:
```yaml
embedding:
  model_path: "./models/bge-m3/model.onnx"
  dimension: 1024
  gpu_provider: "auto"   # macOS automatically uses CoreML acceleration
```

**When no model is configured** (Stub mode): the system uses random vectors; BM25 full-text search works normally, but semantic search quality is very poor.

---

## Authentication Configuration

Enabling authentication is recommended in production.

### Generate an API Key

```bash
# Pick a secret string
API_KEY="my-cortrix-secret-2026"

# Generate the SHA-256 hash
echo -n "$API_KEY" | shasum -a 256 | cut -d' ' -f1
# Output: 7c222fb2927d828af22f592134e8932480637c0d2... (example)
```

### Config File

```yaml
auth:
  enabled: true
  api_keys:
    - key_hash: "7c222fb2927d828af22f592134e8932480637c0d2..."
      tenant_id: "default"
      allowed_namespaces: []    # empty = allow all namespaces
      permissions: 7            # 7 = READ(1) + WRITE(2) + ADMIN(4)
      expires_at: 0             # 0 = never expires
```

### Sending the Key with a Request

```bash
curl -X POST http://localhost:8420/api/v1/query \
  -H "Authorization: Bearer my-cortrix-secret-2026" \
  -H "Content-Type: application/json" \
  -d '{"query": "test", "namespace": "demo"}'
```

---

## Automatic Directory Watching

Once configured, files added to or modified in the specified directory are automatically parsed and ingested:

```yaml
watch_dir:
  data_dir: "/Users/yourname/Documents/my-notes"   # watched directory
  namespace_name: "notes"    # which namespace to ingest into
  watch_enabled: true
```

**Supported formats**: PDF, DOCX, TXT, MD, CSV, JSON, JPG/PNG (OCR)

> If the directory does not exist, only a warning is printed; other functionality is unaffected.

---

## Production Deployment

### Single-Host Docker Deployment

```bash
docker run -d \
  --name cortrix \
  -p 8420:8420 \
  -v $(pwd)/data:/data \
  -v $(pwd)/config.yaml:/app/config.yaml \
  -v $(pwd)/models:/app/models \
  cortrix/cortrix:latest

# Check
curl http://localhost:8420/api/v1/health
```

### Recommended Production Configuration

```yaml
server:
  thread_count: 8       # tune to the number of CPU cores

auth:
  enabled: true         # must be on

log:
  level: "warning"      # reduce log volume in production
  format: "json"        # structured logs for easy collection

namespace:
  data_dir: "/data"     # mount a persistent volume

spc:
  worker_count: 4       # raise document-processing concurrency
```

### Reverse Proxy (Nginx)

```nginx
server {
    listen 443 ssl;
    server_name cortrix.yourdomain.com;

    location /api/ {
        proxy_pass http://127.0.0.1:8420;
        proxy_set_header Host $host;
    }

    location /agent/ {
        proxy_pass http://127.0.0.1:8001/;
        proxy_set_header Host $host;
    }
}
```

---

## FAQ

**Q: The backend shows `llm_enabled: false` after startup?**
A: Check that the `semantic_llm` section in `build/config.yaml` is uncommented and has `provider` + `api_key` + `model` all filled in (a role counts as configured only when all three are set).

**Q: The frontend LLM settings dialog reports a JSON error?**
A: The Cortrix Agent service (port 8001) is not running. Start everything with `./dev.sh`, or start it manually:
```bash
cd cortrix-agent && venv/bin/python -m uvicorn main:app --port 8001
```

**Q: A document stays in `pending` status after upload?**
A: The SPC worker may have hit a parsing error. Check the backend logs; common causes: Python dependencies not installed, OCR environment issues.

**Q: Semantic search results are poor quality?**
A: It may be running in Stub mode (random vectors). Check:
```bash
curl http://localhost:8420/api/v1/health
# In the startup logs you should see: OnnxEmbedder initialized (real_model=true)
```

**Q: The ONNX model load shows a CoreML Warning?**
A: This is normal. CoreML does not support the embedding weight layer (dimension over 16384), so it automatically falls back to CPU; results are unaffected.

**Q: macOS build reports OpenSSL not found?**
```bash
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
cmake -B build -DOPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR ..
```

**Q: A port is already in use?**
```bash
# Find the process using the port
lsof -i :8420
# Or change the port in config.yaml
server:
  port: 8420
```

---

## Documentation Index

| Document | Content |
|------|------|
| [QUICKSTART.md](QUICKSTART.md) | This document: the full user guide |
| [../api/openapi.yaml](../api/openapi.yaml) | OpenAPI spec - the REST API reference |
| [../deployment/DEPLOYMENT_GUIDE.md](../deployment/DEPLOYMENT_GUIDE.md) | Docker/cloud deployment guide |
| [../cortrix-agent/README.md](../cortrix-agent/README.md) | Cortrix Agent service documentation |
| [../README.md](../README.md) | Project overview and architecture |
