"""GET /api/v1/namespaces/{ns_id}/acl — error CX_ERR_NOT_FOUND (Python SDK skeleton)."""
from cortrix import Client
from cortrix.exceptions import CortrixError

client = Client(api_key="cx_live_xxx")

try:
    ...  # TODO: call the matching client method
except CortrixError as e:
    # Agent decision: route by e.retryable / e.category (retry / fallback / notify)
    print(e.code, e.category, e.retryable)
