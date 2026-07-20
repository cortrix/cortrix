# API Examples

These files are generated reference assets. A language file may appear under `success/`
only when it contains a runnable success example. Generated Python SDK placeholders are
kept under `unsupported/python.py`; they must not be presented as working SDK examples.

The hand-refined `query/success/python.py` is the current Python success reference.

## Directory structure

One directory per resource domain (aligned with `paths/*.yaml`). One subdirectory per
endpoint contains success, unsupported, and error reference files:

```
examples/<domain>/<endpoint>/
├── success/
│   ├── curl.sh
│   ├── javascript.js
│   └── python.py                 # only when hand-refined and runnable
├── unsupported/
│   └── python.py                 # generated SDK placeholder; not a success example
├── error_category_<cat>/        # named after the ErrorResponseV1.category enum (not the HTTP status code)
│   ├── curl.sh
│   ├── python.py
│   ├── javascript.js
│   └── response.json
└── ...
```

## Naming convention

- Error scenario subdirectory = `error_category_{category}`, covering the 5 category enum values:
  `auth` / `quota` / `transient` / `permanent` / `timeout`.
- Optional HTTP-code suffix (e.g. `error_category_quota_429_rate_limit`).
- **Categorized by category** (the Agent-friendly schema dimension), which is more stable than the HTTP-status-code dimension.

## Validation contract

- No `success/python.py` may contain a TODO or placeholder SDK call.
- Generated Python SDK placeholders remain under `unsupported/` until implemented and tested.
- Documentation and tooling must not list `unsupported/` files as successful examples.
- Error scenarios remain categorized by retryability and error category.

## 15 resource domains

namespaces · documents · query · memory · sql · sync · watch ·
auth · admin · gdpr · system · tenants · acl · agent · ops
