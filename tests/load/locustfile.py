# =============================================================
# Test suite W3: Locust load profile (design: test-suite-tests.md)
# Nightly: locust -f tests/load/locustfile.py --headless -u 100 -r 10
#          --run-time 5m --host http://localhost:8420
#
# Exercises the agent-facing hot paths: health, doc upload, query,
# memory read. P95 is compared against the META block baseline out-of-band
# (soft gate ±10% — alert, don't block).
# =============================================================
import random
import string

from locust import HttpUser, between, task

NS = "loadtest"


def _rand_text(words: int = 80) -> str:
    return " ".join(
        "".join(random.choices(string.ascii_lowercase, k=random.randint(3, 10)))
        for _ in range(words)
    )


class CortrixAgentUser(HttpUser):
    """Synthetic agent traffic over the public HTTP API."""

    wait_time = between(0.5, 2.0)

    def on_start(self):
        # Idempotent namespace bootstrap; 409 (exists) is fine.
        self.client.post(
            "/api/v1/namespaces",
            json={"name": NS},
            name="POST /namespaces (bootstrap)",
        )

    @task(4)
    def health(self):
        self.client.get("/api/v1/health", name="GET /health")

    @task(2)
    def upload_doc(self):
        body = _rand_text()
        self.client.post(
            f"/api/v1/namespaces/{NS}/documents",
            files={"file": ("load.txt", body, "text/plain")},
            name="POST /documents",
        )

    @task(3)
    def query(self):
        # Global query endpoint; namespace rides in the body (cross-NS query contract)
        self.client.post(
            "/api/v1/query",
            json={"query": _rand_text(6), "namespace": NS, "top_k": 5},
            name="POST /query",
        )

    @task(1)
    def readiness(self):
        # Deployment readiness endpoint (ReadinessRegistry aggregate)
        self.client.get("/api/v1/system/health/ready", name="GET /system/health/ready")
