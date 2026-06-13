# Cortrix Python SDK

Official Python SDK for [Cortrix](https://cortrix.io) — Agent-first semantic
storage. Synchronous (`Cortrix`) and asynchronous (`AsyncCortrix`) clients with
a Resource-style API, full type hints (`py.typed`), and GEN-Agent-friendly error
handling.

> Status: v1.0.0. SDK surface follows the P03 detailed
> design (§ 2.12); HTTP wire follows the real Cortrix architecture. Real-server
> integration paths are exercised in D3.5.

## Installation

```bash
pip install cortrix
```

Requires Python 3.9+. The only runtime dependency is [`httpx`](https://www.python-httpx.org/).

## Quick start

```python
from cortrix import Cortrix

client = Cortrix(base_url="http://localhost:9090", api_key="cx_live_xxx")

# Create a namespace and upload a document (async processing -> task)
client.namespaces.create("contracts", display_name="Contracts")
task = client.documents.upload("contracts", "/path/to/contract.pdf")
print(task.task_id, task.status)

# Semantic search (single NS, cross-NS, or all-NS via ["*"])
results = client.search("contracts", "Party A breach clause", top_k=10)
for item in results.results:
    print(item.score, item.content)

# Coverage / partial-success metadata (AGENT_FRIENDLY A-class meta)
print(results.meta.coverage_ratio, results.meta.namespaces_succeeded)

client.close()
```

### Async

```python
import asyncio
from cortrix import AsyncCortrix

async def main():
    async with AsyncCortrix(base_url="http://localhost:9090", api_key="cx_live_xxx") as client:
        results = await client.search(["contracts", "support_docs"], "refund policy")
        return results

asyncio.run(main())
```

The async client mirrors the sync API exactly — every resource method has an
`async` counterpart.

## Resources

| Resource | Examples |
|---|---|
| `client.documents` | `upload` / `list` / `get` / `status` / `task_progress` / `cancel_task` / `delete` |
| `client.namespaces` | `create` / `list` / `get` / `update` / `delete` / `set_permission` |
| `client.search(...)` | top-level semantic query (`POST /query`) |
| `client.memory` | `search` / `log` / `list` / `create` / `update` (=`edit`) / `delete` (=`invalidate`) |
| `client.sql` | `query` (Text-to-SQL — extended deployments) |
| `client.watchers` | `add` / `list` / `remove` / `events` |
| `client.sync` | `configure` / `status` / `stop` (extended deployments) |
| `client.auth` | `register` / `login` / `logout` / `refresh` / `password_reset` / `me` |
| `client.system` | `health` / `version` / `namespace_stats` / `agent_llm_config` |
| `client.tenants` | `list` / `get` / `invite` / `update_role` / `quota` / `create` |
| `client.ops.gc` | `status` / `run` / `restore` / `purge` |
| `client.import_database(...)` | manual DB import (F16a) |

## Ops namespace

Runtime / maintenance operations live under `client.ops` (Agent-first — no web
console required). GC is the Phase 1 sub-namespace; `admin` / `tenant` are
Phase 2 evolution hooks.

```python
status = client.ops.gc.status()
if status.soft_deleted_count > 100:
    client.ops.gc.run()             # destructive -> sends X-Ops-Confirm: true
client.ops.gc.restore(["doc_abc"]) # restore soft-deleted documents
client.ops.gc.purge()              # permanent (irreversible) -> X-Ops-Confirm: true
```

## Agent-friendly error handling

Every exception derives from `CortrixError` and carries the four GEN-Agent
fields so Agent frameworks can make autonomous decisions:

```python
from cortrix import Cortrix, CortrixError, AuthInvalidCredentialsError, RateLimitError

client = Cortrix(api_key="cx_live_xxx")
try:
    client.documents.upload("docs", "report.pdf")
except AuthInvalidCredentialsError:
    relogin()                                  # precise L2 subclass
except RateLimitError as e:
    sleep((e.retry_after_ms or 1000) / 1000)   # honour server hint
except CortrixError as e:
    if e.category == "transient" and e.retryable:
        retry()
    elif e.category == "permanent":
        log_incident(e.error_code, e.structured_data)
    else:
        raise
```

The SDK retries automatically using the server's hints (priority:
`retryable` field → HTTP status; interval: `retry_after_ms` → `Retry-After`
header → exponential backoff). Configure with `max_retries`.

## Distributed tracing

```python
from opentelemetry.trace import get_current_span
from cortrix import Cortrix

def traceparent() -> str:
    ctx = get_current_span().get_span_context()
    return f"00-{ctx.trace_id:032x}-{ctx.span_id:016x}-01"

client = Cortrix(
    api_key="cx_live_xxx",
    client_id="my-rag-app",        # -> X-Client-Id header (distinguishes callers)
    trace_id_provider=traceparent, # -> traceparent header (W3C trace context)
)
```

`client_id` and `trace_id_provider` are injected on every request; a failing
provider never breaks the request. Phase 2 adds a `cortrix[otel]` extra for
automatic instrumentation (no manual provider needed).

## Development

```bash
pip install -e '.[dev]'
pytest --cov=cortrix          # unit tests (httpx mocked)
mypy --strict cortrix
ruff check cortrix
python scripts/generate_types.py   # regenerate types from the OpenAPI spec
```

## License

AGPL-3.0
