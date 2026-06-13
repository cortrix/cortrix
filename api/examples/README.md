# API Examples — three languages × three scenarios

> S1 skeleton. Each endpoint's examples are filled in domain by domain across S4-S6.

## Directory structure

One directory per resource domain (aligned with `paths/*.yaml`). One subdirectory per endpoint, containing success + error scenarios,
each scenario containing **three languages** (curl / Python SDK / JS fetch) + `response.json` (error scenarios):

```
examples/<domain>/<endpoint>/
├── success/
│   ├── curl.sh
│   ├── python.py
│   └── javascript.js
├── error_category_<cat>/        # named after the ErrorResponseV1.category enum (not the HTTP status code)
│   ├── curl.sh
│   ├── python.py
│   ├── javascript.js
│   └── response.json
└── ...
```

## Naming convention (P04 § 8.2)

- Error scenario subdirectory = `error_category_{category}`, covering the 5 category enum values:
  `auth` / `quota` / `transient` / `permanent` / `timeout`.
- Optional HTTP-code suffix (e.g. `error_category_quota_429_rate_limit`).
- **Categorized by category** (the Agent-friendly schema dimension), which is more stable than the HTTP-status-code dimension.

## CI gate (P04 § 8.4)

- Each endpoint ≥1 success example + ≥2 error examples (including retryable=true 1 + retryable=false 1).
- A missing example blocks the PR.

## 15 resource domains

namespaces · documents · query · memory · sql · sync · watch ·
auth · admin · gdpr · system · tenants · acl · agent · ops
