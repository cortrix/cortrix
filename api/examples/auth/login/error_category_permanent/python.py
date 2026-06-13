"""POST /api/v1/auth/login — error CX_ERR_INVALID_REQUEST (Python SDK skeleton)."""
from cortrix import Client
from cortrix.exceptions import CortrixError

client = Client(api_key="cx_live_xxx")

try:
    ...  # TODO: call the matching client method
except CortrixError as e:
    # Agent decision: route by e.retryable / e.category (retry / fallback / notify)
    print(e.code, e.category, e.retryable)
