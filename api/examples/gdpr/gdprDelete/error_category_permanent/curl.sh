#!/usr/bin/env bash
# POST /api/v1/gdpr/delete/{user_id} — error (HTTP 404)
curl -X POST "https://api.cortrix.io/api/v1/gdpr/delete/{user_id}" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 404, see response.json
