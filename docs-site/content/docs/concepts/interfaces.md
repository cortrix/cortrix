---
title: Interfaces
description: The conventions behind Cortrix's agent-facing interfaces and structured errors.
weight: 30
---

Cortrix is agent-native, and that shows up as a set of conventions that run through every interface.

## What agent-native means for an interface {#agent-first}

When a call fails, a person can read the error message, look something up, and try a different approach. An agent needs information it can act on directly: can this error be retried, how long should it wait, is the result complete, and what should happen next.

Cortrix's interfaces are designed for that second need. The principle is that every API is designed for programs and agents first: errors are structured, states are enumerable, and degraded paths still return data instead of failing outright.

## Convention 1: errors are structured {#structured-errors}

Every error carries `code`, `retryable`, `category`, and `retry_after_ms`. An agent never needs to parse the message with a regular expression:

```json
{
  "error": {
    "code": "CX_ERR_DEPRECATED_FIELD",
    "message": "Field 'namespace' is deprecated, use 'namespaces' array instead",
    "retryable": false,
    "category": "permanent",
    "structured_data": { "deprecated_field": "namespace", "use_instead": "namespaces" }
  }
}
```

`structured_data` holds machine-readable details about the error. The example above tells the caller exactly which field to use instead.

See [Error codes](/docs/reference/errors/).

## Convention 2: partial failure still returns data {#partial-results}

In a cross-namespace query, one failing namespace doesn't fail the whole request. The parts that succeeded come back as usual, and `meta` says how much was covered, what failed, and why:

```json
{
  "meta": {
    "coverage_ratio": 0.5,
    "namespaces_succeeded": ["guide"],
    "namespaces_failed": [
      { "namespace": "nope", "error_code": "CX_ERR_INDEX_CORRUPT", "retryable": false, "category": "permanent" }
    ]
  }
}
```

An agent can use the results it has and then decide whether to retry the part that failed. Batch document submission works the same way.

The cost is that HTTP 200 no longer means "everything succeeded." Callers need to check `meta`.

## Convention 3: responses say what to do next {#next-action}

The progress of an asynchronous task carries an `agent_decision_hint` that says what to do next:

| Value | Meaning |
|---|---|
| `done_fetch_doc` | Processing finished. You can read the document |
| `inspect_error_code` | Processing failed. Look at the error code |

## Convention 4: endpoints describe themselves {#endpoint-metadata}

The OpenAPI spec annotates every endpoint with agent-facing metadata that can be read before the call:

| Metadata | The question it answers |
|---|---|
| Side-effect category | Is this call read-only, a write, or destructive? |
| Retryability and retry strategy | Can it be retried after a failure, and how? |
| Authorization scope | What permission does it need? |
| Rate limit | How often can it be called? |

An agent can use this to check with the user before it runs a destructive operation. For the full list of fields, see [Error codes](/docs/reference/errors/#agent-hints).

## Convention 5: calls can be traced {#traceability}

The MCP server attaches session, trace, and caller identifiers to every request. The SDK sends a caller identifier when you set `client_id`, and a trace identifier when you give it a `trace_id_provider`. The server adopts those identifiers when it records traces, and you can query them later by session:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/traces/<session_id>
```

When several agents work together, this lets you reconstruct what each one did.

## Convention 6: every access path means the same thing {#parity}

MCP tools, framework adapter methods, and HTTP endpoints share names and semantics. The framework adapters' 29 methods map one-to-one to the MCP server's regular tools. What you learn on one access path carries over to the others.

## Between the spec and the implementation {#spec-vs-implementation}

These conventions are design goals. In `v1.0.0-rc.2` there are still places where the implementation and the spec disagree. Testing for this documentation ran into several and recorded them:

- Some error codes don't use the unified `CX_ERR_` prefix.
- The memory edit and delete endpoints require parameters that differ from the OpenAPI definition.
- The watcher events endpoint exists in the spec but returns 404.
- A query `timeout_ms` outside the range the spec defines was accepted in testing with the `namespaces` array, although the server's request validation checks the range.

So when you integrate, go by the `retryable` and `category` in the actual response, and rerun your own smoke tests after each upgrade.

## Next steps {#next-steps}

- [Error codes](/docs/reference/errors/)
- [HTTP API reference](/docs/reference/api/)
- [Choose an access path](/docs/integrations/overview/)
