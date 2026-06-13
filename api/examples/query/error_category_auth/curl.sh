#!/usr/bin/env bash
# POST /api/v1/query — error category=auth (403, retryable=false)
# Trigger: API Key is missing permission for some NS. Agent should read structured_data.unauthorized_namespaces and query only the authorized NS (no retry).
curl -X POST "https://api.cortrix.io/api/v1/query" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ "query": "...", "namespaces": ["contracts", "finance", "hr"], "top_k": 10 }'
# → HTTP 403, see response.json
