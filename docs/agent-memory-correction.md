# Agent Memory Correction — Self-Service Loop

> Best-practice guide for an Agent to find and undo a wrong memory invalidation
> (MEM02 D6 + D9 self-correction loop). This is the public counterpart of
> `cortrix.ai/docs/agent-memory-correction` (MEM02-rev-1b).
>
> Source of truth for the mechanism: `design/features/MEM02-llm-extraction.md`
> §6 (HTTP API), §7 (SDK), §8 (the full self-correction scenario).

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
2. **Agent reads the audit log** — `client.memory.get_audit(...)` lists recent
   invalidations with the invalidated block, the block that caused it, the
   reason, confidence, timestamp, and `auto_revoke_eligible`.
3. **Agent explains and asks** — surface the human-readable reason ("on May 15
   you said you were 'traveling to Beijing', so I retired 'user is in Shanghai'
   — but that reads as a trip, not a move. Restore it?").
4. **On confirmation, Agent hands the revoke to an admin** — revoking is an admin action. An Agent using a normal read/write key cannot revoke: the route returns HTTP 403 with code `FORBIDDEN`, message `ADMIN permission required`, category `auth`, and `retryable: false`. After the user confirms, the Agent passes the invalidated memory's `block_id`, namespace, and reason to a human operator or a separate admin-keyed service trusted by the deployment. The admin calls `POST /api/v1/memory/invalidations/{id}/revoke`, where `{id}` is that `block_id`, with body `{"ns": "<namespace>", "reason": "<why>"}`. The old block's `status` goes `invalidated → active`, and a `memory_revoke` entry is written. In the response and audit, `revoked_by` identifies the authenticated admin caller; any `revoked_by` in the request body is ignored.
5. **Agent fixes the misclassification if needed** — e.g. re-type the offending
   memory from `fact` to `event` via `client.memory.update(memory_id=..., ...)`.

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
recent = client.memory.get_audit(ns="memory", user_id="user_123",
                                 since="2026-05-15T00:00:00Z", limit=20)

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

# 5. correct the type of the memory that caused the bad invalidation
client.memory.update(memory_id="memory_travel_bj", memory_type="event")
```

## Proactive review of low-confidence invalidations

An Agent does not have to wait for a complaint. It can pull the
`auto_revoke_eligible` set and review the judge's weakest calls:

```python
candidates = client.memory.get_audit(auto_revoke_eligible=True,
                                     include_revoked=False)
# Review each low-confidence invalidation; hand the wrong ones to an admin for revoke.
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
Quick Start. Implementation notes remain in `design/features/MEM02-llm-extraction.md`.
