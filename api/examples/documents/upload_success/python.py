"""POST /api/v1/documents — success (Python SDK, async upload)."""
from cortrix import Client

client = Client(api_key="cx_live_xxx")

task = client.documents.upload_async(
    namespace="contracts",
    filename="contract_001.pdf",
    content="...",
    metadata={"source": "legal_dept", "tags": ["contract", "2026"]},
)
print("task:", task.task_id, task.status)

# Poll progress
progress = client.tasks.status(task.task_id)
print(progress.progress.stage, progress.progress.chunks_done, "/", progress.progress.chunks_total)
