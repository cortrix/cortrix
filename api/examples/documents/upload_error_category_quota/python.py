"""POST /api/v1/documents — error category=quota (403, retryable=false)."""
from cortrix import Client
from cortrix.exceptions import CortrixError

client = Client(api_key="cx_live_xxx")

try:
    task = client.documents.upload_async(namespace="contracts", content="...", filename="x.pdf")
except CortrixError as e:
    # category=quota + retryable=false → no retry; prompt to clean up / upgrade the quota
    if e.category == "quota":
        used = e.structured_data.get("ns_quota_used")
        limit = e.structured_data.get("ns_quota_limit")
        print(f"quota full: {used}/{limit} — clean up documents or upgrade the quota")
