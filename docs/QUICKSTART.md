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
    │  C++ backend :8080│  │  Python Agent :8001   │
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

**Two-stage unified LLM configuration (new v0.2 design):**

| Config section | Location | Description |
|--------|------|------|
| `llm_providers` | `build/config.yaml` | Provider registry: fill in an API Key to activate; the UI shows it automatically |
| `llm_roles` | `build/config.yaml` | Default assignment for the three roles; written to SQLite on first startup |
| UI runtime selection | Browser settings page | Each role independently selects a provider + model, persisted to SQLite |

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
2. Find the provider you need in `llm_providers`, fill in its `api_key`, and uncomment it
3. In `llm_roles`, fill in each role's default `provider` + `model` and uncomment it
4. Run `./dev.sh`; the corresponding provider and models appear automatically in the UI

> **Note**: `build/config.yaml` is not committed to git (`build/` is already in `.gitignore`),
> so API Keys are kept safe locally. `config.yaml.example` contains no real keys and can be committed safely.

**Minimal example** (using Zhipu GLM):

```bash
# Edit build/config.yaml, find the GLM section, uncomment it, and fill in the Key:
#
#   llm_providers:
#     - id: "glm"
#       name: "Zhipu GLM"
#       type: "openai_compat"
#       api_key: "your-real-key-here"     ← fill in here
#       base_url: "https://open.bigmodel.cn/api/paas/v4"
#       models:
#         - { id: "glm-4-flash", ... }
#         ...
#
#   llm_roles:
#     semantic_llm: { provider: "glm", model: "glm-4-flash" }   ← uncomment
#     vision_llm:   { provider: "glm", model: "glm-4v-flash" }
#     agent_llm:    { provider: "glm", model: "glm-4-flash" }
```

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

# LLM provider registry (uncomment and fill in api_key to activate)
llm_providers:
  - id: "glm"
    name: "Zhipu GLM"
    type: "openai_compat"
    api_key: "your-glm-api-key"   # ← fill in the real Key
    base_url: "https://open.bigmodel.cn/api/paas/v4"
    models:
      - { id: "glm-4-flash",  label: "GLM-4-Flash (free)",   caps: [text] }
      - { id: "glm-4v-flash", label: "GLM-4V-Flash (vision)",  caps: [text, vision] }

# Default provider + model for each role (written to the database on first startup)
llm_roles:
  semantic_llm: { provider: "glm", model: "glm-4-flash" }
  vision_llm:   { provider: "glm", model: "glm-4v-flash" }
  agent_llm:    { provider: "glm", model: "glm-4-flash" }

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
[cortrix]   Backend:  http://localhost:8080/api/v1/health
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
curl http://localhost:8080/api/v1/health

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
curl -X POST http://localhost:8080/api/v1/namespaces \
  -H "Content-Type: application/json" \
  -d '{"name": "my-docs"}'
```

### Upload Documents

```bash
# Upload a single file
curl -X POST http://localhost:8080/api/v1/namespaces/my-docs/documents \
  -F "file=@/path/to/document.pdf"

# Upload with attached metadata
curl -X POST http://localhost:8080/api/v1/namespaces/my-docs/documents \
  -F "file=@report.pdf" \
  -F 'metadata={"author":"Jane Doe","year":2025}'
```

### Query Document Processing Status

```bash
curl http://localhost:8080/api/v1/namespaces/my-docs/documents/1/status
# status: pending → processing → ready / error
```

### Semantic Search

```bash
curl -X POST http://localhost:8080/api/v1/query \
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

### Two-Stage Design

```
config.yaml (provider registry)    SQLite (runtime role selection)
──────────────────────────         ──────────────────────────
llm_providers:              →UI→   role          provider  model
  - id: "glm"                      semantic_llm  glm       glm-4-flash
    api_key: "xxx"                  vision_llm    glm       glm-4v-flash
    models: [...]                   agent_llm     glm       glm-4-flash
  - id: "openai"
    api_key: "yyy"
```

- **config.yaml** = which providers are available (credentials, model list)
- **SQLite** = which provider + model each role currently uses (persisted here after UI changes)
- **UI settings page** = runtime switching, takes effect immediately, no restart needed

### The Three LLM Roles

| Role | Purpose | Model suggestion |
|------|------|---------|
| `semantic_llm` | Intent classification + future reranking (called on every query) | fast/cheap, e.g. glm-4-flash |
| `vision_llm` | Secondary OCR image enhancement (optional feature) | must be a vision model, e.g. glm-4v-flash |
| `agent_llm` | Frontend RAG conversation | high quality, e.g. glm-4-plus / gpt-4o |

### Adding a New Provider

In the `llm_providers` section of `build/config.yaml`, uncomment the corresponding provider, fill in its `api_key`, and after a restart it appears in the UI:

```yaml
llm_providers:
  - id: "glm"
    name: "Zhipu GLM"
    type: "openai_compat"
    api_key: "your-glm-key"      # fill in the real Key, then uncomment
    base_url: "https://open.bigmodel.cn/api/paas/v4"
    models:
      - { id: "glm-4-flash",  label: "GLM-4-Flash (free)",  caps: [text] }
      - { id: "glm-4v-flash", label: "GLM-4V-Flash (vision)", caps: [text, vision] }
```

**Supported provider types:**

| Provider | `type` field | API Key source |
|------|------------|-------------|
| Zhipu GLM | `openai_compat` | https://open.bigmodel.cn |
| OpenAI | `openai` | https://platform.openai.com |
| Anthropic | `anthropic` | https://console.anthropic.com |
| Ollama (local) | `ollama` | no key needed, runs locally |

### How Configuration Changes Take Effect

| Change type | How it takes effect |
|---------|---------|
| Switching a role's model in the UI | Immediately, no restart needed |
| Adding/removing a provider in config.yaml | UI updates the provider list after a service restart |
| Editing `llm_roles` in config.yaml | Resets the role defaults after a restart (overrides SQLite) |
| Editing `api_key` in config.yaml | Takes effect after a service restart |

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
curl -X POST http://localhost:8080/api/v1/query \
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
  -p 8080:8080 \
  -v $(pwd)/data:/data \
  -v $(pwd)/config.yaml:/app/config.yaml \
  -v $(pwd)/models:/app/models \
  cortrix/cortrix:latest

# Check
curl http://localhost:8080/api/v1/health
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
        proxy_pass http://127.0.0.1:8080;
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
A: Check that at least one provider in `llm_providers` of `build/config.yaml` is uncommented and has an `api_key` filled in, and that `semantic_llm` in `llm_roles` has a matching `provider` + `model` configuration.

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
curl http://localhost:8080/api/v1/health
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
lsof -i :8080
# Or change the port in config.yaml
server:
  port: 9090
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
