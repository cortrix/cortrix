#!/usr/bin/env bash
# POST /api/v1/query — success (cross-NS semantic query)
# Note: natural-language semantic query demo.
curl -X POST "https://api.cortrix.io/api/v1/query" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{
    "query": "Party A breach-of-contract clause",
    "namespaces": ["contracts", "support_docs"],
    "top_k": 10,
    "rerank": true
  }'
