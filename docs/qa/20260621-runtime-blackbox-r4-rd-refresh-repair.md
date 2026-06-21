# Runtime Black-box Round-4 RD Refresh Repair PR Guide

Status: 

Related Hub feedback package:



## Purpose

This PR applies lightweight, API/source-first repairs from the fourth-round clean-room runtime black-box run after the latest R&D refresh.

The patch keeps unresolved product/design issues visible. It does not implement partial fake endpoints for Settings roles or Admin Users, and it does not hide MEM02 extraction timeout or startup security events.

## First Reading Order

1. This file.
2. 
3. 
4. 
5. 
6. 
7. 
8. 
9. 
10. 
11. 
12. Web UI changes under 
13. Tests under , , , and 

## Changed Areas

### MCP and SDK contract alignment

-  now sends  for flat document status lookup.
-  now sends  and  according to the backend contract.
- MCP admin import filter now treats object filters as valid and string filters as expected invalid input.
- MCP  converts message arrays into backend  input.
- Python SDK  now sends  and  instead of stale  /  keys.

Review notes:

- The wrapper fixes are compatibility repairs against the current backend contract.
- MEM02 extraction itself still fails through the backend and remains a review item.

### Namespace and Memory UI contract

- The Web app resolves the current namespace from backend state before upload, chat, and memory requests.
- Upload, Chat, and Memory stores no longer call backend with stale  namespace after clean-room load.
- Memory UI list/create now sends backend-supported , cputime         unlimited
filesize        unlimited
datasize        unlimited
stacksize       7MB
coredumpsize    0kB
addressspace    unlimited
memorylocked    unlimited
maxproc         2666
descriptors     256, and , and normalizes  into UI pagination.

Review notes:

- This fixes the clean-room UI blocker chain without inventing a new namespace model.
- UI Settings roles and Admin Users are intentionally left blocked because their APIs are product/design decisions.

### OpenAPI split assets and local docs

- Backend exposes split OpenAPI YAML assets under  and .
- Vite dev proxy now forwards both  and  to backend.
-  is aligned with the documented document status behavior used by MCP.

Review notes:

- Runtime root  and root  remain the current docs contract.
- Stale  and  expectations should be reviewed as test/design drift, not silently added unless R&D wants aliases.

### Clean-room data-dir startup

- Server bootstrap now creates configured  before namespace/platform DB initialization.

Review notes:

- This removes the clean-room requirement to manually pre-create the runtime data directory.
- It is a startup hardening fix, not a storage architecture change.

### LLM transport observability

- HTTP transport now carries low-level  detail.
- OpenAI-compatible client includes transport failure detail when available.
- Unit coverage was added for transport-error detail propagation.

Review notes:

- This improves backend failure diagnosis but does not close MEM02 extraction timeout.
- R&D should decide whether MEM02 needs additional structured diagnostics above this transport layer.

### Static Web UI security headers

- Backend static Web UI serving now sets , , , and .
- Invalid  was removed from the HTML meta CSP because browsers ignore that directive in meta delivery.

Review notes:

- This closes the frontend CSP console warning and static Web smoke gap.
- Startup secret logging and LogSanitizer disabled remain open security events.

## Fourth-round Evidence

Canonical evidence root:



Primary files:

- 
- 
- 
- 
- 
- 
- 

Latest direct afterfix3 result:

| Area | Result |
|---|---:|
| UI routes | 10 pass / 2 blocked / 0 fail |
| API smoke | 20 direct pass + 4 expected-negative pass / 2 blocked / 0 fail |
| MCP real-httpx | 26 pass / 1 blocked / 0 fail |
| Static Web security smoke | pass |
| Startup events | 4 open events |

## Verification Already Run

Recorded in the evidence root:



This PR submission did not rerun the full suite; it packages the already-recorded fourth-round result.

## Remaining Blockers Not Solved In This PR

| ID | Surface | Current result | Why it remains open |
|---|---|---|---|
|  | API/MCP memory extract |  | Wrapper and transport repairs did not close backend MEM02 extraction. |
|  | UI Settings / Agent config |  404 | Current Agent exposes , , and ; roles API is a design decision. |
|  | UI Admin Users |  404 | Hub P08 describes the full admin users endpoint set; a partial fake endpoint would hide contract drift. |
|  | Runtime startup | plaintext bootstrap token and LogSanitizer disabled | Preserved as raw security evidence. |
|  | Runtime startup | bootstrap URL still prints  | Active backend port can differ. |
|  | Runtime startup/migration | duplicate  migration warning | Core smoke was not blocked, but startup remains noisy. |

## R&D Decisions Requested

1. Accept, replace, or split the MCP/SDK/UI/backend compatibility fixes in this PR.
2. Decide whether MEM02 timeout blocks merge or remains a known backend issue.
3. Decide whether Settings roles should be removed, hidden, re-mapped to , or restored as an Agent API.
4. Decide whether Admin Users should be implemented as the full P08 endpoint set, hidden in CE/current runtime, or edition-gated.
5. Decide whether , , , and  should stay stale expectations or become aliases.
6. Decide how to handle startup token logging, LogSanitizer config absence, bootstrap port mismatch, and duplicate migration warning.

## Files Intentionally Not Included

The local working tree also contains untracked  files. They are not part of this PR and should not be reviewed as part of the Round-4 repair patch.

## Boundary

Raw evidence is intentionally preserved 1:1 in the private CortrixGTM evidence root. This source PR summarizes and links that evidence; it does not sanitize or replace it.
