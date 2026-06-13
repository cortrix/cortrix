#!/usr/bin/env bash
# POST /api/v1/query — error category=quota (429, retryable=true)
# Trigger: rate limit exceeded. Agent should sleep(retry_after_ms) then retry.
curl -X POST "https://api.cortrix.io/api/v1/query" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ "query": "...", "namespaces": ["contracts"], "top_k": 10 }'
# → HTTP 429, see response.json
