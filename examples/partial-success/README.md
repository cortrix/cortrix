# Partial success check

A Cortrix query can span several namespaces. When some of them can't be queried, the request still returns HTTP 200. The response says how complete the result is in `meta.coverage_ratio`, and lists what failed in `meta.namespaces_failed`.

This example shows that behavior with four requests against the Quick Start. It uses only the built-in `demo` namespace, so there is no data to load and no LLM key to configure. The script only sends queries. It doesn't create, change, or delete anything.

## Requirements

- A running Quick Start. See the [Quickstart](https://cortrix.ai/docs/get-started/quickstart/).
- `bash`, `curl`, and `python3`.

## Run it

From the repository root:

```bash
./examples/partial-success/check.sh
```

The script targets `http://127.0.0.1:8420`. To use another address, pass it as the first argument or set `CORTRIX_URL`:

```bash
./examples/partial-success/check.sh http://127.0.0.1:18420
```

To send your own query text, set `CORTRIX_QUERY`.

## What it checks

| Check | Request | What comes back |
|---|---|---|
| 1 | `namespaces: ["demo"]` | HTTP 200, `coverage_ratio` 1.0 |
| 2 | `namespaces: ["demo", "nope"]` | HTTP 200, `coverage_ratio` 0.5, the `demo` results, and `nope` listed in `namespaces_failed` |
| 3 | `namespaces: ["nope", "nope-2"]` | HTTP 200, `coverage_ratio` 0.0, no results, both namespaces listed in `namespaces_failed` |
| 4 | the deprecated singular `namespace` field | HTTP 400 `CX_ERR_DEPRECATED_FIELD` |

`nope` and `nope-2` stand for namespaces that don't exist.

This is written for the Quick Start's no-auth posture. With API-key authentication enabled, a namespace you cannot read -- including one that does not exist -- rejects the whole request with HTTP 403 `CX_ERR_NS_UNAUTHORIZED`, so the response never reveals whether a namespace exists. Checks 2 and 3 do not apply in that configuration.

Each check prints one line with the fields an agent acts on, then `PASS` or `FAIL`. The script exits with 0 when all four pass, 1 when a check fails, and 2 when Cortrix isn't ready at the address.

## Expected output

This is what the script prints. It has not been re-captured against a tagged release since the output format last changed, so treat it as the shape to expect rather than as a transcript of a specific version:

```text
== 1. Every namespace answers
   http=200 coverage_ratio=1.0 results=1 succeeded=demo failed_ns=- failed=-
   PASS
== 2. One namespace cannot be queried: still HTTP 200, coverage_ratio drops
   http=200 coverage_ratio=0.5 results=1 succeeded=demo failed_ns=nope failed=nope(CX_ERR_INDEX_CORRUPT retryable=false category=permanent)
   PASS
== 3. No namespace can be queried: still HTTP 200, coverage_ratio is 0 and results are empty
   http=200 coverage_ratio=0.0 results=0 succeeded=- failed_ns=nope,nope-2 failed=nope(CX_ERR_INDEX_CORRUPT retryable=false category=permanent),nope-2(CX_ERR_INDEX_CORRUPT retryable=false category=permanent)
   PASS
== 4. A malformed request is different: it gets a 4xx, not a partial result
   http=400 code=CX_ERR_DEPRECATED_FIELD retryable=false category=permanent
   PASS

All 4 checks passed.
```

## What to take from it

- The status code doesn't tell you whether a query result is complete. Check 3 returns 200 with no results at all. Read `coverage_ratio`.
- Each entry in `namespaces_failed` carries `retryable`, `category`, and `retry_after_ms`. Decide what to do from those fields. Don't match on the text of the error code.
- If you report totals, run evaluations, or reconcile data, treat a `coverage_ratio` below 1 as a failed query.
- A request that is itself wrong still gets a 4xx, as check 4 shows.

For the field reference and the other error categories, see [Query and rerank](https://cortrix.ai/docs/guides/query/), [Error codes](https://cortrix.ai/docs/reference/errors/), and the [troubleshooting entry on incomplete results](https://cortrix.ai/docs/resources/troubleshooting/#partial-results).
