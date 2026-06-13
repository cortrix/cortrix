#!/usr/bin/env bash
# POST /api/v1/gdpr/export/{user_id} — error (HTTP 401)
curl -X POST "https://api.cortrix.io/api/v1/gdpr/export/{user_id}" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 401, see response.json
