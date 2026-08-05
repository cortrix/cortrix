# Cortrix Python SDK

Official Python SDK for [Cortrix](https://cortrix.ai) — Agent-first semantic
storage. Synchronous (`Cortrix`) and asynchronous (`AsyncCortrix`) clients with
a Resource-style API, full type hints (`py.typed`), and structured error
handling.

> Status: `Verification required`. The SDK surface is documented and test-covered,
> but public-readiness labeling still depends on live API compatibility. See
> [Agent access](../../docs/agent-access.md) and
> [Compatibility](../../docs/compatibility.md).

## Installation

```bash
pip install cortrix
```

Requires Python 3.9+. The only runtime dependency is [`httpx`](https://www.python-httpx.org/).

## Quick start

```python
from cortrix import Cortrix

client = Cortrix(base_url="http://localhost:8420", api_key="your-cortrix-api-key")

# Create a namespace and upload a document (async processing -> task)
client.namespaces.create("contracts", display_name="Contracts")
task = client.documents.upload("contracts", "/path/to/contract.pdf")
print(task.task_id, task.status)

# Semantic search (single NS, cross-NS, or all-NS via ["*"])
results = client.search("contracts", "Party A breach clause", top_k=10)
for item in results.results:
    print(item.score, item.content)

# Coverage / partial-success metadata
print(results.meta.coverage_ratio, results.meta.namespaces_succeeded)

client.close()
```

Expected success signal: the client can reach the configured server and returns
resource objects or typed `CortrixError` exceptions. Check the server status in
[Compatibility](../../docs/compatibility.md) before relying on auth, tenant,
RBAC, quota, or memory extraction paths.

### Async

```python
import asyncio
from cortrix import AsyncCortrix

async def main():
    async with AsyncCortrix(base_url="http://localhost:8420", api_key="your-cortrix-api-key") as client:
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
| `client.import_database(...)` | manual database import |

Some resources map to API areas that are currently blocked or awaiting verification.
Do not treat resource presence as a production-readiness claim.

## Ops namespace

Runtime and maintenance operations live under `client.ops` so scripts and
Agents can perform server operations without a web console.

```python
status = client.ops.gc.status()
if status.soft_deleted_count > 100:
    client.ops.gc.run()             # destructive -> sends X-Ops-Confirm: true
client.ops.gc.restore(["doc_abc"]) # restore soft-deleted documents
client.ops.gc.purge()              # permanent (irreversible) -> X-Ops-Confirm: true
```

## Agent-friendly error handling

Every exception derives from `CortrixError` and carries structured fields so
Agent frameworks can make retry and escalation decisions:

```python
from cortrix import Cortrix, CortrixError, AuthInvalidCredentialsError, RateLimitError

client = Cortrix(api_key="your-cortrix-api-key")
try:
    client.documents.upload("docs", "report.pdf")
except AuthInvalidCredentialsError:
    relogin()                                  # precise L2 subclass
except RateLimitError as e:
    sleep((e.retry_after_ms or 1000) / 1000)   # honor server hint
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
    api_key="your-cortrix-api-key",
    client_id="my-rag-app",        # -> X-Client-Id header (distinguishes callers)
    trace_id_provider=traceparent, # -> traceparent header (W3C trace context)
)
```

`client_id` and `trace_id_provider` are injected on every request; a failing
provider never breaks the request. Automatic instrumentation is not required for
the minimal SDK path.

## Compatibility Notes

- Auth login is currently `Blocked` in the public status baseline.
- Tenant/member/ACL/quota behavior is currently `Blocked` pending contract
  reconciliation.
- memory extraction is currently `Blocked` because the latest runtime verification
  found an LLM transport timeout path.
- Built-in retry/error behavior should be verified against your target
  cortrix-server build.

## Development

```bash
pip install -e '.[dev]'
pytest --cov=cortrix          # unit tests (httpx mocked)
mypy --strict cortrix
ruff check cortrix
python scripts/generate_types.py   # regenerate types from the OpenAPI spec
```

## License

AGPL-3.0-only
