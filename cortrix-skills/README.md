# cortrix-skills

**Cortrix Skill SDK** — drop Cortrix into any LLM agent framework in one line.

`cortrix-skills` is the **Framework path** of Cortrix's three-path agent access
strategy:

| Path | Package | For |
|---|---|---|
| Direct | `cortrix` (Python SDK) | your own code / scripts |
| IDE | Cortrix MCP Server | Claude Desktop, Cursor, Qoder, … |
| **Framework** | **`cortrix-skills`** | **LangChain, Claude Tools, OpenAI Function Calling** |

It exposes a single `CortrixToolKit` with **29 methods** (1:1 with the Cortrix MCP
tools) and three framework adapters that turn that toolkit into native tools for
each framework — so a framework agent can search, upload, and manage memory in
Cortrix with zero hand-written tool glue.

---

## Install

```bash
pip install cortrix-skills                 # CortrixToolKit + Claude/OpenAI tool builders
pip install cortrix-skills[langchain]      # + LangChain adapter
pip install cortrix-skills[claude]         # + anthropic SDK (real Messages round-trip)
pip install cortrix-skills[openai]         # + openai SDK (real Chat round-trip)
pip install cortrix-skills[all]            # everything
```

The framework SDKs are **soft dependencies**: importing `cortrix_skills` never
requires `langchain` / `anthropic` / `openai`. Building Claude / OpenAI tool
definitions needs no framework SDK at all (the definitions are plain JSON). Each
adapter only fails — with a clear install hint — if you call it without its
framework installed.

---

## Quickstart

```python
from cortrix_skills import CortrixToolKit

kit = CortrixToolKit(base_url="https://cortrix.example.com", api_key="sk-cortrix-...")
result = kit.cortrix_query(query="quarterly revenue commentary", top_k=5)
```

### LangChain

```python
from langchain_anthropic import ChatAnthropic
from langchain.agents import create_react_agent, AgentExecutor
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_langchain_tools

kit = CortrixToolKit(base_url="https://cortrix.example.com", api_key="sk-cortrix-...")
tools = as_langchain_tools(kit)                       # 29 StructuredTools

agent = create_react_agent(ChatAnthropic(model="claude-..."), tools)
executor = AgentExecutor(agent=agent, tools=tools)
print(executor.invoke({"input": "find last week's notes on the P12 design"})["output"])
```

### Claude Tools (Anthropic Messages API)

```python
from anthropic import Anthropic
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_claude_tools
from cortrix_skills.adapters.claude import dispatch_claude_tool_use

kit = CortrixToolKit(base_url="https://cortrix.example.com", api_key="sk-cortrix-...")
tools = as_claude_tools(kit)                          # 29 tool definitions (JSON)

client = Anthropic(api_key="sk-ant-...")
resp = client.messages.create(
    model="claude-...", max_tokens=4096, tools=tools,
    messages=[{"role": "user", "content": "find last week's notes on the P12 design"}],
)
for block in resp.content:
    if block.type == "tool_use":
        tool_result = dispatch_claude_tool_use(kit, block)   # success or is_error=True + 4 fields
        # append tool_result to the next messages.create(...) call
```

### OpenAI Function Calling

```python
from openai import OpenAI
from cortrix_skills import CortrixToolKit
from cortrix_skills.adapters import as_openai_functions
from cortrix_skills.adapters.openai import dispatch_openai_tool_call

kit = CortrixToolKit(base_url="https://cortrix.example.com", api_key="sk-cortrix-...")
tools = as_openai_functions(kit)                      # 29 function definitions (JSON)

client = OpenAI(api_key="sk-...")
resp = client.chat.completions.create(
    model="gpt-4o-mini", tools=tools,
    messages=[{"role": "user", "content": "find last week's notes on the P12 design"}],
)
for call in resp.choices[0].message.tool_calls or []:
    content = dispatch_openai_tool_call(kit, call)     # JSON string for a role:"tool" message
```

---

## The 29 methods

`CortrixToolKit` mirrors the Cortrix MCP tools 1:1 (same names, same semantics).
Each method calls the Cortrix Python SDK (`cortrix`) underneath, falling back to
a direct HTTP call for endpoints the SDK does not expose yet.

| Group | Methods |
|---|---|
| Core (12) | `cortrix_health` · `cortrix_query` · `cortrix_upload` · `cortrix_list_documents` · `cortrix_list_namespaces` · `cortrix_create_namespace` · `cortrix_memory_search` · `cortrix_log_interaction` · `cortrix_list_interactions` · `cortrix_document_status` · `cortrix_add_watcher` · `cortrix_list_watchers` |
| Extended (4) | `cortrix_cross_ns_query` · `cortrix_async_upload` · `cortrix_memory_search_filter` · `cortrix_memory_extract_trigger` |
| New (4) | `cortrix_memory_extract` · `cortrix_task_status` · `cortrix_cancel_task` · `cortrix_query_explain` |
| Memory audit (2) | `cortrix_memory_get_audit` · `cortrix_memory_revoke_fact` |
| Memory opt-out (1) | `cortrix_memory_opt_out` |
| Bulk / ops (2) | `cortrix_batch_submit` · `cortrix_list_operations` |
| Memory CRUD (4) | `cortrix_memory_list` · `cortrix_memory_create` · `cortrix_memory_edit` · `cortrix_memory_invalidate` |

`cortrix_memory_invalidate` is a **soft delete** (`status=invalidated` +
`revoked_at`) — memories are never hard-deleted, so they stay auditable.

Admin tools are intentionally out of scope (use the MCP server or the Python SDK
admin namespace for those).

---

## Error handling (machine-readable, framework-native)

Cortrix errors are **passed through unchanged** from the Python SDK — this
package adds no error codes of its own. Every error carries four
agent-actionable fields:

```
code · retryable · category (auth|quota|transient|permanent|timeout) · retry_after_ms · structured_data
```

Each adapter surfaces those four fields in its framework's native error shape:

- **LangChain** — a `ToolException` whose message is the four-field JSON.
- **Claude Tools** — a `tool_result` block with `is_error=True` + four-field JSON.
- **OpenAI** — the four-field JSON returned as the tool message content.

So an agent can decide on its own whether to retry, back off (`retry_after_ms`),
or surface the failure — without parsing free-text error strings.

---

## Configuration

| Parameter | Default | Notes |
|---|---|---|
| `base_url` | — | required unless you pass `client=` |
| `api_key` | `None` | Cortrix API key (`sk-cortrix-...`) |
| `default_namespace` | `"default"` | used when a method's `namespace` is omitted |
| `client=` | — | inject a pre-built `cortrix.Cortrix` (or a mock, for tests) |

`CortrixToolKit` is lazy — no network call happens until the first method runs.
It supports `with CortrixToolKit(...) as kit:` and `kit.close()` to release the
underlying HTTP connection.

---

## Development

```bash
python -m venv .venv && source .venv/bin/activate
pip install -e ../sdk/python          # Cortrix Python SDK (editable)
pip install -e .[dev]                 # this package + pytest
pytest --cov=cortrix_skills
```

> Real LLM round-trips, the `spec_lint` three-way check run, and notebook
> execution are integration-stage activities and are not exercised by the unit
> suite (which mocks the SDK / HTTP layer).

## License

Apache-2.0. Historical `v1.0.0-rc.1` release artifacts remain under `AGPL-3.0-only`.
