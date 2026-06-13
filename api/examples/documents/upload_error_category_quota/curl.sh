#!/usr/bin/env bash
# POST /api/v1/documents — error category=quota (403, retryable=false)
# Trigger: NS document quota is full. Agent should not retry; prompt to clean up or upgrade the quota.
curl -X POST "https://api.cortrix.io/api/v1/documents" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ "namespace": "contracts", "content": "...", "filename": "x.pdf" }'
# → HTTP 403, see response.json
