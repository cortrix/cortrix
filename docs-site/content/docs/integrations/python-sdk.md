---
title: Python SDK
description: Use the Cortrix client in a Python application.
weight: 30
---

On this page you'll install the Cortrix Python SDK, connect to a local Cortrix service, and run a query.

The SDK provides a synchronous client, `Cortrix`, and an asynchronous client, `AsyncCortrix`, both fully typed. Its runtime dependency is `httpx`, plus `typing-extensions` on Python versions before 3.12.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- Python 3.9 or later.

## Install {#install}

Install the package from PyPI into a virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install cortrix
```

Confirm the installed version:

```bash
pip list | grep -E '^cortrix '
```

```text
cortrix                   1.0.0rc2
```

> [!NOTE]
> Cortrix is in pre-release, so pip installs the latest pre-release, `1.0.0rc2`.

## Your first query {#first-query}

Save this as `check.py`. It connects to the service the quickstart started and queries the `demo` namespace:

```python {title="check.py"}
from cortrix import Cortrix

client = Cortrix(base_url="http://127.0.0.1:8420")

print(client.system.health())

results = client.search(
    "demo",
    "What does semantic storage keep close to the agents that need it?",
    top_k=5,
)
for item in results.results:
    print(round(item.rerank_score, 3), item.metadata["source_path"])
print(results.meta.coverage_ratio, results.meta.namespaces_succeeded)

client.close()
```

Run it:

```bash
python check.py
```

```text
{'status': 'alive', 'uptime_seconds': 70, 'version': '1.0.0-rc.2'}
0.993 quickstart-demo.txt
1.0 ['demo']
```

The quickstart service has no authentication, so there's no `api_key` here. To connect to a service that requires one, use `Cortrix(base_url=..., api_key="your-cortrix-api-key")`.

> [!WARNING]
> If every call raises `CortrixError` with status code 502 while `curl` to the same address works, a system proxy is usually intercepting requests to your own machine. Bypass the proxy for local addresses and try again:
>
> ```bash
> NO_PROXY=127.0.0.1,localhost python check.py
> ```

## Client options {#client-options}

| Option | Purpose |
|---|---|
| `base_url` | The service address. Defaults to `http://localhost:8420` |
| `api_key` | An API key. Optional |
| `tenant_id` | A tenant identifier. Optional |
| `timeout` | The request timeout |
| `max_retries` | The maximum number of automatic retries |
| `http_client` | Pass in your own `httpx.Client` |
| `client_id` | Sent as the `X-Client-Id` header to tell callers apart |
| `trace_id_provider` | A function that returns a W3C `traceparent`, for distributed tracing |

## Query {#search}

`client.search()` maps to `POST /query`. The first argument can be a single namespace, a list of namespaces, or `["*"]` for all of them:

```python
results = client.search(["contracts", "support_docs"], "refund policy", top_k=10)
```

| Parameter | Default | Purpose |
|---|---|---|
| `top_k` | `10` | How many results to return |
| `rerank` | `True` | Whether to rerank |
| `include_sources` | `False` | Whether to return source information |
| `filters` | `None` | Filter conditions |

`coverage_ratio`, `namespaces_succeeded`, and `namespaces_failed` in `results.meta` tell you whether a cross-namespace query fully succeeded.

## Upload documents {#upload}

```python
client.namespaces.create("contracts", display_name="Contracts")
task = client.documents.upload("contracts", "/path/to/contract.pdf")
print(task.task_id, task.status)
```

Uploads are processed asynchronously and return a task. Track it with `client.documents.status()` or `client.documents.task_progress()`.

## Async client {#async}

The async client mirrors the sync client exactly. Every resource method has an `async` counterpart:

```python
import asyncio
from cortrix import AsyncCortrix

async def main():
    async with AsyncCortrix(base_url="http://127.0.0.1:8420") as client:
        return await client.search(["demo"], "semantic storage")

asyncio.run(main())
```

## Resources {#resources}

| Resource | Methods |
|---|---|
| `client.documents` | `upload`, `list`, `get`, `status`, `task_progress`, `cancel_task`, `delete` |
| `client.namespaces` | `create`, `list`, `get`, `update`, `delete`, `set_permission` |
| `client.search(...)` | Semantic query |
| `client.memory` | `search`, `log`, `list`, `create`, `update`, `delete` |
| `client.watchers` | `add`, `list`, `remove`, `events` |
| `client.system` | `health`, `version`, `namespace_stats`, `agent_llm_config` |
| `client.ops.gc` | `status`, `run`, `restore`, `purge` |
| `client.import_database(...)` {{< badge text="In development" tone="info" >}} | Start a database import manually |
| `client.tenants` {{< badge text="In development" tone="info" >}} | `list`, `get`, `invite`, `update_role`, `quota`, `create` |

Resources marked {{< badge text="In development" tone="info" >}} have no documentation yet.

## Handle errors {#error-handling}

Every exception derives from `CortrixError` and carries structured fields, so you can decide whether to retry or report:

```python
from time import sleep
from cortrix import Cortrix, CortrixError, RateLimitError

client = Cortrix(base_url="http://127.0.0.1:8420")
try:
    client.documents.upload("docs", "report.pdf")
except RateLimitError as e:
    sleep((e.retry_after_ms or 1000) / 1000)
except CortrixError as e:
    if e.category == "transient" and e.retryable:
        ...  # retry
    elif e.category == "permanent":
        print(e.error_code, e.structured_data)
    else:
        raise
```

The SDK retries automatically using the server's hints. To decide whether to retry, it looks at the `retryable` field first and then at the HTTP status code. For the wait time it uses `retry_after_ms`, then the `Retry-After` header, then exponential backoff. Adjust the number of attempts with `max_retries`.

## Operations {#ops}

Runtime and maintenance operations live under `client.ops`:

```python
status = client.ops.gc.status()
client.ops.gc.restore(["doc_abc"])
```

> [!CAUTION]
> `client.ops.gc.run()` and `client.ops.gc.purge()` are destructive. `purge()` deletes data permanently and can't be undone.

## Next steps {#next-steps}

- [MCP server](/docs/integrations/mcp/)
- [HTTP API reference](/docs/reference/api/)
- [Compatibility and status](/docs/resources/compatibility/)
