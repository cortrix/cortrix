"""POST /api/v1/memory/search — error category=timeout (504, retryable=true)."""
import time
from cortrix import Client
from cortrix.exceptions import TimeoutError as CortrixTimeoutError

client = Client(api_key="cx_live_xxx")

for attempt in range(3):
    try:
        result = client.memory.search(query="...", namespace="user_memory", user_id="user_001")
        break
    except CortrixTimeoutError as e:
        # retryable=true → back off per retry_after_ms and retry
        time.sleep((e.retry_after_ms or 1000) / 1000)
