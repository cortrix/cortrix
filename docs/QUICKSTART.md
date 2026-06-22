# Cortrix Quickstart

This guide starts Cortrix from source, checks the backend, and gives you the first API calls to verify the local runtime.

Cortrix is in active pre-release development. For current feature status, read [Compatibility and known status](compatibility.md).

## Prerequisites

Minimum local tools:

- CMake
- A C++17 compiler
- OpenSSL development headers
- Node.js 20 or newer for the Web UI
- Python 3.9 or newer for document parsing, the Python SDK, and the built-in Agent
- Docker, only if you use the deployment templates

macOS:

```bash
xcode-select --install
brew install cmake openssl node
```

Ubuntu / Debian:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev git curl
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
```

## 1. Clone And Configure

```bash
git clone https://github.com/cortrix/cortrix.git
cd cortrix
mkdir -p build
cp config.yaml.example build/config.yaml
```

`build/config.yaml` is ignored by Git. Put real provider keys only in that local file or in local environment variables.

For a first backend smoke test, you can leave LLM roles disabled. LLM-backed features such as reranking, OCR enhancement, memory extraction, and built-in Agent chat require provider configuration.

## 2. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The backend binary is created at:

```text
build/cortrix-server
```

If you want a lighter build without ONNX semantic embeddings, use:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORTRIX_USE_ONNX=OFF
cmake --build build -j
```

BM25 full-text search can still work in this mode, but semantic search quality should not be treated as representative.

## 3. Start Cortrix

Recommended local path:

```bash
./dev.sh
```

`dev.sh` starts the backend, the built-in Agent service, and the Web UI when their dependencies are available.

Expected service URLs:

```text
Backend:  http://localhost:8420
Agent:    http://localhost:8001
Web UI:   http://localhost:5173
```

Manual backend-only startup:

```bash
./build/cortrix-server --config build/config.yaml
```

Manual Agent startup:

```bash
cd cortrix-agent
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn main:app --host 0.0.0.0 --port 8001
```

Manual Web UI startup:

```bash
npm install --prefix web
npm run dev --prefix web
```

## 4. Verify Health

Backend health:

```bash
curl http://localhost:8420/api/v1/health
```

Expected response shape:

```json
{
  "status": "healthy",
  "version": "0.1.0",
  "llm_enabled": false
}
```

Readiness:

```bash
curl http://localhost:8420/api/v1/system/health/ready
```

Expected response shape:

```json
{
  "status": "ok"
}
```

Agent health, if the Agent service is running:

```bash
curl http://localhost:8001/health
```

Expected response shape:

```json
{
  "status": "ok"
}
```

`llm_enabled: false` means the backend did not load a configured LLM role. That is acceptable for a basic backend smoke test. Configure the relevant LLM roles before testing LLM-backed features.

## 5. First API Calls

Create a namespace:

```bash
curl -X POST http://localhost:8420/api/v1/namespaces \
  -H "Content-Type: application/json" \
  -d '{"name": "quickstart"}'
```

List namespaces:

```bash
curl http://localhost:8420/api/v1/namespaces
```

Query:

```bash
curl -X POST http://localhost:8420/api/v1/query \
  -H "Content-Type: application/json" \
  -d '{
    "query": "What is Cortrix?",
    "namespace": "quickstart",
    "top_k": 5
  }'
```

Expected response shape:

```json
{
  "results": [],
  "meta": {
    "...": "..."
  }
}
```

An empty result set is acceptable before you upload documents. If the server returns a structured error, compare it with [Compatibility and known status](compatibility.md) and the [OpenAPI spec](../api/openapi.yaml).

## 6. Upload A Document

Create a small local file:

```bash
printf "Cortrix stores documents for agent-native retrieval.\\n" > quickstart.txt
```

Upload it:

```bash
curl -X POST http://localhost:8420/api/v1/namespaces/quickstart/documents \
  -F "file=@quickstart.txt"
```

Check document processing status using the returned document or task identifier. The exact response shape is part of the current API contract and should be verified against [OpenAPI](../api/openapi.yaml).

Query again after processing completes:

```bash
curl -X POST http://localhost:8420/api/v1/query \
  -H "Content-Type: application/json" \
  -d '{
    "query": "What does Cortrix store?",
    "namespace": "quickstart",
    "top_k": 5
  }'
```

## 7. Configure LLM Roles

LLM roles live in `build/config.yaml`.

Each role uses:

```yaml
semantic_llm:
  provider: "openai"
  api_key: "your-api-key"
  model: "gpt-4o-mini"
  base_url: "https://api.openai.com/v1"
```

Supported role names:

- `semantic_llm`
- `vision_llm`
- `agent_llm`
- `doc_summary_llm`
- `enricher_llm`

Changes take effect after restarting the relevant service.

Provider notes:

- OpenAI-compatible providers can be used by the C++ backend roles.
- The built-in Agent uses its Python provider adapters for `agent_llm`.
- If a provider uses a native protocol that is not OpenAI-compatible, use it only where the implementation supports that provider or route it through a compatible gateway.

## 8. Choose An Agent Access Path

Use [Agent access](agent-access.md) to choose the integration path:

- HTTP API / OpenAPI for custom clients.
- MCP for IDE Agents and MCP-compatible clients.
- Python SDK for Python applications.
- Built-in Agent for local fixed-flow chat.

## Troubleshooting

### The backend starts with `llm_enabled: false`

Check that the relevant role in `build/config.yaml` has `provider`, `api_key`, and `model` set. Some roles also need `base_url`.

### The Agent health check fails

Start the Agent service:

```bash
cd cortrix-agent
.venv/bin/uvicorn main:app --host 0.0.0.0 --port 8001
```

### A port is already in use

Find the process:

```bash
lsof -i :8420
```

Then stop that process or change the configured port.

### Semantic quality is poor

The ONNX embedding model may be missing or disabled. Verify your embedding model path in `build/config.yaml`.

### Auth, tenant, ACL, RBAC, quota, or memory extraction behaves differently from the docs

Check [Compatibility and known status](compatibility.md). Several advanced areas are documented in the API surface but remain blocked or under RD review in the current verification baseline.
