# Minimal agent examples

Two short scripts that show the retrieval step of an agent against Cortrix: one with the Python SDK, one through the MCP server. Both run against the Quick Start, need no LLM key, and print what an agent would act on.

Neither script calls a model. They stop where your agent takes over: with the passages to answer from, and a clear note about anything that couldn't be searched.

## Requirements

- A running Quick Start. See the [Quickstart](https://cortrix.ai/docs/get-started/quickstart/).
- Python 3.10 or later, in a virtual environment.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install cortrix cortrix-mcp mcp
```

Both scripts target `http://127.0.0.1:8420`. Pass `--base-url` to use another address.

> If every SDK call fails with a 502 while `curl` to the same address works, a system proxy is intercepting requests to your own machine. Run the scripts with `NO_PROXY=127.0.0.1,localhost`.

## Python SDK: `sdk_retrieval.py`

```bash
python examples/minimal-agent/sdk_retrieval.py
```

What it does:

1. Creates a namespace with a unique name and loads the three files in `notes/`.
2. Asks one question across that namespace and one that doesn't exist.
3. Sorts the response into three things: passages the agent can use, namespaces worth retrying, and namespaces it should tell the user it couldn't search.
4. Deletes the namespace it created. Pass `--keep` to leave it in place.

Output from `v1.0.0-rc.2` with `cortrix` 1.0.0rc2:

```text
loaded refund-policy.md: completed
loaded shipping.md: completed
loaded warranty.md: completed

question: How long does a customer have to request a refund?
coverage_ratio: 0.5
context 1: Customers can request a refund within thirty days of delivery. A request made after thirty days is reviewed by a support lead before any refund is issued.
context 2: Hardware is covered by a one-year limited warranty that starts on the delivery date. The warranty does not cover accidental damage.
context 3: Standard orders ship within two business days. Orders placed on a weekend ship on the following Monday.
not searched: not-a-real-namespace (permanent)
The agent should answer from the context above and say which part it could not search.
```

The `retrieve` function is the part to copy. It reads `coverage_ratio` and each entry in `namespaces_failed`, and decides from `retryable` -- not from the text of an error code. `category` is not part of the decision; it is carried through so the message to the user can say why a namespace was skipped.

The script waits for each upload by polling `documents.task_progress` itself. `documents.upload_and_wait` does the same in one call, but in `cortrix` 1.0.0rc2 it doesn't return for a finished task. See [#121](https://github.com/cortrix/cortrix/issues/121).

## MCP: `mcp_client.py`

```bash
python examples/minimal-agent/mcp_client.py
```

What it does:

1. Starts `cortrix-mcp` over stdio, the same way an MCP client such as Claude Code or Cursor does.
2. Lists the tools and calls `cortrix_health`.
3. Calls `cortrix_cross_ns_query` across the Quick Start's `demo` namespace and one that doesn't exist.

It doesn't write anything.

Output from `v1.0.0-rc.2` with `cortrix-mcp` 1.0.0rc2:

```text
tools: 31
health: alive (version 1.0.0-rc.2)
coverage_ratio: 0.5
results: 1
not searched: not-a-real-namespace (retryable=false, category=permanent)
session_id: mcp-session-87cd89523ad8
```

The `session_id` changes on every run.

The server also logs one `HTTP Request: ...` line to stderr per backend call -- two for this script. The block above is stdout only; add `2>/dev/null` to suppress the log.

Every tool result has two layers. `data` is the result of the call. `meta` tells the agent whether it can retry, and for a query its `structured_data` carries `coverage_ratio` and `namespaces_failed`, the same fields the HTTP API returns. A tool call that fails comes back with `isError` set: a one-line message followed by the structured error as JSON.

To connect an MCP client instead of running this script, see [MCP server](https://cortrix.ai/docs/integrations/mcp/).

## Next

- [Query and rerank](https://cortrix.ai/docs/guides/query/) and [Error codes](https://cortrix.ai/docs/reference/errors/) for the fields used here.
- [Python SDK](https://cortrix.ai/docs/integrations/python-sdk/) and [MCP tools](https://cortrix.ai/docs/reference/mcp-tools/) for the rest of the surface.
- [Compatibility and status](https://cortrix.ai/docs/resources/compatibility/) for what is `Preview` and what is still in development.
