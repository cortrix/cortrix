#!/usr/bin/env bash
# POST /api/v1/admin/users/{id}/disable — error (HTTP 404)
curl -X POST "https://api.cortrix.io/api/v1/admin/users/{id}/disable" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 404, see response.json
