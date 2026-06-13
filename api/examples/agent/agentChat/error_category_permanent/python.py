"""POST /api/v1/agent/chat — error CX_ERR_INVALID_REQUEST (Python SDK skeleton)."""
from cortrix import Client
from cortrix.exceptions import CortrixError

client = Client(api_key="cx_live_xxx")

try:
    ...  # TODO: call the corresponding client method
except CortrixError as e:
    # Agent decision: route by e.retryable / e.category (retry / fallback / notify)
    print(e.code, e.category, e.retryable)
