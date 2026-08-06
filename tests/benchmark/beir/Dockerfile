# =============================================================
# Test suite W2: BEIR benchmark container (design: test-suite-tests.md)
# Owned by Benchmark Suite (D-R3 #481 cloud track); test suite provides
# the integration point only — Release Gate triggers benchmark-benchmark.yml
# after PASS (topic 5 coordination timing).
# =============================================================
FROM python:3.10-slim

RUN pip install --no-cache-dir beir requests

WORKDIR /bench
# Benchmark lands run_beir.py + dataset pinning here (scifact/nfcorpus first).
COPY run_beir.py* ./

ENTRYPOINT ["python", "run_beir.py"]
