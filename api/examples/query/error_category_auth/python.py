"""POST /api/v1/query — error category=auth (403, retryable=false)."""
from cortrix import Client
from cortrix.exceptions import ForbiddenError

client = Client(api_key="cx_live_xxx")

requested = ["contracts", "finance", "hr"]
try:
    result = client.query.run(query="...", namespaces=requested, top_k=10)
except ForbiddenError as e:
    # retryable=false → no retry; read the unauthorized NS from structured_data and degrade to querying only the authorized ones
    unauthorized = set(e.structured_data.get("unauthorized_namespaces", []))
    allowed = [ns for ns in requested if ns not in unauthorized]
    if allowed:
        result = client.query.run(query="...", namespaces=allowed, top_k=10)
