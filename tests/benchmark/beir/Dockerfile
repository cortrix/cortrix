# =============================================================
# F23 W2: BEIR benchmark container (design: F23-tests.md §3.2)
# Owned by F44 Benchmark Suite (D-R3 #481 cloud track); F23 provides
# the integration point only — Release Gate triggers f44-benchmark.yml
# after D4 PASS (topic 5 coordination timing).
# =============================================================
FROM python:3.10-slim

RUN pip install --no-cache-dir beir requests

WORKDIR /bench
# F44 lands run_beir.py + dataset pinning here (scifact/nfcorpus first).
COPY run_beir.py* ./

ENTRYPOINT ["python", "run_beir.py"]
