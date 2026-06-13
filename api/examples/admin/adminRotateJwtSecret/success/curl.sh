#!/usr/bin/env bash
# POST /api/v1/admin/auth/rotate-jwt-secret — success
curl -X POST "https://api.cortrix.io/api/v1/admin/auth/rotate-jwt-secret" \
  -H "X-API-Key: cx_live_xxx"
