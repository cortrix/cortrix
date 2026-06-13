#!/usr/bin/env bash
# DELETE /api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id} — error (HTTP 401)
curl -X DELETE "https://api.cortrix.io/api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id}" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 401, see response.json
