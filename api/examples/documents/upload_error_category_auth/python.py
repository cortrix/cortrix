"""POST /api/v1/documents — error category=auth (403, retryable=false)."""
from cortrix import Client
from cortrix.exceptions import ForbiddenError

client = Client(api_key="cx_live_readonly")

try:
    task = client.documents.upload_async(namespace="contracts", content="...", filename="x.pdf")
except ForbiddenError as e:
    # retryable=false → notify the caller to update the API Key / request ns:write permission
    print("forbidden:", e.structured_data.get("required_scope"))
