<!-- Hero -->
<div align="center">
  <img src="docs/assets/cortrix-logo.svg" width="200" alt="Cortrix logo">

  # Cortrix

  ### Agent-Native Semantic Storage Engine

  [![CI](https://img.shields.io/github/actions/workflow/status/cortrix/cortrix/ci.yml?branch=main)](https://github.com/cortrix/cortrix/actions)
  [![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)
  [![PyPI](https://img.shields.io/pypi/v/cortrix)](https://pypi.org/project/cortrix)
  [![Discord](https://img.shields.io/discord/0?label=Discord&logo=discord)](https://discord.gg/cortrix)
  [![GitHub stars](https://img.shields.io/github/stars/cortrix/cortrix?style=social)](https://github.com/cortrix/cortrix/stargazers)

  [Watch Demo Video (60s)](https://cortrix.ai) <!-- Video coming Phase 1.5 -->

  [Live Demo (Cloud)](https://cortrix.ai/cloud) · [Documentation](#-api-documentation) · [Discord](https://discord.gg/cortrix)
</div>

> **README is for humans.** For Agent / LLM consumption, read the machine-readable [OpenAPI spec](api/openapi.yaml) or connect a [MCP Server](#-agent-integration) directly.

---

## ✨ Why Cortrix?

- **Agent-first** — every API is designed for machine consumption: a 4-field error schema (`code` / `retryable` / `category` / `retry_after_ms`), `x-cortrix-*` OpenAPI hints (retryability, auth scope, typical latency, rate limit), and a native MCP Server.
- **Hybrid retrieval** — P-HNSW vector index + BM25 full-text scoring fused with RRF, plus cross-namespace parallel query in a single call.
- **Memory System** — conversational memory with `fact` / `preference` / `event` classification, automatic invalidation, and immune decay.
- **PG-native Access (pgcortrix)** — a drop-in PostgreSQL extension: query Cortrix semantically from any SQL client, BI tool, or existing PG stack via `SELECT * FROM pgcortrix_search(...)`. Zero migration, full ecosystem — `psql`, pgAdmin, DBeaver, Grafana, and Superset all work out of the box. DBA-friendly governance via SQL. (See [`sql-extensions/pgcortrix`](sql-extensions/pgcortrix/) for the full integration matrix.)
- **Open license** — AGPL-3.0; commercial licenses available for closed-source use.

> **Note:** `pgcortrix` is Cortrix's PG-native access surface — it is **not** part of any benchmark head-to-head against `pgvector` (different role: ecosystem access vs. vector index). It is presented here as an integration capability — "consume Cortrix semantic retrieval with your existing PostgreSQL tooling."

---

## 🚀 Quick Start (3 minutes)

### Step 1: Start Cortrix Server (Docker, demo data pre-loaded)

```bash
docker run -d -p 8080:8080 \
  -v cortrix-data:/data \
  cortrix/cortrix-demo:v1.0   # Includes a pre-built "demo" namespace (5 self-referential docs)
```

Verify it's running:

```bash
curl http://localhost:8080/api/v1/system/health/ready
# → {"status": "ok", "version": "1.0.0"}
```

### Step 2: Install the Python SDK

```bash
pip install cortrix   # Python 3.9+
```

### Step 3: Query

```python
from cortrix import Cortrix

# The SDK owns the /api/v1 prefix — pass only the host.
client = Cortrix(base_url="http://localhost:8080")
results = client.search("demo", "What is Cortrix?", top_k=10)
for r in results.results:
    print(f"[{r.score:.2f}] {r.content[:200]}")
```

Expected output:

```
[0.92] Cortrix is an open-source Agent-Native Semantic Storage Engine...
[0.87] Cortrix provides semantic storage with hybrid retrieval (vector + BM25)...
[0.83] ...
```

<details>
<summary>curl example</summary>

```bash
curl -X POST http://localhost:8080/api/v1/query \
  -H "Content-Type: application/json" \
  -d '{"namespaces": ["demo"], "query": "What is Cortrix?", "top_k": 10}'
```
</details>

<details>
<summary>JavaScript (fetch) example</summary>

```javascript
const response = await fetch("http://localhost:8080/api/v1/query", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ namespaces: ["demo"], query: "What is Cortrix?", top_k: 10 }),
});
const { results } = await response.json();
```
</details>

### Alternative: Build from Source

```bash
git clone https://github.com/cortrix/cortrix
cd cortrix
docker compose -f deploy/docker-compose.yml up -d   # Builds + starts the server
```

> **Troubleshooting**
> - *Docker not running*: ensure Docker Desktop is started.
> - *Port already allocated*: override with `CORTRIX_PORT=18080 docker compose -f deploy/docker-compose.yml up` (avoid `9091`, used by the OpenMetrics endpoint).
> - *`pip install` times out*: use a mirror, e.g. `pip install -U cortrix --index-url https://pypi.org/simple/`.
> - *First query is slow / returns 0 results*: the demo index may build on first use (5–30s); retry once.
> - *Out of memory*: the bge-m3 embedding model needs ≥ 4 GB; ≥ 8 GB RAM is recommended. Check with `docker stats`.

### Next Steps

- **[Cross-NS Query](docs/tutorial-cross-ns.md)** — parallel search across multiple namespaces (a Cortrix-unique feature).
- **[Memory System Tutorial](docs/tutorial-memory.md)** — `fact` / `preference` / `event` auto-classification.
- **[Upload Your Own Data](docs/tutorial-upload.md)** — connect your business documents.
- **[Agent Integration](#-agent-integration)** — connect via MCP / HTTP / LangChain / etc.

---

## 🤖 Agent Integration

Cortrix is built for AI Agents. There are three ways to connect.

### MCP Server (Claude Code / Cline / Cursor) ⭐ Recommended

The simplest path for Agent IDE users:

```json
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://localhost:8080",
        "CORTRIX_NAMESPACE": "demo",
        "CORTRIX_API_KEY": "your-api-key"
      }
    }
  }
}
```

Claude Code / Cline / Cursor will auto-discover **31 Cortrix tools** (search, memory, upload, extract, task status, and more). See the [`cortrix-mcp/`](cortrix-mcp/) README for the full tool reference.

### Direct HTTP API (for custom Agents)

```python
# Any Agent framework can call the Cortrix HTTP API directly:
import requests

response = requests.post(
    "http://localhost:8080/api/v1/query",
    json={"namespaces": ["demo"], "query": "What is Cortrix?", "top_k": 10},
)
```

Cortrix returns Agent-friendly responses:

- **4-field error schema**: `{code, retryable, category, retry_after_ms}` — see [Agent-Friendly design](docs/agent-friendly.md).
- **`x-cortrix-*` OpenAPI extensions**: `retryable`, `auth_scope`, `typical_latency` (P50/P99), `rate_limit`.

See the [OpenAPI spec](api/openapi.yaml) for the full reference (71+ endpoints).

### Framework Integrations (via Cortrix Skill SDK)

Available now — 2 adapters (`pip install cortrix-skills[langchain|claude|all]`):

- **LangChain** — `from cortrix_skills.adapters.langchain import as_langchain_tools` (29 LangChain tools).
- **Claude Tools** — `from cortrix_skills.adapters.claude import as_claude_tools` (29 tool definitions for the Anthropic API `tool_use`).

Coming in Phase 1.5:

- **LlamaIndex** — `from cortrix_skills.adapters.llamaindex import ...`
- **OpenAI Function Calling** — `from cortrix_skills.adapters.openai import ...`

---

## 📚 Examples / Use Cases

### 1. RAG for an AI Assistant (LangChain)

```python
from cortrix_skills.adapters.langchain import CortrixRetriever
from langchain.chains import RetrievalQA

retriever = CortrixRetriever(base_url="http://localhost:8080", namespace="knowledge_base")
qa = RetrievalQA.from_chain_type(llm=..., retriever=retriever)
answer = qa.run("How do I configure Cortrix?")
```

### 2. Long-term Agent Memory

```python
from cortrix import Cortrix

client = Cortrix(base_url="http://localhost:8080")

# Log an interaction — Cortrix auto-extracts memory (type=preference, "user prefers dark mode").
client.memory.log(
    "support",
    query="Set my theme",
    response="Switched the UI to dark mode.",
    user_id="user_123",
)

# Retrieve memory for a user.
preferences = client.memory.search(
    "support",
    "UI preferences",
    user_id="user_123",
)
```

### 3. Semantic SQL (pgcortrix)

```sql
-- Drop-in PostgreSQL extension (V1: plpython3u + HTTP to the Cortrix Server).
CREATE EXTENSION plpython3u;     -- Prerequisite (untrusted PL; superuser install)
CREATE EXTENSION pgcortrix;

SELECT pgcortrix_configure('your-api-key');

SELECT * FROM pgcortrix_search('contracts', 'breach of confidentiality clauses', top_k := 10);
```

| Use case | Why it matters | Audience |
|---|---|---|
| RAG (LangChain) | Plug Cortrix straight into the LangChain ecosystem | AI Agent / RAG developers |
| Long-term Memory | Cortrix's own Memory System (`fact` / `preference` / `event`) | Agent / chatbot developers |
| Semantic SQL (pgcortrix) | Data-infrastructure positioning — not just a vector library | DBAs / data teams |

---

## 📖 API Documentation

### Self-Deployed (Community Edition)

The three main paths for Cortrix CE users:

- **Local Swagger UI** — `http://localhost:8080/docs` (after `docker compose up` or `cortrix-server` start).
- **OpenAPI Spec** — [`api/openapi.yaml`](api/openapi.yaml) — OpenAPI 3.0, 71+ endpoints.
- **Python SDK** — [`pip install cortrix`](https://pypi.org/project/cortrix) — generated from the spec.

### Managed Service

- **Cortrix Cloud** (managed, no install) — [cortrix.ai/cloud](https://cortrix.ai/cloud) — launching Phase 1.5.

---

## 📊 Benchmarks

| Test | Cortrix | Baseline | Improvement |
|---|---:|---:|---:|
| Recall@10 (CN dataset) | **0.92** | 0.78 (pgvector) | **+18%** |
| Cross-NS Query (P50) | **180 ms** | N/A (Cortrix-unique) | — |
| Memory Search (P99) | **320 ms** | N/A | — |
| Build Index (100k docs) | **12 min** | 18 min (pgvector) | **1.5×** |
| Storage (per 1M chunks) | **4.2 GB** | 5.8 GB (pgvector) | **−28%** |

> Numbers from `cortrix-bench` v1.0 on the CN dataset `cortrix-ragdata-cn-v1`.
> Hardware: AWS m6i.xlarge (4 vCPU / 16 GB RAM). Final figures land with the v1.0 release.

[Full benchmarks →](BENCHMARK.md) · [Methodology →](BENCHMARK.md#methodology) · [Reproduce →](https://github.com/cortrix/cortrix-bench)

---

## 🎯 Roadmap

- ✅ **v1.0 (Now)** — Community Edition: Cortrix Server + P-HNSW + BM25 hybrid + pgcortrix + MCP Server + Python SDK.
- 🚧 **v1.5 (Q3 2026)** — Cloud SaaS launch + Web UI + async document processing + LlamaIndex / OpenAI adapters.
- 🔮 **v2.0 (Q1 2027)** — multi-tenant + TypeScript SDK + advanced RAG.

---

## 💬 Community

- [Discord](https://discord.gg/cortrix) — real-time chat.
- [GitHub Discussions](https://github.com/cortrix/cortrix/discussions) — Q&A.
- [Twitter @CortrixAI](https://twitter.com/CortrixAI) — updates.
- [hello@cortrix.ai](mailto:hello@cortrix.ai)

---

## 🤝 Contributing

We welcome contributions! See [CONTRIBUTING.md](CONTRIBUTING.md) for the development setup, branch workflow, and PR checklist.

## 📄 License

AGPL-3.0 — see [LICENSE](LICENSE) for details. Commercial licenses are available for closed-source use — contact [hello@cortrix.ai](mailto:hello@cortrix.ai).
