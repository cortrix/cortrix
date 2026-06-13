#!/usr/bin/env bash
# GET /api/v1/agent/sessions/{session_id} — error (HTTP 404)
curl -X GET "https://api.cortrix.io/api/v1/agent/sessions/{session_id}" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 404, see response.json
