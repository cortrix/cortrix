"""POST /api/v1/memory/search — error category=auth (401, retryable=false)."""
from cortrix import Client
from cortrix.exceptions import AuthInvalidApiKeyError

client = Client(api_key="cx_live_revoked")

try:
    result = client.memory.search(query="...", namespace="user_memory", user_id="user_001")
except AuthInvalidApiKeyError:
    # retryable=false → no retry; re-create the API Key
    print("API Key invalid — recreate via POST /auth/api-keys")
