#!/usr/bin/env bash
# DELETE /api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id} — success
curl -X DELETE "https://api.cortrix.io/api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id}" \
  -H "X-API-Key: cx_live_xxx"
