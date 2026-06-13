#!/usr/bin/env bash
# POST /api/v1/admin/users/{id}/enable — success
curl -X POST "https://api.cortrix.io/api/v1/admin/users/{id}/enable" \
  -H "X-API-Key: cx_live_xxx"
