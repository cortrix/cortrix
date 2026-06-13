#!/usr/bin/env bash
# POST /api/v1/memory/search — error category=timeout (504, retryable=true)
# Trigger: memory index query timed out. Agent should sleep(retry_after_ms) then retry.
curl -X POST "https://api.cortrix.io/api/v1/memory/search" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ "query": "...", "namespace": "user_memory", "user_id": "user_001" }'
# → HTTP 504, see response.json
