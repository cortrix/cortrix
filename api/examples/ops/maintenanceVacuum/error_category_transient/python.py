"""POST /api/v1/maintenance/vacuum — error CX_ERR_RATE_LIMIT (Python SDK skeleton)."""
from cortrix import Client
from cortrix.exceptions import CortrixError

client = Client(api_key="cx_live_xxx")

try:
    ...  # TODO: call the corresponding client method
except CortrixError as e:
    # Agent decision: route by e.retryable / e.category (retry / fallback / notify)
    print(e.code, e.category, e.retryable)
