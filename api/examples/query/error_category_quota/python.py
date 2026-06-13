"""POST /api/v1/query — error category=quota (429, retryable=true)."""
import time
from cortrix import Client
from cortrix.exceptions import RateLimitError

client = Client(api_key="cx_live_xxx")

try:
    result = client.query.run(query="...", namespaces=["contracts"], top_k=10)
except RateLimitError as e:
    # retryable=true + category=transient/quota → back off per retry_after_ms and retry
    time.sleep((e.retry_after_ms or 1000) / 1000)
    result = client.query.run(query="...", namespaces=["contracts"], top_k=10)
