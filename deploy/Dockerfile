# ==========================================================
# Cortrix — Semantic Storage for the Agentic Era
# Multi-stage Dockerfile
#
# Build:  docker build -t cortrix:quickstart -f deploy/Dockerfile ..
# Run:    use deploy/docker-compose.yml for the pinned-model QuickStart
# ==========================================================

ARG CORTRIX_SOURCE_REVISION=local-source

# ========== Stage 1: C++ Builder ==========
FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    wget \
    curl \
    pkg-config \
    libssl-dev \
    libsqlite3-dev \
    libpq-dev \
    python3.10 \
    python3.10-dev \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# cmake >= 3.24 required: Dependencies.cmake uses DOWNLOAD_EXTRACT_TIMESTAMP
# (jammy's apt cmake is 3.22, which mis-parses that keyword as a URL entry)
RUN pip3 install --no-cache-dir "cmake>=3.27"

WORKDIR /build

# Copy CMake infrastructure first for better layer caching
COPY cmake          cortrix/cmake/
COPY CMakeLists.txt cortrix/CMakeLists.txt
COPY VERSION        cortrix/VERSION

# Copy cortrix OSS source (tests/ required: root CMakeLists add_subdirectory
# is unconditional; BUILD_TESTS=OFF only skips the targets)
COPY include     cortrix/include/
COPY src         cortrix/src/
COPY tests       cortrix/tests/

# Build cortrix-server. Configure downloads are retried in a fresh build tree
# because upstream TLS connections can terminate mid-transfer.
RUN attempt=1; \
    while true; do \
        rm -rf build && mkdir -p build; \
        if cd build && cmake -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_TESTS=OFF ../cortrix; then \
            cd /build; \
            break; \
        fi; \
        cd /build; \
        if [ "$attempt" -ge 3 ]; then \
            echo "ERROR: CMake configure failed after $attempt attempts"; \
            exit 1; \
        fi; \
        echo "WARN: CMake configure attempt $attempt/3 failed; retrying"; \
        attempt=$((attempt + 1)); \
        sleep 3; \
    done && \
    cmake --build build --parallel "$(nproc)" --target cortrix-server && \
    strip build/cortrix-server

# Pre-build Python wheels for runtime (cortrix-agent lives at repo top level).
# PIP_INDEX_URL is overridable per build (e.g. a regional mirror when the
# default route to pypi is flaky); generous timeout/retries either way.
ARG PIP_INDEX_URL=https://pypi.org/simple
ENV PIP_INDEX_URL=${PIP_INDEX_URL} \
    PIP_TIMEOUT=120 \
    PIP_RETRIES=5
# No existence guard: requirements.lock is always expected, and a silent skip
# here ships an image whose cortrix-agent crash-loops (missing uvicorn).
# Python sources are COPYed after the C++ build on purpose: they take no part
# in the cmake step, and copying them earlier invalidated the (expensive)
# compile layer on every Python-side change.
# sdk/python is the Python SDK — cortrix-agent imports it as a sibling package
# (main.py resolves ../sdk/python), so it ships as source, not a wheel.
COPY cortrix-agent  cortrix/cortrix-agent/
COPY sdk/python     cortrix/sdk/python/
RUN mkdir -p /wheels && \
    pip3 wheel --no-cache-dir --require-hashes --wheel-dir /wheels \
        -r cortrix/cortrix-agent/requirements.lock

# ========== Stage 2: Web UI Builder ==========
# node 20: react-router-dom@7 requires >=20. NPM_REGISTRY overridable per
# build (regional mirror when the default route is flaky).
FROM node:20-slim AS web-builder

ARG NPM_REGISTRY=https://registry.npmjs.org

WORKDIR /build

# Copy web UI source (repo dir is web/; vite outDir is `static`, normalize to dist)
COPY web/ ./web-ui/

RUN if [ -f "web-ui/package.json" ]; then \
        cd web-ui && \
        npm config set registry ${NPM_REGISTRY} && \
        npm ci --ignore-scripts --fetch-retries 5 --fetch-retry-maxtimeout 120000 && \
        npm run build && \
        rm -rf dist && mv static dist; \
    else \
        mkdir -p web-ui/dist && \
        echo '<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">' \
             '<meta name="viewport" content="width=device-width,initial-scale=1.0">' \
             '<title>Cortrix</title>' \
             '<style>body{font-family:system-ui;display:flex;justify-content:center;' \
             'align-items:center;min-height:100vh;margin:0;background:#0a0a0a;color:#e0e0e0}' \
             '.c{text-align:center}h1{font-size:2.5rem;margin-bottom:.5rem}' \
             'p{color:#888;font-size:1.1rem}a{color:#6cb4ee}</style></head>' \
             '<body><div class="c"><h1>Cortrix</h1>' \
             '<p>Semantic Storage for the Agentic Era</p>' \
             '<p style="margin-top:2rem"><a href="/api/v1/health">/api/v1/health</a>' \
             ' &middot; <a href="/api/v1/namespaces">/api/v1/namespaces</a></p>' \
             '</div></body></html>' > web-ui/dist/index.html; \
    fi

# Bundle the OpenAPI spec (resolve $refs from api/openapi.yaml + components/* +
# paths/*) into a single self-contained spec the runtime serves at /openapi.json
# and /docs (Swagger UI). Without this the spec files are absent from the image,
# /openapi.json 404s, and the error handler rewrites it to the SPA — so Swagger
# UI and Agent API-discovery silently break. (R10 C-M1; SoT scripts/bundle_openapi.sh)
COPY api/ ./api/
RUN npx --yes @redocly/cli@1 bundle api/openapi.yaml -o openapi.bundled.yaml --ext yaml

# ========== Stage 3: Runtime ==========
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

LABEL maintainer="Cortrix Team <team@cortrix.ai>"
LABEL description="Cortrix — Semantic Storage for the Agentic Era"
LABEL version="1.0.0-rc.1"

# Runtime dependencies only — no compilers.
# curl + python3.10-venv are needed by the first-run provisioning in
# entrypoint.sh (download-models.sh fetches ONNX assets; the full profile creates
# for the docling/paddleocr parser stack). tar is in the base image.
# libgl1 / libglib2.0-0 / libgomp1: paddleocr imports opencv (libGL.so.1) and
# paddlepaddle needs OpenMP — without them the provisioned parser venv installs
# but fails to import at runtime, breaking PDF/image OCR + the vision_llm path.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libsqlite3-0 \
    libpq5 \
    python3.10 \
    python3-pip \
    python3.10-venv \
    libgl1 \
    libglib2.0-0 \
    libgomp1 \
    supervisor \
    curl \
    wget \
    ca-certificates \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

ARG CORTRIX_SOURCE_REVISION
LABEL org.opencontainers.image.source="https://github.com/cortrix/cortrix"
LABEL org.opencontainers.image.revision="${CORTRIX_SOURCE_REVISION}"
ENV CORTRIX_SOURCE_REVISION="${CORTRIX_SOURCE_REVISION}"

# Non-root user
RUN useradd -m -u 1000 -s /bin/bash cortrix

# Directory layout
RUN mkdir -p /app /app/config /app/web-ui /app/python /data /data/blobs /data/p-hnsw /data/logs && \
    chown -R cortrix:cortrix /app /data

WORKDIR /app

# --- Copy artifacts from builder stages ---

# C++ server binary + the ONNX Runtime shared lib it links against
# (FetchContent ships a prebuilt .so; without it the server exits 127)
COPY --from=builder /build/build/cortrix-server /app/cortrix_server
COPY --from=builder /build/build/_deps/onnxruntime-src/lib/libonnxruntime.so* /usr/local/lib/
RUN ldconfig

# Default YAML config + readiness log-sanitizer rules (main.cpp reads the fixed
# path /app/config/sensitive_fields.yaml)
COPY deploy/cortrix.yaml              /app/config/cortrix.yaml
COPY src/logging/sensitive_fields.yaml /app/config/sensitive_fields.yaml

# Web UI static files
COPY --from=web-builder /build/web-ui/dist /app/web-ui

# Bundled OpenAPI spec — the server (WORKDIR /app, OpenApiRoot()=".") serves it
# from /app/build/openapi.bundled.yaml at /openapi.json + /docs. (R10 C-M1)
COPY --from=web-builder /build/openapi.bundled.yaml /app/build/openapi.bundled.yaml

# Python code + pre-built wheels (image layout keeps /app/python/cortrix-agent
# with the Python SDK as its sibling at /app/python/sdk/python — main.py resolves
# parents[1]/sdk/python; supervisord.conf depends on this layout)
COPY --from=builder /build/cortrix/cortrix-agent /app/python/cortrix-agent
COPY --from=builder /build/cortrix/sdk/python    /app/python/sdk/python
COPY --from=builder /wheels /tmp/wheels

# Parser bridge scripts (auto-located at /app/scripts by the server).
# txt/md parse via the bridge's plain-text fast path with zero extra deps;
# the heavy docling/paddleocr stacks are installed on first run by
# entrypoint.sh into a venv on the /data volume (PROFILE=full default),
# from /app/scripts/requirements-parser.txt.
COPY scripts/docling_bridge.py \
     scripts/paddleocr_bridge.py \
     scripts/requirements-parser.txt /app/scripts/

# Model fetcher + immutable manifest. Weights remain outside Git and are
# downloaded from exact revisions, verified, then atomically installed.
COPY deploy/download-models.sh /app/scripts/download-models.sh
COPY deploy/model-manifest.tsv /app/config/model-manifest.tsv

# Source-backed QuickStart bootstrap and its project-authored fixture.
COPY deploy/quickstart-bootstrap.sh /app/scripts/quickstart-bootstrap.sh
COPY deploy/fixtures/quickstart-demo.txt /app/fixtures/quickstart-demo.txt
RUN chmod +x /app/scripts/download-models.sh /app/scripts/quickstart-bootstrap.sh

# Install Python deps from wheels (fast, no compilation)
# Fail the build loudly if the cortrix-agent deps cannot install — a silent
# failure here ships an image whose cortrix-agent crash-loops at startup.
RUN pip3 install --no-cache-dir /tmp/wheels/*.whl && rm -rf /tmp/wheels

# --- Deploy-time config files ---
COPY deploy/supervisord.conf  /etc/supervisor/conf.d/cortrix.conf
COPY deploy/entrypoint.sh     /app/entrypoint.sh
COPY deploy/healthcheck.sh    /app/healthcheck.sh

RUN chmod +x /app/entrypoint.sh /app/healthcheck.sh && \
    chown -R cortrix:cortrix /app

# --- Runtime settings ---
USER cortrix
EXPOSE 8420
VOLUME ["/data"]

HEALTHCHECK --interval=5s --timeout=5s --start-period=30m --retries=3 \
    CMD /app/healthcheck.sh

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["start"]
