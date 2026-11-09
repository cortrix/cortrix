# Error handling

Every Cortrix error response has the same shape, so an agent can decide what to do next from fields, without reading the message. The error object carries `code`, `message`, `retryable`, `category`, `retry_after_ms`, `structured_data`, and `request_id`. `category` is one of five values:

| `category` | Meaning |
|---|---|
| `auth` | the key is missing, invalid, or lacks a permission, or the namespace is not visible to it |
| `quota` | a limit was reached |
| `transient` | a temporary condition; the same request can succeed later |
| `permanent` | the request itself is wrong or can't be done; the same request fails again unchanged |
| `timeout` | the server did not finish in time |

`message` is for people and can change between releases. `structured_data` holds the machine-readable detail, such as the name of a missing parameter. The full schema is in [`api/components/errors.yaml`](../../api/components/errors.yaml).

Some operations answer HTTP 200 even when part of the work failed. `POST /api/v1/gc/restore` and cross-namespace queries list the failures per item (`failed[]`), with the same fields (`error_code` instead of `code`), and report how much succeeded in `coverage_ratio`. See [`../partial-success`](../partial-success) for the same contract on queries.

This example has two files:

- `decide.py` is the whole decision logic, about 60 lines of standard library Python. `decide()` takes one error object and returns an action. `decide_items()` applies it to every entry of a 200 response's `failed[]`. Copy it into your own agent.
- `demo.py` sends six requests that fail on purpose, prints each response, asks `decide()` what to do, and does it. It uses `urllib` and prints the raw fields, so you can see exactly what the server sends.

Neither needs an LLM key or the SDK.

## The rules

`decide()` checks these in order and never reads `message`:

| Check | Action | What the agent does |
|---|---|---|
| `code` is `CX_ERR_OPS_CONFIRM_REQUIRED` | `ask_human` | Send nothing. The operation is destructive and needs a person to confirm it. |
| `retryable` is true, or `category` is `timeout` | `retry` | Wait `retry_after_ms`, or back off 1 s, 2 s, 4 s when it is null. After 3 retries, `give_up`. |
| `category` is `auth` | `check_then_escalate` | Check the request first (does the namespace exist, is the name right). Only then ask for a key with more permission. |
| `category` is `quota` | `stop` | Retrying will not help. Report it. |
| `category` is `permanent` and `structured_data` names the next step | `fix_and_retry` | Do that step and send the request again. `missing_param` names a parameter to add; `live_doc_id` names a document to delete first. |
| `category` is `permanent` otherwise | `give_up` | Report the failure. The same request fails again. |

The demo doesn't trigger a retryable error; `decide()` handles it as in the table.

## Requirements

- A Cortrix server built from `main`, with API-key authentication on. Follow the [Quickstart](https://cortrix.ai/docs/get-started/quickstart/), but clone `main` instead of the release tag:

```bash
git clone --depth 1 https://github.com/cortrix/cortrix.git
cd cortrix
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

- Two API keys: a read-write key for the agent (`permissions: 3`) and a key that includes ADMIN (`permissions: 7`). Configure them as `auth.api_keys` entries in the server config, as described in [Compatibility](../../docs/compatibility.md#auth-and-security-boundaries) and [`config.yaml.example`](../../config.yaml.example). The config holds only the SHA-256 hash of each key. In a copy of `deploy/cortrix.yaml`, replace the `auth:` block with:

```yaml
auth:
  enabled: true
  api_keys:
    - key_hash: "<sha256 of the read-write key>"
      tenant_id: "default_tenant"
      permissions: 3
      expires_at: 0
    - key_hash: "<sha256 of the admin key>"
      tenant_id: "default_tenant"
      permissions: 7
      expires_at: 0
```

  Generate a key and its hash with:

```bash
key="$(python3 -c 'import secrets; print(secrets.token_hex(16))')"
printf %s "$key" | shasum -a 256 | cut -d' ' -f1
```

  The default config is baked into the image at `/app/config/cortrix.yaml`, and a bind-mounted file replaces it (see the comment at the top of `deploy/cortrix.yaml`). Mount your edited copy there and restart the server. Keep the keys themselves out of the config and out of Git.

- Python 3.10 or later. No packages to install.

## Run it

From the repository root:

```bash
export CORTRIX_API_KEY=<the read-write key>
export CORTRIX_ADMIN_KEY=<the admin key>
python examples/error-handling/demo.py
```

The script targets `http://127.0.0.1:8420`. Pass `--base-url` to use another address, and `--api-key` or `--admin-key` instead of the environment variables. Each run creates a namespace with a new name and deletes it at the end; pass `--keep` to leave it in place. A deleted namespace name can't be created again on the same server, which is why the names carry a timestamp.

The namespace is created and deleted with the admin key, because creating a namespace needs ADMIN permission. Everything the agent does in the scenarios uses the read-write key, except where a scenario says it hands over to the admin key.

Output from a build of `main` at `7bea4af`, CPU only, no LLM, authentication on:

```text
namespace: error-handling-1794211870
uploaded refund-policy.md: 01M6ZNRY20AA71XMSEVMW71BH8, completed

== a. Delete a document without the namespace
  request  : DELETE /documents/01M6ZNRY20AA71XMSEVMW71BH8
  response : HTTP 400 CX_ERR_MISSING_PARAM category=permanent retryable=false retry_after_ms=null
  structured_data: {"missing_param": "namespace"}
  decision : {"code": "CX_ERR_MISSING_PARAM", "category": "permanent", "action": "fix_and_retry", "add_param": "namespace"}
  next step: add namespace=error-handling-1794211870 and send it again -> HTTP 204

== b. Restore the document with a read-write key
  request  : POST /gc/restore document_ids=[01M6ZNRY20AA71XMSEVMW71BH8]
  response : HTTP 403 FORBIDDEN category=auth retryable=false retry_after_ms=null
  structured_data: {}
  decision : {"code": "FORBIDDEN", "category": "auth", "action": "check_then_escalate"}
  next step: check the request first: GET /namespaces/error-handling-1794211870 -> HTTP 200, the namespace exists
  next step: the key is the problem, so repeat with the admin key -> HTTP 200, coverage_ratio=1.0

== c. Use a namespace name with a typo
  request  : GET /namespaces/error-handling-1794211870-typo/documents
  response : HTTP 403 CX_ERR_NS_UNAUTHORIZED category=auth retryable=false retry_after_ms=null
  structured_data: {}
  decision : {"code": "CX_ERR_NS_UNAUTHORIZED", "category": "auth", "action": "check_then_escalate"}
  next step: check the request first: GET /namespaces -> HTTP 200, visible: error-handling-1794211870
  next step: error-handling-1794211870-typo is not in the list. A different key cannot help: check the namespace name

== d. Purge without the confirmation header
  request  : POST /gc/purge
  response : HTTP 400 CX_ERR_OPS_CONFIRM_REQUIRED category=permanent retryable=false retry_after_ms=null
  structured_data: null
  decision : {"code": "CX_ERR_OPS_CONFIRM_REQUIRED", "category": "permanent", "action": "ask_human"}
  next step: nothing sent. Purge removes every soft-deleted document on the server, so a person decides

== e. Restore two ids: the deleted document and a mistyped copy
  (setup: DELETE /documents/01M6ZNRY20AA71XMSEVMW71BH8?namespace=error-handling-1794211870 -> HTTP 204)
  request  : POST /gc/restore document_ids=[01M6ZNRY20AA71XMSEVMW71BH8, 01M6ZNRY20AA71XMSEVMW71BHX]
  response : HTTP 200 coverage_ratio=0.5
  item 01M6ZNRY20AA71XMSEVMW71BH8: succeeded
  item 01M6ZNRY20AA71XMSEVMW71BHX: CX_ERR_GC_DOC_NOT_FOUND category=permanent retryable=false
    decision : {"code": "CX_ERR_GC_DOC_NOT_FOUND", "category": "permanent", "action": "give_up", "why": "no next step in structured_data"}
  next step: give up on this id and report it

== f. Send a query that is not valid JSON
  request  : POST /query body={not json
  response : HTTP 400 INVALID_ARGUMENT category=permanent retryable=false retry_after_ms=null
  structured_data: {}
  decision : {"code": "INVALID_ARGUMENT", "category": "permanent", "action": "give_up", "why": "no next step in structured_data"}
  next step: the same request fails again unchanged. Give up and report it

DELETE /namespaces/error-handling-1794211870 -> 204
```

The ids and the timestamp in the names are new on every run.

## What each scenario shows

- **a. Missing parameter.** `DELETE /documents/{id}` needs `?namespace=`. The 400 carries `structured_data.missing_param`, so the agent adds the parameter and sends it again without reading the message.
- **b. Not allowed.** Restoring needs ADMIN permission, and a read-write key gets 403. The rule is to check before swapping keys: the agent confirms the namespace is there, then repeats the call with the admin key. Wherever your agent has no admin key, hand the ids to a person who does.
- **c. A typo in a namespace name.** With authentication on, a namespace you can't see answers 403 `CX_ERR_NS_UNAUTHORIZED`, whether it exists or not. The error says nothing about which, so a stronger key is the wrong guess. The agent lists the namespaces its key can see (`GET /api/v1/namespaces`), finds the name is not among them, and reports "check the namespace name".
- **d. A destructive call needs a person.** `POST /api/v1/gc/purge` without `X-Ops-Confirm: true` answers 400 `CX_ERR_OPS_CONFIRM_REQUIRED`. The agent stops there. The demo never sends the header: a purge removes every soft-deleted document on the server and can't be undone.
- **e. A 200 with failures.** Restoring two ids, one real and one with its last character changed, answers HTTP 200 with `coverage_ratio` 0.5. `decide_items()` reads the failed entry with the same rules: the typo id is permanent with nothing to act on, so the agent gives up on that id and keeps the restored document.
- **f. A malformed request.** A body that is not JSON gets a permanent 400. Sending it again changes nothing.

Without authentication (the Quick Start default), a missing namespace is not a 403 and any caller can restore, so scenarios b and c print why they are skipped and the run still exits with status 0. See [Compatibility](../../docs/compatibility.md#auth-and-security-boundaries) for how authentication changes these answers.
