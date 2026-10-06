---
title: Error codes
description: The structure of an error response, the error categories, and common error codes.
weight: 40
---

Cortrix error responses are designed for programs and agents. Whether to retry, how long to wait, and what kind of error it is each have a dedicated field, so you never need to parse the message text.

## Error response structure {#structure}

```json
{
  "error": {
    "code": "CX_ERR_DEPRECATED_FIELD",
    "message": "Field 'namespace' is deprecated, use 'namespaces' array instead",
    "retryable": false,
    "category": "permanent",
    "retry_after_ms": null,
    "structured_data": {
      "deprecated_field": "namespace",
      "use_instead": "namespaces"
    }
  }
}
```

| Field | Type | Description |
|---|---|---|
| `code` | string | The error code |
| `message` | string | A description for people. Don't parse it in code. The structured details are in `structured_data` |
| `retryable` | boolean | Whether a retry is worthwhile |
| `category` | string | The error category. See the next table |
| `retry_after_ms` | integer or null | The suggested wait before a retry, in milliseconds |
| `structured_data` | object | Structured data about this error, for programs to use |
| `request_id` | string | A server-generated request identifier for finding the request in logs. Some error responses include it |

## Error categories {#categories}

| `category` | Meaning | What to do |
|---|---|---|
| `auth` | An authentication or permission problem | Don't retry. Check your credentials or permissions |
| `quota` | A quota was exceeded | Slow down, or wait and try again |
| `transient` | A temporary failure | Wait for `retry_after_ms`, then retry |
| `timeout` | A timeout | Wait for `retry_after_ms`, then retry |
| `permanent` | Something is wrong with the request itself | Don't retry it unchanged. Fix the request |

## Decide whether to retry {#retry-decision}

1. Look at `retryable` first. If it's `false`, don't retry.
2. For the wait time, use `retry_after_ms`, then the `Retry-After` header (in seconds), then exponential backoff.

The Python SDK has this logic built in. See [Python SDK](/docs/integrations/python-sdk/#error-handling).

## Partial success {#partial-success}

Some operations return HTTP 200 even when part of the work failed. The details are in the response's `meta`, and every failure carries the same structured fields:

| Operation | Where the failures are |
|---|---|
| Cross-namespace query | `meta.namespaces_failed[]` |
| Batch document submit | `meta.failed[]` |

When an asynchronous task fails, the failure appears in the task progress as `error_code` and `error_msg`, not in the response to the request that submitted it.

So don't judge whether an operation fully succeeded from the HTTP status code alone.

## How error codes are named {#naming}

The spec requires every error code to use the `CX_ERR_` prefix, which lets you filter Cortrix errors in logs and monitoring.

> [!NOTE]
> In testing, some errors in `v1.0.0-rc.2` used generic codes without the prefix, such as `INVALID_ARGUMENT` and `NOT_FOUND`. When you handle errors, go by `retryable` and `category`, and don't assume every code starts with `CX_ERR_`.

## Common error codes {#common-codes}

These are the high-frequency codes registered in the spec, with the suggested way to handle each:

| Error code | Retryable | Category | Typical cause | What to do |
|---|:--:|---|---|---|
| `CX_ERR_NAMESPACE_NOT_FOUND` | No | `permanent` | The namespace name is misspelled, or the namespace was deleted | Confirm the name with `GET /namespaces`, or create the namespace |
| `CX_ERR_NS_UNAUTHORIZED` | No | `auth` | The API key has no access to this namespace | Read the namespaces you can't access from `structured_data.unauthorized_namespaces` and query only the ones you can |
| `CX_ERR_NS_TIMEOUT` | Yes | `timeout` | A query on one namespace timed out | Use the partial results you got, and optionally retry just that namespace |
| `CX_ERR_RATE_LIMIT` | Yes | `transient` | The rate limit was exceeded. The `v1.0.0-rc.2` server doesn't emit this code | Wait for `retry_after_ms` and retry, or lower your request rate |
| `CX_ERR_AUTH_INVALID_API_KEY` | No | `auth` | The API key is invalid or revoked | Use a different API key |
| `CX_ERR_INDEX_CORRUPT` | No | `permanent` | The vector index is unavailable | Use the partial results and notify whoever operates the service |

These are the error codes that came up in testing for this documentation:

| Error code | HTTP status | When it appears |
|---|:--:|---|
| `CX_ERR_DEPRECATED_FIELD` | 400 | A query request used the singular `namespace` |
| `CX_ERR_NS_NOT_FOUND` | 404 | A file was uploaded to a namespace that doesn't exist |
| `CX_ERR_ADMIN_LOOPBACK_REQUIRED` | 403 | An admin endpoint was called from a non-loopback address |
| `CX_ERR_SPC_PROCESS_FAILED` | — | An asynchronous processing task failed. It appears in the task progress |

For the MCP adapter's own error codes, see [MCP server](/docs/integrations/mcp/#response-schema).

## Agent hints in the OpenAPI spec {#agent-hints}

The OpenAPI spec annotates every endpoint with a set of `x-cortrix-*` extension fields, so an agent knows what kind of endpoint it's dealing with before it calls it:

| Field | Meaning |
|---|---|
| `x-cortrix-retryable` | Whether this endpoint can usually be retried |
| `x-cortrix-retry-strategy` | The suggested way to retry: `exponential_backoff`, `linear`, `no_retry`, or `immediate` |
| `x-cortrix-side-effects` | The side-effect category: `none`, `read_only`, `write`, or `destructive` |
| `x-cortrix-auth-required` | Whether authentication is required |
| `x-cortrix-auth-scope` | The required scope: `ns:read`, `ns:write`, `tenant:admin`, or `ops:admin` |
| `x-cortrix-rate-limit` | The rate limit, as `count/period/scope` |
| `x-cortrix-typical-latency` | Estimated P50 and P99 latency |

These fields are design annotations in the spec, not measured results.

## Next steps {#next-steps}

- [HTTP API reference](/docs/reference/api/)
- [Troubleshooting](/docs/resources/troubleshooting/)
