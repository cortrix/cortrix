#!/usr/bin/env bash
# POST /api/v1/memory/search — error category=auth (401, retryable=false)
# Trigger: API Key invalid/revoked. Agent should not retry; update the API Key (re-create via POST /auth/api-keys).
curl -X POST "https://api.cortrix.io/api/v1/memory/search" \
  -H "X-API-Key: cx_live_revoked" \
  -H "Content-Type: application/json" \
  -d '{ "query": "...", "namespace": "user_memory", "user_id": "user_001" }'
# → HTTP 401, see response.json
