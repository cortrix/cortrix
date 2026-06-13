"""POST /api/v1/query — success (Python SDK, cross-NS semantic query)."""
from cortrix import Client

client = Client(api_key="cx_live_xxx")

result = client.query.run(
    query="Party A breach-of-contract clause",  # natural-language semantic query
    namespaces=["contracts", "support_docs"],
    top_k=10,
    rerank=True,
)

# Class-A meta: returns results + coverage even if some NS fail (degradation path)
for item in result.results:
    print(item.score, item.namespace, item.content[:50])
print("coverage:", result.meta.coverage_ratio, "failed:", result.meta.namespaces_failed)
