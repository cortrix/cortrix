# Agent Memory Correction — Self-Service Loop

> Best-practice guide for an Agent to find and undo a wrong memory invalidation
> (MEM02 D6 + D9 self-correction loop). This is the public counterpart of
> `cortrix.ai/docs/agent-memory-correction` (MEM02-rev-1b).

## Background — why invalidations are reversible

MEM02 extracts long-term memories (`fact` / `preference` / `event`) from
interactions. When a newly extracted fact contradicts an existing one (D5
judgment), MEM02 marks the **old** block `status = invalidated` and records *why*
— it never deletes it (the full-retention policy). Every invalidation is
therefore reversible.

Two facts make self-correction safe:

- **The old memory is preserved.** Invalidation only flips `status` in
  `blocks.metadata_json` and stamps the reason / confidence / triggering block —
  the content stays. Revoking restores `status = active`.
- **Low-confidence invalidations are flagged.** When the judge's confidence is
  below `mem02.invalidation.auto_revoke_confidence_threshold` (default `0.7`),
  the old block is marked `auto_revoke_eligible = true`, so an Agent can find the
  shaky calls and review them proactively.

The audit trail is the F18a operation_log (CE) — there is **no** separate
`memory_audit_log` table. MEM02 writes four `memory_*` actions:
`memory_extract` / `memory_invalidate` / `memory_revoke` / `memory_blocks_update`.

## The self-correction loop (Agent-driven)

1. **User signals a problem** — e.g. "your memory of me seems wrong lately."
2. **Agent reads the audit log** — the Python SDK has no dedicated memory audit method. Memory invalidations and revokes are recorded in the operations log: `GET /api/v1/operations?action_in=memory_invalidate,memory_revoke` (SDK: `client.ops.list_operations(action_in="memory_invalidate,memory_revoke")`; MCP: `cortrix_memory_get_audit`, which also includes extraction entries). Entries expose `action`, `resource_id`, `user_id`, `namespace_id`, `summary`, and `timestamp`; queries default to the caller's own user, and querying another user requires an admin key. `GET /api/v1/memory/invalidations` currently returns an empty list and a `source` pointer to this operations query.
3. **Agent explains and asks** — surface the human-readable reason ("on May 15
   you said you were 'traveling to Beijing', so I retired 'user is in Shanghai'
   — but that reads as a trip, not a move. Restore it?").
4. **On confirmation, Agent hands the revoke to an admin** — revoking is an admin action. An Agent using a normal read/write key cannot revoke: the route returns HTTP 403 with code `FORBIDDEN`, message `ADMIN permission required`, category `auth`, and `retryable: false`. After the user confirms, the Agent passes the invalidated memory's `block_id`, namespace, and reason to a human operator or a separate admin-keyed service trusted by the deployment. The admin calls `POST /api/v1/memory/invalidations/{id}/revoke`, where `{id}` is that `block_id`, with body `{"ns": "<namespace>", "reason": "<why>"}`. The old block's `status` goes `invalidated → active`, and a `memory_revoke` entry is written. In the response and audit, `revoked_by` identifies the authenticated admin caller; any `revoked_by` in the request body is ignored.
5. **Agent fixes the misclassification if needed** — PATCH cannot change `memory_type`. Create a corrected memory with `client.memory.create(namespace, content, memory_type="event")`, then invalidate the wrong one with `client.memory.delete(memory_id, namespace=namespace)`. To correct only the content, use `client.memory.update(memory_id, content=content, namespace=namespace)`.

> SDK / MCP naming (P12 v1.0.4 SoT): the audit + revoke tools are
> `cortrix_memory_get_audit` (#21) and `cortrix_memory_revoke_fact` (#22). The
> older `invalidations.list` / `invalidations.revoke` names are deprecated.
> SDK keys memories by `memory_id` (the SDK abstraction), not the internal
> C++ `block_id`.

`cortrix_memory_revoke_fact` calls the same admin route. It succeeds only when the MCP server runs with an admin key; a read/write key gets the same HTTP 403 `FORBIDDEN` response.

```python
from cortrix import Cortrix
client = Cortrix(base_url="http://localhost:8420/api/v1")

# 2. read recent invalidations
recent = client.ops.list_operations(
    action_in="memory_invalidate,memory_revoke", limit=20
)

# 4. admin side: revoke a wrong one after user confirmation and Agent handoff
# A read/write key gets HTTP 403 FORBIDDEN; use the admin caller's key.
import os
import httpx

block_id = "memory_old_fact"  # The invalidated memory's block_id from the handoff.
response = httpx.post(
    f"http://localhost:8420/api/v1/memory/invalidations/{block_id}/revoke",
    headers={"X-API-Key": os.environ["CORTRIX_ADMIN_API_KEY"]},
    json={"ns": "memory", "reason": "user confirmed: travel is not an address change"},
)
response.raise_for_status()

# 5. create the corrected event, then invalidate the misclassified memory
client.memory.create(
    "memory", "User traveled to Beijing on May 15, 2026.", memory_type="event"
)
client.memory.delete("memory_travel_bj", namespace="memory")
```

## Proactive review of low-confidence invalidations

An Agent does not have to wait for a complaint. There is currently no server-side `auto_revoke_eligible` filter in the operations query, so the Agent reviews recent invalidations itself:

```python
candidates = client.ops.list_operations(action="memory_invalidate")
# Review recent invalidations; hand the wrong ones to an admin for revoke.
```

## Error handling the Agent should expect

MEM02 returns the GEN-Agent error body (4 machine-readable fields + structured
data) with these `CX_ERR_MEM02_*` codes:

| code | category | retryable | what the Agent should do |
|---|---|:--:|---|
| `CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT` | timeout | yes | retry after `retry_after_ms` |
| `CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT` | transient | yes | retry; persistent → report |
| `CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED` | quota | no | back off; raise the day's budget |
| `CX_ERR_MEM02_CONTRADICTION_AMBIGUOUS` | transient | yes | retry; the judge was unsure |
| `CX_ERR_MEM02_LLM_DISABLED` | permanent | no | extraction is off (NullEnricher); only `interaction_log` is kept |

Revoke can also return `FORBIDDEN` (category `auth`, retryable: no): it needs an admin key. Hand the invalidation to an admin instead of retrying.

## Implementation status

This guide describes a design contract. The live endpoints, Query-pipeline contradiction
path, Python-middleware async worker, and MEM04 opt-out integration are not currently
implemented as one supported workflow. Do not treat the examples above as an executable
Quick Start.
