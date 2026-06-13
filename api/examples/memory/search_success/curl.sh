#!/usr/bin/env bash
# POST /api/v1/memory/search — success (memory semantic search, user_id required)
# Note: natural-language memory recall demo.
curl -X POST "https://api.cortrix.io/api/v1/memory/search" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{
    "query": "the project progress the user mentioned last time",
    "namespace": "user_memory",
    "user_id": "user_001",
    "top_k": 5
  }'
