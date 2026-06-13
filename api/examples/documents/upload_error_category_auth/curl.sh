#!/usr/bin/env bash
# POST /api/v1/documents — error category=auth (403, retryable=false)
# Trigger: API Key lacks write permission for this NS. Agent should not retry; update the key or request permission.
curl -X POST "https://api.cortrix.io/api/v1/documents" \
  -H "X-API-Key: cx_live_readonly" \
  -H "Content-Type: application/json" \
  -d '{ "namespace": "contracts", "content": "...", "filename": "x.pdf" }'
# → HTTP 403, see response.json
