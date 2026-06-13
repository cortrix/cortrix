#!/usr/bin/env bash
# POST /api/v1/documents — success (async upload, returns task_id)
curl -X POST "https://api.cortrix.io/api/v1/documents" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{
    "namespace": "contracts",
    "filename": "contract_001.pdf",
    "content": "...",
    "metadata": {"source": "legal_dept", "tags": ["contract", "2026"]}
  }'
# → HTTP 202 { "task_id": "task_xxx_001", "status": "submitted", ... }
