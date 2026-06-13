"""POST /api/v1/memory/search — success (Python SDK, user_id isolation required)."""
from cortrix import Client

client = Client(api_key="cx_live_xxx")

result = client.memory.search(
    query="the project progress the user mentioned last time",  # natural-language memory recall
    namespace="user_memory",
    user_id="user_001",
    top_k=5,
)
for m in result.memories:
    print(m.memory_type, m.status, m.content)
