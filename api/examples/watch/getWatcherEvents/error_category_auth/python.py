"""GET /api/v1/watch/{id}/events — error CX_ERR_AUTH_INVALID_API_KEY (Python SDK skeleton)."""
from cortrix import Client
from cortrix.exceptions import CortrixError

client = Client(api_key="cx_live_xxx")

try:
    ...  # TODO: call the corresponding client method
except CortrixError as e:
    # Agent decision: route by e.retryable / e.category (retry / degrade / notify)
    print(e.code, e.category, e.retryable)
